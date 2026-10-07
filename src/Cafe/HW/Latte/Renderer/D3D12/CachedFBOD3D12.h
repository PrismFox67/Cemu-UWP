#pragma once

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

class D3D12Renderer;
class LatteTextureViewD3D12;

// Set of render target views that are bound together. Unlike Vulkan there are no render pass or framebuffer objects in
// D3D12, so this only caches the RTV/DSV handles and the formats which become part of the pipeline state.
class CachedFBOD3D12 : public LatteCachedFBO
{
public:
	CachedFBOD3D12(D3D12Renderer* renderer, uint64 key);
	~CachedFBOD3D12() override;

	uint32 GetNumRTVs() const { return m_numRTVs; } // highest used color slot + 1
	const D3D12_CPU_DESCRIPTOR_HANDLE* GetRTVs() const { return m_rtvs; }
	bool HasDSV() const { return m_hasDSV; }
	D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const { return m_dsv; }

	DXGI_FORMAT GetRTVFormat(uint32 index) const { return m_rtvFormats[index]; }
	DXGI_FORMAT GetDSVFormat() const { return m_dsvFormat; }
	bool IsColorSlotInteger(uint32 index) const { return m_isInteger[index]; }

	LatteTextureViewD3D12* GetColorView(uint32 index) const;
	LatteTextureViewD3D12* GetDepthView() const;

	// hash of all formats, used as part of the pipeline key
	uint64 GetFormatHash() const { return m_formatHash; }

private:
	D3D12Renderer* m_renderer;
	uint32 m_numRTVs = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE m_rtvs[8]{};
	DXGI_FORMAT m_rtvFormats[8]{};
	bool m_isInteger[8]{};
	bool m_hasDSV = false;
	D3D12_CPU_DESCRIPTOR_HANDLE m_dsv{};
	DXGI_FORMAT m_dsvFormat = DXGI_FORMAT_UNKNOWN;
	uint64 m_formatHash = 0;
};
