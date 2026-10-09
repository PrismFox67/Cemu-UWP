#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"

LatteTextureD3D12::LatteTextureD3D12(D3D12Renderer* renderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
	uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
	: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth), m_renderer(renderer)
{
	D3D12Format::GetTextureFormatInfo(renderer->GetFormatSupport(), format, isDepth, m_formatInfo);
	cemu_assert_debug(hasStencil == m_formatInfo.hasStencil || !isDepth);

	sint32 effectiveBaseWidth = width;
	sint32 effectiveBaseHeight = height;
	sint32 effectiveBaseDepth = depth;
	if (overwriteInfo.hasResolutionOverwrite)
	{
		effectiveBaseWidth = overwriteInfo.width;
		effectiveBaseHeight = overwriteInfo.height;
		effectiveBaseDepth = overwriteInfo.depth;
	}
	effectiveBaseDepth = std::max(1, effectiveBaseDepth);

	m_desc.Alignment = 0;
	m_desc.Width = (UINT64)std::max(1, effectiveBaseWidth);
	m_desc.Height = (UINT)std::max(1, effectiveBaseHeight);
	m_desc.MipLevels = (UINT16)std::max<uint32>(1, mipLevels);
	m_desc.Format = m_formatInfo.resourceFormat;
	m_desc.SampleDesc.Count = 1; // GPU7 MSAA surfaces are emulated as single sampled, like the Vulkan backend does
	m_desc.SampleDesc.Quality = 0;
	m_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	m_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	switch (dim)
	{
	case Latte::E_DIM::DIM_1D:
		m_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
		m_desc.Height = 1;
		m_desc.DepthOrArraySize = (UINT16)effectiveBaseDepth;
		break;
	case Latte::E_DIM::DIM_3D:
		m_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		m_desc.DepthOrArraySize = (UINT16)effectiveBaseDepth;
		break;
	case Latte::E_DIM::DIM_2D:
	case Latte::E_DIM::DIM_2D_ARRAY:
	case Latte::E_DIM::DIM_CUBEMAP:
	case Latte::E_DIM::DIM_2D_MSAA:
		m_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		m_desc.DepthOrArraySize = (UINT16)effectiveBaseDepth;
		break;
	default:
		cemu_assert_unimplemented();
		m_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		m_desc.DepthOrArraySize = (UINT16)effectiveBaseDepth;
		break;
	}

	// block compressed textures must have dimensions that are multiples of the block size for the top mip
	if (m_formatInfo.isCompressed)
	{
		m_desc.Width = AlignUp<UINT64>(m_desc.Width, 4);
		m_desc.Height = AlignUp<UINT>(m_desc.Height, 4);
	}

	if (isDepth)
		m_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	else if (m_formatInfo.rtvFormat != DXGI_FORMAT_UNKNOWN)
		m_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	m_planeCount = m_formatInfo.hasStencil ? 2 : 1;

	D3D12_HEAP_PROPERTIES heapProps = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
	HRESULT hr = renderer->GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &m_desc, initialState, nullptr, IID_PPV_ARGS(&m_resource));
	D3D12_Checkpoint(renderer->GetDevice(), "game texture created");
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: Failed to create texture {}x{}x{} fmt {:04x} ({}): {}", m_desc.Width, m_desc.Height, m_desc.DepthOrArraySize, (uint32)format, (uint32)m_desc.Format, D3D12_HResultToString(hr));
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
			renderer->HandleDeviceError(hr, "CreateCommittedResource (texture)");
		// otherwise keep going without a host resource. All users check GetResource() and views become null descriptors
	}
	else if (renderer->IsDebugNamingEnabled())
	{
		D3D12_SetDebugName(m_resource.Get(), fmt::format("tex_{:08x}_fmt{:04x}_tm{:x}", physAddress, (uint32)format, (uint32)tileMode));
	}
	m_states.resize(GetSubresourceCount(), initialState);
}

LatteTextureD3D12::~LatteTextureD3D12()
{
	cemu_assert_debug(views.empty());
	m_renderer->NotifyTextureRelease(this);
	m_renderer->ReleaseResourceDeferred(std::move(m_resource));
}

void LatteTextureD3D12::AllocateOnHost()
{
	// committed resources are allocated on creation
}

LatteTextureView* LatteTextureD3D12::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
{
	cemu_assert_debug(mipCount > 0);
	cemu_assert_debug(sliceCount > 0);
	cemu_assert_debug((firstMip + mipCount) <= this->mipLevels);
	cemu_assert_debug((firstSlice + sliceCount) <= this->depth);
	return new LatteTextureViewD3D12(m_renderer, this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
}

bool LatteTextureD3D12::AllSubresourcesInState(D3D12_RESOURCE_STATES state) const
{
	for (auto s : m_states)
		if (s != state)
			return false;
	return true;
}

void LatteTextureD3D12::SetAllSubresourceStates(D3D12_RESOURCE_STATES state)
{
	std::fill(m_states.begin(), m_states.end(), state);
}
