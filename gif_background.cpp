// ReShade 6.x addon: ReShade menu pencerelerinin arkasina animasyonlu GIF cizer.
// GIF yolu: <addon dosyasinin klasoru>\reshade-gif\background.gif
// Gerekli: reshade SDK (reshade/include, reshade/deps/imgui) ve stb_image.h

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID ImU64

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <reshade.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_GIF
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "GIF Background";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "ReShade menu arka planina animasyonlu GIF ekler.";

static HMODULE g_module = nullptr;

struct gif_state
{
	device *dev = nullptr;
	std::vector<resource> tex;
	std::vector<resource_view> srv;
	std::vector<int> delay_ms;
	int total_ms = 0;
	int w = 0, h = 0;
	bool loaded = false;
	bool failed = false;
	bool enabled = true;
	bool fullscreen = false;     // false: sadece ReShade pencerelerinin icinde
	float opacity = 0.8f;        // GIF opakligi
	float window_alpha = 0.35f;  // ReShade pencere arka plan opakligi (dusuk = GIF daha cok gorunur)
	float speed = 1.0f;
	std::string status = "Henuz yuklenmedi";
} g;

static float g_orig_window_alpha = -1.0f;

static std::string gif_path()
{
	char buf[MAX_PATH] = {};
	GetModuleFileNameA(g_module, buf, MAX_PATH);
	std::string p(buf);
	p = p.substr(0, p.find_last_of("\\/") + 1);
	return p + "reshade-gif\\background.gif";
}

static void free_gif()
{
	if (g.dev != nullptr)
	{
		for (auto v : g.srv) if (v.handle) g.dev->destroy_resource_view(v);
		for (auto t : g.tex) if (t.handle) g.dev->destroy_resource(t);
	}
	g.srv.clear();
	g.tex.clear();
	g.delay_ms.clear();
	g.total_ms = 0;
	g.loaded = false;
	g.failed = false;
	g.dev = nullptr;
	g.status = "Henuz yuklenmedi";
}

static void load_gif(device *dev)
{
	const std::string path = gif_path();
	std::ifstream f(path, std::ios::binary);
	if (!f)
	{
		g.failed = true;
		g.status = "GIF bulunamadi: " + path;
		return;
	}
	std::vector<unsigned char> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	int *delays = nullptr;
	int w = 0, h = 0, frames = 0, comp = 0;
	unsigned char *pixels = stbi_load_gif_from_memory(file.data(), (int)file.size(), &delays, &w, &h, &frames, &comp, 4);
	if (pixels == nullptr || frames <= 0)
	{
		g.failed = true;
		g.status = "GIF okunamadi (bozuk dosya?)";
		return;
	}

	g.dev = dev;
	g.w = w;
	g.h = h;
	const size_t frame_bytes = (size_t)w * h * 4;

	for (int i = 0; i < frames; ++i)
	{
		resource_desc desc(w, h, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::gpu_only,
			resource_usage::shader_resource | resource_usage::copy_dest);
		subresource_data data;
		data.data = pixels + frame_bytes * i;
		data.row_pitch = w * 4;
		data.slice_pitch = (uint32_t)frame_bytes;

		resource tex = {};
		resource_view view = {};
		if (!dev->create_resource(desc, &data, resource_usage::shader_resource, &tex) ||
			!dev->create_resource_view(tex, resource_usage::shader_resource,
				resource_view_desc(format::r8g8b8a8_unorm), &view))
		{
			if (tex.handle) dev->destroy_resource(tex);
			free_gif();
			g.failed = true;
			g.status = "Texture olusturulamadi (GIF cok buyuk olabilir)";
			stbi_image_free(pixels);
			stbi_image_free(delays);
			return;
		}
		int d = delays ? delays[i] : 100;
		if (d <= 0) d = 100;
		g.tex.push_back(tex);
		g.srv.push_back(view);
		g.delay_ms.push_back(d);
		g.total_ms += d;
	}

	stbi_image_free(pixels);
	stbi_image_free(delays);
	g.loaded = true;
	g.status = "Yuklendi: " + std::to_string(frames) + " kare, " + std::to_string(w) + "x" + std::to_string(h);
}

// GetCurrentContext() addon'lara acik degil; context'e GetIO() uzerinden ulasiyoruz
// (IO, ImGuiContext'in bir uyesi oldugu icin adresten geri hesaplanir).
static ImGuiContext *get_ctx()
{
	ImGuiContext *ctx = (ImGuiContext *)((char *)&ImGui::GetIO() - offsetof(ImGuiContext, IO));
	if (!ctx->Initialized || ctx->Windows.Size <= 0 || ctx->Windows.Size > 512 || ctx->Windows.Data == nullptr)
		return nullptr;
	return ctx;
}

