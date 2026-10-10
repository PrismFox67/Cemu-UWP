#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"

#include <mutex>
#include <unordered_set>

// identical to LatteTextureVk_AdjustTextureCompSel. GPU7 formats whose channel order differs from the host format
static uint32 _AdjustTextureCompSel(Latte::E_GX2SURFFMT format, uint32 compSel)
{
	switch (format)
	{
	case Latte::E_GX2SURFFMT::R8_UNORM: // R8 is replicated on all channels
	case Latte::E_GX2SURFFMT::R8_SNORM:
		if (compSel >= 1 && compSel <= 3)
			compSel = 0;
		break;
	case Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM: // order of components is reversed (RGBA -> ABGR)
		if (compSel <= 3)
			compSel = 3 - compSel;
		break;
	case Latte::E_GX2SURFFMT::BC4_UNORM:
	case Latte::E_GX2SURFFMT::BC4_SNORM:
		if (compSel >= 1 && compSel <= 3)
			compSel = 0;
		break;
	case Latte::E_GX2SURFFMT::BC5_UNORM:
	case Latte::E_GX2SURFFMT::BC5_SNORM:
		if (compSel == 3)
			compSel = 1; // read alpha as green
		break;
	case Latte::E_GX2SURFFMT::A2_B10_G10_R10_UNORM:
		if (compSel <= 3)
			compSel = 3 - compSel;
		break;
	case Latte::E_GX2SURFFMT::X24_G8_UINT:
		if (compSel <= 3)
			compSel = 3;
		break;
	case Latte::E_GX2SURFFMT::R4_G4_UNORM:
		if (compSel == 0)
			compSel = 1;
		else if (compSel == 1)
			compSel = 0;
		break;
	default:
		break;
	}
	return compSel;
}

static uint32 _AdjustSwizzle(Latte::E_GX2SURFFMT format, uint32 word4)
{
	uint32 r = _AdjustTextureCompSel(format, (word4 >> 16) & 7);
	uint32 g = _AdjustTextureCompSel(format, (word4 >> 19) & 7);
	uint32 b = _AdjustTextureCompSel(format, (word4 >> 22) & 7);
	uint32 a = _AdjustTextureCompSel(format, (word4 >> 25) & 7);
	return (r << 16) | (g << 19) | (b << 22) | (a << 25);
}

LatteTextureViewD3D12::LatteTextureViewD3D12(D3D12Renderer* renderer, LatteTextureD3D12* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format), m_renderer(renderer), m_baseTexture(texture)
{
	m_uniqueId = renderer->GenUniqueId();
	const D3D12TextureFormatInfo& baseInfo = texture->GetFormatInfo();
	m_srvFormat = baseInfo.srvFormat;
	m_rtvFormat = baseInfo.rtvFormat;
	m_dsvFormat = baseInfo.dsvFormat;
	if (!texture->overwriteInfo.hasFormatOverwrite && format != texture->format)
	{
		// reinterpret within the typeless family if possible
		const auto& support = renderer->GetFormatSupport();
		DXGI_FORMAT srv = D3D12Format::GetCompatibleViewFormat(support, texture->GetDesc().Format, format, texture->isDepth, false);
		if (srv != DXGI_FORMAT_UNKNOWN)
			m_srvFormat = srv;
		else
			cemuLog_logDebugOnce(LogType::Force, "D3D12: View format {:04x} is not compatible with texture format {:04x}, using the base format", (uint32)format, (uint32)texture->format);
		if (!texture->isDepth && m_rtvFormat != DXGI_FORMAT_UNKNOWN)
		{
			DXGI_FORMAT rtv = D3D12Format::GetCompatibleViewFormat(support, texture->GetDesc().Format, format, false, true);
			if (rtv != DXGI_FORMAT_UNKNOWN)
				m_rtvFormat = rtv;
		}
	}
	m_usesSliceCopy = (dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_MSAA) && firstSlice > 0 && !texture->Is3DTexture() && !texture->isDepth;

	// diagnostics for views that select array slices (MK8's button icons are still wrong on the Xbox): log each distinct
	// layout once
	if (firstSlice > 0 || sliceCount > 1 || texture->GetDesc().DepthOrArraySize > 1)
	{
		static std::mutex s_logMutex;
		static std::unordered_set<uint64> s_logged;
		const uint64 key = ((uint64)dim << 56) ^ ((uint64)format << 40) ^ ((uint64)std::min(firstSlice, 255) << 32) ^ ((uint64)std::min(sliceCount, 255) << 24) ^ ((uint64)texture->GetDesc().DepthOrArraySize << 8) ^ (uint64)texture->tileMode;
		std::lock_guard lock(s_logMutex);
		if (s_logged.size() < 64 && s_logged.insert(key).second)
			cemuLog_log(LogType::Force, "D3D12: view dim {} fmt {:04x} slices {}+{} mips {}+{} of {}x{}x{} (dim {} tm {}){}", (uint32)dim, (uint32)format, firstSlice, sliceCount, firstMip, mipCount,
				texture->width, texture->height, texture->depth, (uint32)texture->dim, (uint32)texture->tileMode, m_usesSliceCopy ? " slice copy" : "");
	}
}

