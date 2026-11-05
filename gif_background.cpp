// ReShade 6.x addon: ReShade menusunun arkasina animasyonlu GIF cizer.
// GIF yolu: <addon dosyasinin klasoru>\reshade-gif\background.gif
// Gerekli: reshade SDK (reshade/include, reshade/deps/imgui) ve stb_image.h

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID ImU64

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <imgui.h>
#include <reshade.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_GIF
#include "stb_image.h"

#include <algorithm>
#include <cmath>
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
	bool fullscreen = false;     // false: sadece asagidaki dikdortgen (varsayilan: sol ReShade paneli)
	float opacity = 0.8f;        // GIF opakligi
	float window_alpha = 0.35f;  // ReShade pencere arka plan opakligi (dusuk = GIF daha cok gorunur)
	float speed = 1.0f;
	// Panel dikdortgeni (piksel). Yukseklik 0 = ekran yuksekligi
	int rect_x = 0;
	int rect_y = 0;
	int rect_w = 393;
	int rect_h = 0;
	std::string status = "Henuz yuklenmedi";
} g;

static float g_orig_window_alpha = -1.0f;
static bool g_overlay_open = false;

// ReShade menusu acildiginda / kapandiginda cagrilir
static bool on_open_overlay(effect_runtime *, bool open, input_source)
{
	g_overlay_open = open;
	return false; // islemi engelleme
}

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

// ReShade pencere arka planini seffaflastir / eski haline dondur
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
	// Menu kapaliyken hicbir sey cizme
	if (!g_overlay_open)
		return;

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

	// Cizilecek dikdortgen
	ImVec2 p0(0.0f, 0.0f), p1 = screen;
	if (!g.fullscreen)
	{
		p0 = ImVec2((float)g.rect_x, (float)g.rect_y);
		const float rw = (float)std::max(g.rect_w, 1);
		const float rh = g.rect_h > 0 ? (float)g.rect_h : screen.y - p0.y;
		p1 = ImVec2(p0.x + rw, p0.y + std::max(rh, 1.0f));
	}
	const float rect_w = p1.x - p0.x;
	const float rect_h = p1.y - p0.y;

	// "Cover": dikdortgeni doldur, tasan kismi kirp
	const float scale = std::max(rect_w / g.w, rect_h / g.h);
	const float fu = rect_w / (g.w * scale);
	const float fv = rect_h / (g.h * scale);
	const ImVec2 uv0(0.5f - 0.5f * fu, 0.5f - 0.5f * fv);
	const ImVec2 uv1(0.5f + 0.5f * fu, 0.5f + 0.5f * fv);
	const ImU32 tint = IM_COL32(255, 255, 255, (int)(std::clamp(g.opacity, 0.0f, 1.0f) * 255.0f));

	ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)g.srv[frame].handle, p0, p1, uv0, uv1, tint);
}

// Overlay'deki ayar penceresi
static void draw_settings(effect_runtime *runtime)
{
	if (!g.loaded && !g.failed)
		load_gif(runtime->get_device());

	ImGui::Checkbox("GIF arka plani goster", &g.enabled);
	ImGui::Checkbox("Tum ekrana ciz (kapaliysa sadece asagidaki panel alani)", &g.fullscreen);
	ImGui::SliderFloat("GIF opakligi", &g.opacity, 0.0f, 1.0f);
	ImGui::SliderFloat("Pencere seffafligi", &g.window_alpha, 0.0f, 1.0f);
	ImGui::SliderFloat("Hiz", &g.speed, 0.1f, 3.0f);

	ImGui::SeparatorText("Panel alani (piksel)");
	ImGui::DragInt("Sol (X)", &g.rect_x, 1.0f, 0, 4000);
	ImGui::DragInt("Ust (Y)", &g.rect_y, 1.0f, 0, 4000);
	ImGui::DragInt("Genislik", &g.rect_w, 1.0f, 1, 4000);
	ImGui::DragInt("Yukseklik (0 = tam)", &g.rect_h, 1.0f, 0, 4000);

	if (ImGui::Button("GIF'i yeniden yukle"))
		free_gif();
	ImGui::TextWrapped("%s", g.status.c_str());
	ImGui::TextWrapped("Dosya: %s", gif_path().c_str());
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
		reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_open_overlay);
		reshade::register_overlay("GIF Background", draw_settings);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}
	return TRUE;
}