// ReShade pencere arka planini gerekirse seffaflastir / eski haline dondur
static void update_window_alpha()
{
	ImGuiStyle &style = ImGui::GetStyle();
	if (g.enabled && !g.fullscreen)
	{
		if (g_orig_window_alpha < 0.0f)
			g_orig_window_alpha = style.Colors[ImGuiCol_WindowBg].w;
		style.Colors[ImGuiCol_WindowBg].w = g.window_alpha;
	}
	else if (g_orig_window_alpha >= 0.0f)
	{
		style.Colors[ImGuiCol_WindowBg].w = g_orig_window_alpha;
		g_orig_window_alpha = -1.0f;
	}
}

// Overlay cizilirken her karede cagrilir
static void draw_background(effect_runtime *runtime)
{
	update_window_alpha();

	if (!g.enabled)
		return;
	if (!g.loaded && !g.failed)
		load_gif(runtime->get_device());
	if (!g.loaded)
		return;

	const double t_ms = std::fmod(ImGui::GetTime() * 1000.0 * g.speed, (double)g.total_ms);
	size_t frame = 0;
	double acc = 0.0;
	for (size_t i = 0; i < g.delay_ms.size(); ++i)
	{
		acc += g.delay_ms[i];
		if (t_ms < acc) { frame = i; break; }
	}

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	// "Cover": ekrani doldur, tasan kismi kirp
	const float scale = std::max(screen.x / g.w, screen.y / g.h);
	const float fu = screen.x / (g.w * scale);
	const float fv = screen.y / (g.h * scale);
	const ImVec2 uv0(0.5f - 0.5f * fu, 0.5f - 0.5f * fv);
	const ImVec2 uv1(0.5f + 0.5f * fu, 0.5f + 0.5f * fv);
	const ImU32 tint = IM_COL32(255, 255, 255, (int)(std::clamp(g.opacity, 0.0f, 1.0f) * 255.0f));
	const ImTextureID tex = (ImTextureID)g.srv[frame].handle;

	ImDrawList *dl = ImGui::GetBackgroundDrawList();

	if (g.fullscreen)
	{
		dl->AddImage(tex, ImVec2(0, 0), screen, uv0, uv1, tint);
		return;
	}

	// Sadece ReShade pencerelerinin icine ciz
	ImGuiContext *ctx = get_ctx();
	if (ctx == nullptr)
		return;

	for (int i = 0; i < ctx->Windows.Size; ++i)
	{
		ImGuiWindow *win = ctx->Windows[i];
		if (win == nullptr || win->Hidden || !(win->Active || win->WasActive))
			continue;
		if (win->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal))
			continue;
		// Tum ekrani kaplayan (dock/host) pencereleri atla
		if (win->Size.x >= screen.x * 0.95f && win->Size.y >= screen.y * 0.95f)
			continue;
		if (win->Size.x < 8.0f || win->Size.y < 8.0f)
			continue;

		dl->PushClipRect(win->Pos, ImVec2(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y), true);
		dl->AddImage(tex, ImVec2(0, 0), screen, uv0, uv1, tint);
		dl->PopClipRect();
	}
}

// Overlay'deki ayar penceresi
static void draw_settings(effect_runtime *runtime)
{
	if (!g.loaded && !g.failed)
		load_gif(runtime->get_device());

	ImGui::Checkbox("GIF arka plani goster", &g.enabled);
	ImGui::Checkbox("Tum ekrana ciz (kapaliysa sadece ReShade pencereleri)", &g.fullscreen);
	ImGui::SliderFloat("GIF opakligi", &g.opacity, 0.0f, 1.0f);
	ImGui::SliderFloat("Pencere seffafligi", &g.window_alpha, 0.0f, 1.0f);
	ImGui::SliderFloat("Hiz", &g.speed, 0.1f, 3.0f);
	if (ImGui::Button("GIF'i yeniden yukle"))
		free_gif();
	ImGui::TextWrapped("%s", g.status.c_str());
	ImGui::TextWrapped("Dosya: %s", gif_path().c_str());

	if (ImGui::CollapsingHeader("Teshis (pencere listesi)"))
	{
		ImGuiContext *ctx = get_ctx();
		if (ctx == nullptr)
		{
			ImGui::TextUnformatted("Context okunamadi.");
		}
		else
		{
			ImGui::Text("Pencere sayisi: %d", ctx->Windows.Size);
			for (int i = 0; i < ctx->Windows.Size; ++i)
			{
				ImGuiWindow *w = ctx->Windows[i];
				if (w == nullptr)
					continue;
				ImGui::Text("%s | %.0f,%.0f %.0fx%.0f | act=%d was=%d hid=%d fl=0x%X",
					w->Name ? w->Name : "?", w->Pos.x, w->Pos.y, w->Size.x, w->Size.y,
					(int)w->Active, (int)w->WasActive, (int)w->Hidden, (unsigned)w->Flags);
			}
		}
	}
}

static void on_destroy_device(device *dev)
{
	if (g.dev == dev)
		free_gif();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		g_module = hModule;
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::register_event<reshade::addon_event::reshade_overlay>(draw_background);
		reshade::register_overlay("GIF Background", draw_settings);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}
	return TRUE;
}
