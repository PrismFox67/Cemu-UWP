#pragma once

// Resource binding model of the D3D12 backend. Kept free of Windows/D3D12 headers so the SPIR-V binding remapper
// (D3D12SpirvRemap.cpp) can be built and tested on any platform.

#include "Cafe/HW/Latte/Core/LatteConst.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace D3D12Const
{
	// --- resource binding model ---
	//
	// Shaders are compiled from Vulkan GLSL. Before translating SPIR-V to DXIL, every (set, binding) decoration is
	// rewritten so each resource lands in a fixed register space:
	//
	//   space    = stage * kSpacesPerStage + class
	//   register = rank of the original binding among the resources of the same class that exist in the module
	//
	// The resulting binding -> register table (D3D12BindingRemap) is stored with the compiled shader. At draw time the
	// renderer looks up the register for each binding listed in LatteDecompilerShaderResourceMapping, so resources
	// that the SPIR-V optimizer removed simply have no register and are skipped.
	enum class Stage : uint32
	{
		Vertex = 0,
		Pixel = 1,
		Geometry = 2,
		Count = 3
	};

	enum class BindingClass : uint32
	{
		CBV = 0, // uniform blocks (b#)
		SRV = 1, // sampled textures (t#), combined samplers share this space as s#
		UAV = 2, // storage buffers (u#), used for transform feedback via SSBO
	};

	inline constexpr uint32 kSpacesPerStage = 4;
	inline constexpr uint32 kSpacePushConstants = 30; // internal shaders only (surface copy etc.)
	inline constexpr uint32 kSpaceRuntimeData = 31; // spirv_to_dxil vertex runtime data (first vertex, base instance)

	inline constexpr uint32 RegisterSpace(Stage stage, BindingClass cls)
	{
		return (uint32)stage * kSpacesPerStage + (uint32)cls;
	}

	// per-stage descriptor table sizes
	inline constexpr uint32 kMaxCBVsPerStage = 1 + LATTE_NUM_MAX_UNIFORM_BUFFERS; // uniform var block + uniform buffers
	inline constexpr uint32 kMaxSRVsPerStage = LATTE_NUM_MAX_TEX_UNITS;
	inline constexpr uint32 kMaxUAVsPerStage = 1;
	inline constexpr uint32 kStageTableSize = kMaxCBVsPerStage + kMaxSRVsPerStage + kMaxUAVsPerStage;
	inline constexpr uint32 kStageTableOffsetCBV = 0;
	inline constexpr uint32 kStageTableOffsetSRV = kMaxCBVsPerStage;
	inline constexpr uint32 kStageTableOffsetUAV = kMaxCBVsPerStage + kMaxSRVsPerStage;
	inline constexpr uint32 kMaxSamplersPerStage = LATTE_NUM_MAX_TEX_UNITS;

	// root parameter indices
	inline constexpr uint32 RootParamStageTable(Stage stage) { return (uint32)stage * 2 + 0; }
	inline constexpr uint32 RootParamStageSamplers(Stage stage) { return (uint32)stage * 2 + 1; }
	inline constexpr uint32 kRootParamRuntimeData = 6;
	inline constexpr uint32 kRootParamPushConstants = 7;
	inline constexpr uint32 kRootParamCount = 8;

	inline constexpr uint32 kRuntimeDataDwords = 16; // must be >= sizeof(dxil_spirv_vertex_runtime_data) / 4
	inline constexpr uint32 kPushConstantDwords = 16;
}

// Result of rewriting the descriptor bindings of a SPIR-V module.
// Maps the binding numbers that appear in the GLSL source (layout(binding = N)) to D3D12 shader registers.
// The register space is implied by the stage and binding class (see D3D12Const::RegisterSpace).
struct D3D12BindingRemap
{
	static constexpr uint32 kMaxBinding = 64;
	static constexpr sint8 kUnused = -1;

	sint8 cbv[kMaxBinding];
	sint8 srv[kMaxBinding]; // textures. The sampler of a combined image sampler uses the same register (s#)
	sint8 uav[kMaxBinding];
	uint8 cbvCount = 0;
	uint8 srvCount = 0;
	uint8 uavCount = 0;

	D3D12BindingRemap()
	{
		std::fill(std::begin(cbv), std::end(cbv), kUnused);
		std::fill(std::begin(srv), std::end(srv), kUnused);
		std::fill(std::begin(uav), std::end(uav), kUnused);
	}

	sint32 GetCBV(sint32 binding) const { return (binding >= 0 && binding < (sint32)kMaxBinding) ? cbv[binding] : kUnused; }
	sint32 GetSRV(sint32 binding) const { return (binding >= 0 && binding < (sint32)kMaxBinding) ? srv[binding] : kUnused; }
	sint32 GetUAV(sint32 binding) const { return (binding >= 0 && binding < (sint32)kMaxBinding) ? uav[binding] : kUnused; }
};

// Rewrites the DescriptorSet/Binding decorations of a SPIR-V module so that every resource lands in the fixed per-stage
// register space of its class (see D3D12Const::RegisterSpace), with registers assigned in order of the original
// binding numbers. The original binding -> register mapping is returned in remapOut. Implemented in D3D12SpirvRemap.cpp
bool D3D12_RemapSpirvBindings(std::vector<uint32>& spirv, D3D12Const::Stage stage, D3D12BindingRemap& remapOut, std::string& logOut);
