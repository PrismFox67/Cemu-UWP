#pragma once

// Direct3D 12 backend for Cemu
//
// Target: Windows 10/11 desktop (Win32 HWND) and UWP (CoreWindow / SwapChainPanel), including Xbox in Dev Mode.
// Shaders: the backend consumes the same Vulkan-flavored GLSL as the Vulkan renderer. It is compiled to SPIR-V with
// glslang and then translated to DXIL with Mesa's spirv_to_dxil (see RendererShaderD3D12.cpp).
//
// Only APIs available to UWP apps are used. In particular: no Agility SDK features, no D3D12 enhanced barriers,
// no D3D11on12, and no Win32 window APIs outside of the HWND swap chain path.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12BindingModel.h"

using Microsoft::WRL::ComPtr;

// filter encoding helpers from the Windows SDK's d3d12.h (missing from some third party headers)
#ifndef D3D12_ENCODE_BASIC_FILTER
#define D3D12_ENCODE_BASIC_FILTER(min, mag, mip, reduction) \
	((D3D12_FILTER)((((min) & D3D12_FILTER_TYPE_MASK) << D3D12_MIN_FILTER_SHIFT) | \
	(((mag) & D3D12_FILTER_TYPE_MASK) << D3D12_MAG_FILTER_SHIFT) | \
	(((mip) & D3D12_FILTER_TYPE_MASK) << D3D12_MIP_FILTER_SHIFT) | \
	(((reduction) & D3D12_FILTER_REDUCTION_TYPE_MASK) << D3D12_FILTER_REDUCTION_TYPE_SHIFT)))
#endif
#ifndef D3D12_ENCODE_ANISOTROPIC_FILTER
#define D3D12_ENCODE_ANISOTROPIC_FILTER(reduction) \
	((D3D12_FILTER)(D3D12_ANISOTROPIC_FILTERING_BIT | \
	D3D12_ENCODE_BASIC_FILTER(D3D12_FILTER_TYPE_LINEAR, D3D12_FILTER_TYPE_LINEAR, D3D12_FILTER_TYPE_LINEAR, reduction)))
#endif

class D3D12Renderer;

namespace D3D12Const
{
	// number of command lists that may be in flight at once
	inline constexpr uint32 kMaxSubmissionsInFlight = 4;
	// number of swap chain buffers
	inline constexpr uint32 kSwapChainBufferCount = 3;
	// shader-visible heap sizes
	inline constexpr uint32 kGpuViewHeapSize = 256 * 1024;
	inline constexpr uint32 kGpuSamplerHeapSize = 2048; // D3D12 hardware limit for shader visible sampler heaps
	// constant buffer views must start at multiples of this and have sizes that are multiples of it
	inline constexpr uint32 kCBVAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
}

inline D3D12Const::Stage D3D12StageFromLatte(LatteConst::ShaderType shaderType)
{
	switch (shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		return D3D12Const::Stage::Vertex;
	case LatteConst::ShaderType::Pixel:
		return D3D12Const::Stage::Pixel;
	case LatteConst::ShaderType::Geometry:
		return D3D12Const::Stage::Geometry;
	default:
		cemu_assert_debug(false);
		return D3D12Const::Stage::Vertex;
	}
}

// throws std::runtime_error with the HRESULT and a description
void D3D12_ThrowIfFailed(HRESULT hr, const char* what);
std::string D3D12_HResultToString(HRESULT hr);

template<typename T>
inline T AlignUp(T value, T alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

inline D3D12_RESOURCE_BARRIER D3D12_TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint32 subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = subresource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

inline D3D12_RESOURCE_BARRIER D3D12_UAVBarrier(ID3D12Resource* resource)
{
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = resource;
	return barrier;
}

inline D3D12_HEAP_PROPERTIES D3D12_HeapProps(D3D12_HEAP_TYPE type)
{
	D3D12_HEAP_PROPERTIES props{};
	props.Type = type;
	props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;
	return props;
}

inline D3D12_RESOURCE_DESC D3D12_BufferDesc(uint64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	return desc;
}

inline uint32 D3D12_CalcSubresource(uint32 mipSlice, uint32 arraySlice, uint32 planeSlice, uint32 mipLevels, uint32 arraySize)
{
	return mipSlice + arraySlice * mipLevels + planeSlice * mipLevels * arraySize;
}

void D3D12_SetDebugName(ID3D12Object* object, const std::string& name);
