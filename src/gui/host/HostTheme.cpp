#include "gui/host/HostTheme.h"

#include "config/ActiveSettings.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace
{
	std::mutex s_mutex;
	std::vector<uint8> s_imageRgba; // the user's background, decoded once
	int s_imageWidth = 0;
	int s_imageHeight = 0;
	std::atomic_bool s_imageReady = false;
	bool s_lightPanel = true;
	const auto s_start = std::chrono::steady_clock::now();
	constexpr uint64 kBackgroundTextureKey = 0x7468656D65424721ull; // not a title ID

	fs::path _ThemeFolder()
	{
		return ActiveSettings::GetUserDataPath("theme");
	}

	// PNG/JPG -> RGBA8 with WIC (part of Windows and allowed in UWP apps)
	bool _DecodeImage(const fs::path& path, std::vector<uint8>& rgba, int& width, int& height)
	{
		using Microsoft::WRL::ComPtr;
		const bool coInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
		bool ok = false;
		{
			ComPtr<IWICImagingFactory> factory;
			ComPtr<IWICBitmapDecoder> decoder;
			ComPtr<IWICBitmapFrameDecode> frame;
			ComPtr<IWICFormatConverter> converter;
			UINT w = 0, h = 0;
			if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
				SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
				SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
				SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
				SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0)
			{
				rgba.resize((size_t)w * h * 4);
				ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
				width = (int)w;
				height = (int)h;
			}
		}
		if (coInit)
			CoUninitialize();
		return ok;
	}

	// --- the default backdrop: our own icon shapes, white with a soft blue-grey bevel underneath ---
	enum class Shape { CircleA, CircleB, PillL, PillR, Cross, House, Wrench, Bubble, Key, Pad, Ring };

	void _DrawShape(ImDrawList* dl, Shape shape, ImVec2 c, float s, float angle, ImU32 col)
	{
		const float ca = std::cos(angle), sa = std::sin(angle);
		auto P = [&](float x, float y) { return ImVec2(c.x + (x * ca - y * sa) * s, c.y + (x * sa + y * ca) * s); };
		auto quad = [&](float x0, float y0, float x1, float y1) { dl->AddQuadFilled(P(x0, y0), P(x1, y0), P(x1, y1), P(x0, y1), col); };
		const float t = 0.16f; // stroke width relative to the size
		switch (shape)
		{
		case Shape::CircleA:
		case Shape::CircleB:
			dl->AddCircle(c, s * 0.5f, col, 24, s * t);
			// a letter-like mark: A = two strokes, B = bar with bumps
			if (shape == Shape::CircleA)
			{
				dl->AddLine(P(-0.17f, 0.22f), P(0.0f, -0.24f), col, s * 0.1f);
				dl->AddLine(P(0.17f, 0.22f), P(0.0f, -0.24f), col, s * 0.1f);
			}
			else
			{
				quad(-0.14f, -0.22f, -0.04f, 0.22f);
				dl->AddCircle(P(0.02f, -0.1f), s * 0.1f, col, 12, s * 0.08f);
				dl->AddCircle(P(0.03f, 0.1f), s * 0.12f, col, 12, s * 0.08f);
			}
			break;
		case Shape::PillL:
		case Shape::PillR:
		{
			// shoulder button: rounded outline with an L or R bar
			const ImVec2 pts[4] = { P(-0.55f, -0.3f), P(0.55f, -0.3f), P(0.55f, 0.3f), P(-0.55f, 0.3f) };
			dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, s * t);
			quad(-0.12f, -0.17f, -0.03f, 0.17f);
			if (shape == Shape::PillL)
				quad(-0.12f, 0.1f, 0.14f, 0.17f);
			else
				dl->AddCircle(P(0.03f, -0.06f), s * 0.09f, col, 10, s * 0.07f);
			break;
		}
		case Shape::Cross:
			quad(-0.14f, -0.45f, 0.14f, 0.45f);
			quad(-0.45f, -0.14f, 0.45f, 0.14f);
			break;
		case Shape::House:
			dl->AddTriangleFilled(P(-0.45f, -0.02f), P(0.0f, -0.45f), P(0.45f, -0.02f), col);
			quad(-0.3f, -0.05f, 0.3f, 0.4f);
			break;
		case Shape::Wrench:
			quad(-0.4f, -0.07f, 0.25f, 0.07f);
			dl->AddCircle(P(0.32f, 0.0f), s * 0.14f, col, 12, s * 0.1f);
			dl->AddCircleFilled(P(-0.4f, 0.0f), s * 0.1f, col, 10);
			break;
		case Shape::Bubble:
			dl->AddCircleFilled(c, s * 0.4f, col, 20);
			dl->AddTriangleFilled(P(-0.25f, 0.2f), P(-0.4f, 0.5f), P(0.0f, 0.32f), col);
			break;
		case Shape::Key:
			dl->AddCircle(P(-0.25f, 0.0f), s * 0.18f, col, 14, s * 0.1f);
			quad(-0.08f, -0.06f, 0.45f, 0.06f);
			quad(0.28f, 0.0f, 0.36f, 0.18f);
			break;
		case Shape::Pad:
		{
			const ImVec2 pts[4] = { P(-0.5f, -0.28f), P(0.5f, -0.28f), P(0.5f, 0.28f), P(-0.5f, 0.28f) };
			dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, s * t);
			quad(-0.28f, -0.14f, 0.28f, 0.14f);
			break;
		}
		case Shape::Ring:
			dl->AddCircle(c, s * 0.3f, col, 18, s * 0.12f);
			break;
		}
	}

	uint32 _Hash(uint32 x)
	{
		x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
		return x;
	}
	float _Rand(uint32 seed) { return (_Hash(seed) & 0xFFFFFF) / float(0x1000000); }

	void _DrawDefaultBackground(ImDrawList* dl, ImVec2 min, ImVec2 max)
	{
		const float w = max.x - min.x, h = max.y - min.y;
		// sky: vivid blue at the top to white at the bottom
		const ImU32 top = IM_COL32(8, 164, 236, 255), mid = IM_COL32(110, 200, 245, 255), bottom = IM_COL32(252, 254, 255, 255);
		dl->AddRectFilledMultiColor(min, ImVec2(max.x, min.y + h * 0.55f), top, top, mid, mid);
		dl->AddRectFilledMultiColor(ImVec2(min.x, min.y + h * 0.55f), max, mid, mid, bottom, bottom);

		// icons: a fixed scatter, denser near the top, drifting slowly right and down and looping
		const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_start).count();
		const float unit = h / 1080.0f;
		constexpr int kIcons = 140;
		for (int i = 0; i < kIcons; i++)
		{
			const float depth = 0.45f + 0.55f * _Rand(i * 7 + 1); // 1 = near: bigger, faster, more opaque
			float y = std::pow(_Rand(i * 7 + 2), 1.6f); // more icons at the top
			if (y > 0.92f)
				continue;
			const float speed = 6.0f * depth * unit; // px per second
			const float x = std::fmod(_Rand(i * 7 + 3) * (w + 200.0f * unit) + time * speed, w + 200.0f * unit) - 100.0f * unit;
			y = y * h + std::sin(time * 0.15f + i) * 6.0f * unit;
			const float size = (18.0f + 34.0f * depth * _Rand(i * 7 + 4)) * unit;
			const float angle = (_Rand(i * 7 + 5) - 0.5f) * 1.6f + std::sin(time * 0.1f + i * 1.7f) * 0.15f;
			const Shape shape = (Shape)(_Hash(i * 7 + 6) % 11);
			const float fade = 1.0f - std::max(0.0f, y / h - 0.55f) * 1.6f; // fainter towards the white bottom
			const ImVec2 c(min.x + x, min.y + y);
			// bevel: a slightly offset blue-grey copy under the white shape
			_DrawShape(dl, shape, ImVec2(c.x + size * 0.06f, c.y + size * 0.08f), size, angle, IM_COL32(70, 120, 170, (int)(70 * depth * fade)));
			_DrawShape(dl, shape, c, size, angle, IM_COL32(255, 255, 255, (int)(235 * (0.55f + 0.45f * depth) * fade)));
		}
	}
}

