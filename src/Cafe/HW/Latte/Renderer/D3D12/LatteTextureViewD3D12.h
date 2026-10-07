#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

#include <unordered_map>

class D3D12Renderer;
class LatteTextureD3D12;

class LatteTextureViewD3D12 : public LatteTextureView
{
public:
	LatteTextureViewD3D12(D3D12Renderer* renderer, LatteTextureD3D12* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount);
	~LatteTextureViewD3D12() override;

	LatteTextureD3D12* GetBaseTexture() const { return m_baseTexture; }

	// staging (non shader-visible) SRV for the given GPU7 sampler swizzle (SQ_TEX_RESOURCE_WORD4)
	D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(uint32 gpuSamplerSwizzle);
	D3D12_CPU_DESCRIPTOR_HANDLE GetSRVRGBA() { return GetSRV(kRGBASwizzle); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetRTV();
	D3D12_CPU_DESCRIPTOR_HANDLE GetDSV();

	DXGI_FORMAT GetRTVFormat() const { return m_rtvFormat; }
	DXGI_FORMAT GetDSVFormat() const { return m_dsvFormat; }
	DXGI_FORMAT GetSRVFormat() const { return m_srvFormat; }

	// unique per view object, used for cache keys
	uint64 GetUniqueId() const { return m_uniqueId; }

	static constexpr uint32 kRGBASwizzle = 0x06880000;

private:
	D3D12_CPU_DESCRIPTOR_HANDLE CreateSRV(uint32 gpuSamplerSwizzle);

	D3D12Renderer* m_renderer;
	LatteTextureD3D12* m_baseTexture;
	DXGI_FORMAT m_srvFormat;
	DXGI_FORMAT m_rtvFormat;
	DXGI_FORMAT m_dsvFormat;
	uint64 m_uniqueId;

	// most views are only ever sampled with one or two swizzles
	struct SRVCacheEntry
	{
		uint32 swizzle = 0xFFFFFFFF;
		D3D12_CPU_DESCRIPTOR_HANDLE handle{};
	};
	SRVCacheEntry m_srvCache[2];
	std::unordered_map<uint32, D3D12_CPU_DESCRIPTOR_HANDLE> m_srvFallbackCache;
	D3D12_CPU_DESCRIPTOR_HANDLE m_rtv{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_dsv{};
};
