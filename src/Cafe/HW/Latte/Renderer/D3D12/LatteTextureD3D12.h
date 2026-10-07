#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Format.h"

class D3D12Renderer;

class LatteTextureD3D12 : public LatteTexture
{
public:
	LatteTextureD3D12(D3D12Renderer* renderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
		uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth);
	~LatteTextureD3D12() override;

	void AllocateOnHost() override;

	ID3D12Resource* GetResource() const { return m_resource.Get(); }
	const D3D12_RESOURCE_DESC& GetDesc() const { return m_desc; }
	const D3D12TextureFormatInfo& GetFormatInfo() const { return m_formatInfo; }
	bool IsAlternateFormat() const { return m_formatInfo.isAlternateFormat; }
	bool IsCompressedFormat() const { return m_formatInfo.isCompressed; }
	bool Is3DTexture() const { return m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D; }

	uint32 GetMipCount() const { return m_desc.MipLevels; }
	uint32 GetArraySize() const { return Is3DTexture() ? 1 : m_desc.DepthOrArraySize; }
	uint32 GetPlaneCount() const { return m_planeCount; }
	uint32 GetSubresourceIndex(uint32 mip, uint32 slice, uint32 plane = 0) const
	{
		return D3D12_CalcSubresource(mip, Is3DTexture() ? 0 : slice, plane, m_desc.MipLevels, GetArraySize());
	}
	uint32 GetSubresourceCount() const { return m_desc.MipLevels * GetArraySize() * m_planeCount; }

	// resource state tracking (legacy barriers). One entry per subresource
	D3D12_RESOURCE_STATES GetSubresourceState(uint32 subresource) const { return m_states[subresource]; }
	void SetSubresourceState(uint32 subresource, D3D12_RESOURCE_STATES state) { m_states[subresource] = state; }
	bool AllSubresourcesInState(D3D12_RESOURCE_STATES state) const;
	void SetAllSubresourceStates(D3D12_RESOURCE_STATES state);

	// the D3D12 state textures are kept in when they are not being written to
	static constexpr D3D12_RESOURCE_STATES kShaderReadState = (D3D12_RESOURCE_STATES)((UINT)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | (UINT)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;

private:
	D3D12Renderer* m_renderer;
	ComPtr<ID3D12Resource> m_resource;
	D3D12_RESOURCE_DESC m_desc{};
	D3D12TextureFormatInfo m_formatInfo;
	uint32 m_planeCount = 1;
	std::vector<D3D12_RESOURCE_STATES> m_states;
};
