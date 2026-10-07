#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"

CachedFBOD3D12::CachedFBOD3D12(D3D12Renderer* renderer, uint64 key)
	: LatteCachedFBO(key), m_renderer(renderer)
{
	uint64 h = 0xcbf29ce484222325ull;
	auto mix = [&h](uint64 v) {
		h ^= v;
		h *= 0x100000001B3ull;
	};
	for (uint32 i = 0; i < 8; i++)
	{
		auto* view = (LatteTextureViewD3D12*)colorBuffer[i].texture;
		if (!view)
		{
			m_rtvs[i] = renderer->GetNullRTV();
			m_rtvFormats[i] = DXGI_FORMAT_UNKNOWN;
			mix(0);
			continue;
		}
		if (view->GetRTVFormat() == DXGI_FORMAT_UNKNOWN)
		{
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Texture format {:04x} cannot be used as a render target", (uint32)view->format);
			m_rtvs[i] = renderer->GetNullRTV();
			m_rtvFormats[i] = DXGI_FORMAT_UNKNOWN;
			mix(0);
			continue;
		}
		m_rtvs[i] = view->GetRTV();
		m_rtvFormats[i] = view->GetRTVFormat();
		m_isInteger[i] = view->GetBaseTexture()->GetFormatInfo().isInteger;
		m_numRTVs = i + 1;
		mix((uint64)m_rtvFormats[i] + 1);
	}
	auto* depthView = (LatteTextureViewD3D12*)depthBuffer.texture;
	if (depthView)
	{
		m_hasDSV = true;
		m_dsv = depthView->GetDSV();
		m_dsvFormat = depthView->GetDSVFormat();
	}
	mix((uint64)m_dsvFormat + 0x1000);
	m_formatHash = h;
}

CachedFBOD3D12::~CachedFBOD3D12()
{
	m_renderer->NotifyFBORelease(this);
}

LatteTextureViewD3D12* CachedFBOD3D12::GetColorView(uint32 index) const
{
	return (LatteTextureViewD3D12*)colorBuffer[index].texture;
}

LatteTextureViewD3D12* CachedFBOD3D12::GetDepthView() const
{
	return (LatteTextureViewD3D12*)depthBuffer.texture;
}
