#pragma once

// Platform independent part of the D3D12 shader pipeline:
//
//   Vulkan GLSL (Cemu's decompiler output) --glslang--> SPIR-V --D3D12_RemapSpirvBindings--> SPIR-V with D3D12 register
//   spaces --SPIRV-Cross--> HLSL (shader model 5.1)
//
// The HLSL is then compiled to bytecode by D3D12ShaderCompiler (FXC or DXC). Nothing in here depends on Windows or D3D12
// headers, so the whole translation can be built and tested on any platform.

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12BindingModel.h"

namespace D3D12ShaderTranslate
{
	// shader model of the generated HLSL. 5.1 is the D3D12 baseline and adds the register spaces the binding model
	// relies on. The same source also compiles with DXC for shader model 6.x
	inline constexpr uint32 kHLSLShaderModel = 51;

	// must be called once before the other functions. Safe to call more than once
	void Initialize();

	// Vulkan GLSL -> SPIR-V, using the same glslang configuration as the Vulkan backend (RendererShaderVk)
	bool GLSLToSPIRV(D3D12Const::Stage stage, const std::string& glslSource, std::vector<uint32>& spirvOut, std::string& logOut);

	// Translates a SPIR-V module whose bindings were rewritten by D3D12_RemapSpirvBindings() into HLSL with entry point "main".
	// Registers come straight from the remapped decorations (set = register space, binding = register). Combined image
	// samplers become a Texture + SamplerState pair that share the register number (t# / s#).
	// gl_VertexIndex / gl_InstanceIndex include the base vertex / base instance like in Vulkan. The offsets are read from
	// the runtime data root constants (b0, space kSpaceRuntimeData): dword 0 = base vertex, dword 1 = base instance.
	// Implemented in D3D12ShaderTranslateHLSL.cpp (SPIRV-Cross and glslang both declare namespace spv)
	bool SPIRVToHLSL(const std::vector<uint32>& spirv, D3D12Const::Stage stage, std::string& hlslOut, std::string& logOut);

	// all of the above. On failure logOut names the step that failed
	bool GLSLToHLSL(D3D12Const::Stage stage, const std::string& glslSource, std::string& hlslOut, D3D12BindingRemap& remapOut, std::string& logOut);
}