void HostTheme::Initialize()
{
	std::error_code ec;
	fs::create_directories(_ThemeFolder(), ec);
	{
		std::ifstream in(_ThemeFolder() / "theme.txt");
		std::string line;
		if (std::getline(in, line))
			s_lightPanel = line.rfind("dark", 0) != 0;
	}
	std::thread([]() {
		for (const char* name : { "background.png", "background.jpg", "background.jpeg" })
		{
			const fs::path path = _ThemeFolder() / name;
			std::error_code ec2;
			if (!fs::is_regular_file(path, ec2))
				continue;
			std::vector<uint8> rgba;
			int w = 0, h = 0;
			if (_DecodeImage(path, rgba, w, h))
			{
				std::scoped_lock lock(s_mutex);
				s_imageRgba = std::move(rgba);
				s_imageWidth = w;
				s_imageHeight = h;
				s_imageReady = true;
				cemuLog_log(LogType::Force, "Host: launcher background {} ({}x{})", _pathToUtf8(path), w, h);
			}
			else
				cemuLog_log(LogType::Force, "Host: couldn't read the launcher background {}", _pathToUtf8(path));
			break;
		}
	}).detach();
}

void HostTheme::DrawBackground(ImDrawList* drawList, ImVec2 min, ImVec2 max, void* (*getTexture)(uint64, const uint8*, int, int))
{
	if (s_imageReady && getTexture)
	{
		void* texture;
		int w, h;
		{
			std::scoped_lock lock(s_mutex);
			texture = getTexture(kBackgroundTextureKey, s_imageRgba.data(), s_imageWidth, s_imageHeight);
			w = s_imageWidth;
			h = s_imageHeight;
		}
		if (texture)
		{
			// cover: fill the screen, crop the longer side
			const float screenAspect = (max.x - min.x) / (max.y - min.y), imageAspect = (float)w / (float)h;
			ImVec2 uv0(0, 0), uv1(1, 1);
			if (imageAspect > screenAspect)
			{
				const float crop = (1.0f - screenAspect / imageAspect) * 0.5f;
				uv0.x = crop;
				uv1.x = 1.0f - crop;
			}
			else
			{
				const float crop = (1.0f - imageAspect / screenAspect) * 0.5f;
				uv0.y = crop;
				uv1.y = 1.0f - crop;
			}
			drawList->AddImage(texture, min, max, uv0, uv1);
			return;
		}
	}
	_DrawDefaultBackground(drawList, min, max);
}

bool HostTheme::IsLightPanel()
{
	return s_lightPanel;
}

void HostTheme::SetLightPanel(bool light)
{
	s_lightPanel = light;
	std::ofstream(_ThemeFolder() / "theme.txt", std::ios::trunc) << (light ? "light\n" : "dark\n");
}

ImU32 HostTheme::PanelFill() { return s_lightPanel ? IM_COL32(255, 255, 255, 200) : IM_COL32(12, 22, 40, 185); }
ImU32 HostTheme::PanelBorder() { return s_lightPanel ? IM_COL32(255, 255, 255, 240) : IM_COL32(120, 170, 230, 90); }
ImU32 HostTheme::PanelText() { return s_lightPanel ? IM_COL32(30, 40, 55, 255) : IM_COL32(240, 244, 250, 255); }
ImU32 HostTheme::PanelTextDim() { return s_lightPanel ? IM_COL32(95, 110, 130, 255) : IM_COL32(160, 175, 195, 255); }
ImU32 HostTheme::Accent() { return IM_COL32(0, 152, 230, 255); }