void LatteTextureViewD3D12::UpdateSliceCopy()
{
	const uint64 version = std::max(m_baseTexture->lastUpdateEventCounter, m_baseTexture->lastWriteEventCounter);
	if (m_sliceCopy && version == m_sliceCopyVersion)
		return;
	ID3D12Resource* baseResource = m_baseTexture->GetResource();
	if (!baseResource)
		return;
	ID3D12Device* device = m_renderer->GetDevice();
	if (!m_sliceCopy)
	{
		D3D12_RESOURCE_DESC desc = m_baseTexture->GetDesc();
		desc.Width = (UINT64)std::max(1, m_baseTexture->GetMipWidth(firstMip));
		desc.Height = (UINT)std::max(1, m_baseTexture->GetMipHeight(firstMip));
		if (m_baseTexture->GetFormatInfo().isCompressed)
		{
			desc.Width = AlignUp<UINT64>(desc.Width, 4);
			desc.Height = AlignUp<UINT>(desc.Height, 4);
		}
		desc.DepthOrArraySize = 1;
		desc.MipLevels = (UINT16)std::max(1, numMip);
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_sliceCopy))))
		{
			cemuLog_logOnce(LogType::Force, "D3D12: Failed to create a texture slice copy, sampling the array view instead");
			m_usesSliceCopy = false;
			return;
		}
		D3D12_SetDebugName(m_sliceCopy.Get(), "TextureSliceCopy");
		m_sliceCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	m_renderer->TransitionResource(m_sliceCopy.Get(), m_sliceCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
	m_renderer->TransitionTexture(m_baseTexture, firstMip, numMip, firstSlice, 1, D3D12_RESOURCE_STATE_COPY_SOURCE);
	m_renderer->FlushBarriers();
	const D3D12_RESOURCE_DESC srcDesc = m_baseTexture->GetDesc();
	const D3D12_RESOURCE_DESC dstDesc = m_sliceCopy->GetDesc();
	for (sint32 m = 0; m < numMip; m++)
	{
		const UINT srcSub = m_baseTexture->GetSubresourceIndex(firstMip + m, firstSlice, 0);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT srcFp, dstFp;
		UINT rows;
		UINT64 rowSize, total;
		device->GetCopyableFootprints(&srcDesc, srcSub, 1, 0, &srcFp, &rows, &rowSize, &total);
		device->GetCopyableFootprints(&dstDesc, (UINT)m, 1, 0, &dstFp, &rows, &rowSize, &total);
		D3D12_BOX box{ 0, 0, 0, std::min(srcFp.Footprint.Width, dstFp.Footprint.Width), std::min(srcFp.Footprint.Height, dstFp.Footprint.Height), 1 };
		D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
		dst.pResource = m_sliceCopy.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = (UINT)m;
		src.pResource = baseResource;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = srcSub;
		m_renderer->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	}
	m_renderer->TransitionResource(m_sliceCopy.Get(), m_sliceCopyState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	m_renderer->m_hasRecordedWork = true;
	m_sliceCopyVersion = version;
}

LatteTextureViewD3D12::~LatteTextureViewD3D12()
{
	m_renderer->NotifyTextureViewRelease(this);
	auto& srvHeap = m_renderer->GetStagingViewHeap();
	for (auto& e : m_srvCache)
		if (e.handle.ptr)
			srvHeap.Free(e.handle);
	for (auto& it : m_srvFallbackCache)
		srvHeap.Free(it.second);
	if (m_rtv.ptr)
		m_renderer->GetStagingRTVHeap().Free(m_rtv);
	if (m_dsv.ptr)
		m_renderer->GetStagingDSVHeap().Free(m_dsv);
	if (m_sliceCopy)
		m_renderer->ReleaseResourceDeferred(std::move(m_sliceCopy));
}

D3D12_CPU_DESCRIPTOR_HANDLE LatteTextureViewD3D12::GetSRV(uint32 gpuSamplerSwizzle)
{
	if (m_usesSliceCopy)
		UpdateSliceCopy();
	gpuSamplerSwizzle &= 0x0FFF0000;
	for (auto& e : m_srvCache)
	{
		if (e.swizzle == gpuSamplerSwizzle)
			return e.handle;
	}
	for (auto& e : m_srvCache)
	{
		if (e.swizzle == 0xFFFFFFFF)
		{
			e.swizzle = gpuSamplerSwizzle;
			e.handle = CreateSRV(gpuSamplerSwizzle);
			return e.handle;
		}
	}
	auto it = m_srvFallbackCache.find(gpuSamplerSwizzle);
	if (it != m_srvFallbackCache.end())
		return it->second;
	D3D12_CPU_DESCRIPTOR_HANDLE h = CreateSRV(gpuSamplerSwizzle);
	m_srvFallbackCache.emplace(gpuSamplerSwizzle, h);
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE LatteTextureViewD3D12::CreateSRV(uint32 gpuSamplerSwizzle)
{
	// The SRV dimension has to match the sampler type in the shader. Cemu's GLSL emitter declares:
	//   DIM_1D -> sampler1D, DIM_2D/DIM_2D_MSAA -> sampler2D, DIM_2D_ARRAY -> sampler2DArray,
	//   DIM_CUBEMAP -> samplerCubeArray, DIM_3D -> sampler3D
	// and the shader's texture dimension is taken from the bound view, so we derive the SRV dimension from the view.
	// D3D12's Texture1D/Texture2D SRVs cannot select an array slice, unlike Vulkan's 1D/2D image views. For views that
	// start at a slice other than 0 we fall back to the array SRV form with a single slice. This is technically a
	// dimension mismatch but is handled consistently by all D3D12 drivers we are aware of.
	D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
	desc.Format = m_srvFormat;
	desc.Shader4ComponentMapping = D3D12Format::GetComponentMapping(_AdjustSwizzle(format, gpuSamplerSwizzle));
	if (m_usesSliceCopy && m_sliceCopy)
	{
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MostDetailedMip = 0;
		desc.Texture2D.MipLevels = (UINT)std::max(1, numMip);
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_renderer->GetStagingViewHeap().Allocate();
		m_renderer->GetDevice()->CreateShaderResourceView(m_sliceCopy.Get(), &desc, h);
		return h;
	}
	const bool baseIs3D = m_baseTexture->Is3DTexture();
	if (baseIs3D)
	{
		if (dim != Latte::E_DIM::DIM_3D)
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Non-3D view of a 3D texture is not supported, binding the 3D texture instead");
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MostDetailedMip = firstMip;
		desc.Texture3D.MipLevels = numMip;
	}
	else
	{
		switch (dim)
		{
		case Latte::E_DIM::DIM_1D:
			if (firstSlice == 0)
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
				desc.Texture1D.MostDetailedMip = firstMip;
				desc.Texture1D.MipLevels = numMip;
			}
			else
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
				desc.Texture1DArray.MostDetailedMip = firstMip;
				desc.Texture1DArray.MipLevels = numMip;
				desc.Texture1DArray.FirstArraySlice = firstSlice;
				desc.Texture1DArray.ArraySize = 1;
			}
			break;
		case Latte::E_DIM::DIM_2D_ARRAY:
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.MostDetailedMip = firstMip;
			desc.Texture2DArray.MipLevels = numMip;
			desc.Texture2DArray.FirstArraySlice = firstSlice;
			desc.Texture2DArray.ArraySize = numSlice;
			break;
		case Latte::E_DIM::DIM_CUBEMAP:
			if ((numSlice % 6) == 0 && m_baseTexture->GetDesc().Width == m_baseTexture->GetDesc().Height)
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
				desc.TextureCubeArray.MostDetailedMip = firstMip;
				desc.TextureCubeArray.MipLevels = numMip;
				desc.TextureCubeArray.First2DArrayFace = firstSlice;
				desc.TextureCubeArray.NumCubes = numSlice / 6;
			}
			else
			{
				cemuLog_logDebugOnce(LogType::Force, "D3D12: Cubemap view with {} slices is not cube compatible", numSlice);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MostDetailedMip = firstMip;
				desc.Texture2DArray.MipLevels = numMip;
				desc.Texture2DArray.FirstArraySlice = firstSlice;
				desc.Texture2DArray.ArraySize = numSlice;
			}
			break;
		case Latte::E_DIM::DIM_3D:
			cemuLog_logDebugOnce(LogType::Force, "D3D12: 3D view of a non-3D texture is not supported");
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.MostDetailedMip = firstMip;
			desc.Texture2DArray.MipLevels = numMip;
			desc.Texture2DArray.FirstArraySlice = firstSlice;
			desc.Texture2DArray.ArraySize = numSlice;
			break;
		case Latte::E_DIM::DIM_2D:
		case Latte::E_DIM::DIM_2D_MSAA:
		default:
			if (firstSlice == 0)
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MostDetailedMip = firstMip;
				desc.Texture2D.MipLevels = numMip;
				desc.Texture2D.PlaneSlice = 0;
			}
			else
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MostDetailedMip = firstMip;
				desc.Texture2DArray.MipLevels = numMip;
				desc.Texture2DArray.FirstArraySlice = firstSlice;
				desc.Texture2DArray.ArraySize = 1;
			}
			break;
		}
	}
	D3D12_CPU_DESCRIPTOR_HANDLE h = m_renderer->GetStagingViewHeap().Allocate();
	m_renderer->GetDevice()->CreateShaderResourceView(m_baseTexture->GetResource(), &desc, h);
	D3D12_Checkpoint(m_renderer->GetDevice(), "texture SRV created");
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE LatteTextureViewD3D12::GetRTV()
{
	if (m_rtv.ptr)
		return m_rtv;
	cemu_assert_debug(!m_baseTexture->isDepth);
	D3D12_RENDER_TARGET_VIEW_DESC desc{};
	desc.Format = m_rtvFormat;
	if (m_baseTexture->Is3DTexture())
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MipSlice = firstMip;
		desc.Texture3D.FirstWSlice = firstSlice;
		desc.Texture3D.WSize = numSlice;
	}
	else if (m_baseTexture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = firstMip;
		desc.Texture1DArray.FirstArraySlice = firstSlice;
		desc.Texture1DArray.ArraySize = numSlice;
	}
	else
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice = firstMip;
		desc.Texture2DArray.FirstArraySlice = firstSlice;
		desc.Texture2DArray.ArraySize = numSlice;
		desc.Texture2DArray.PlaneSlice = 0;
	}
	m_rtv = m_renderer->GetStagingRTVHeap().Allocate();
	m_renderer->GetDevice()->CreateRenderTargetView(m_baseTexture->GetResource(), &desc, m_rtv);
	D3D12_Checkpoint(m_renderer->GetDevice(), "texture RTV created");
	return m_rtv;
}

D3D12_CPU_DESCRIPTOR_HANDLE LatteTextureViewD3D12::GetDSV()
{
	if (m_dsv.ptr)
		return m_dsv;
	cemu_assert_debug(m_baseTexture->isDepth);
	D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
	desc.Format = m_dsvFormat;
	desc.Flags = D3D12_DSV_FLAG_NONE;
	if (m_baseTexture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
	{
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = firstMip;
		desc.Texture1DArray.FirstArraySlice = firstSlice;
		desc.Texture1DArray.ArraySize = numSlice;
	}
	else
	{
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice = firstMip;
		desc.Texture2DArray.FirstArraySlice = firstSlice;
		desc.Texture2DArray.ArraySize = numSlice;
	}
	m_dsv = m_renderer->GetStagingDSVHeap().Allocate();
	m_renderer->GetDevice()->CreateDepthStencilView(m_baseTexture->GetResource(), &desc, m_dsv);
	D3D12_Checkpoint(m_renderer->GetDevice(), "texture DSV created");
	return m_dsv;
}
