#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/RendererShader.h"

// Turns the Vulkan GLSL emitted by Cemu's shader decompiler into D3D12 shader bytecode.
//
// Default pipeline (no extra dependencies, works on desktop and UWP/Xbox):
//   GLSL --glslang--> SPIR-V --binding remap--> SPIR-V --SPIRV-Cross--> HLSL 5.1 --FXC (d3dcompiler_47)--> DXBC
// Optional, selected with the environment variable CEMU_D3D12_SHADER_COMPILER:
//   "dxc"            HLSL is compiled with DXC to DXIL (shader model 6.0). Needs dxcompiler.dll and dxil.dll next to
//                    the executable and a shader model 6.0 capable driver. Much faster to compile than FXC
//   "spirv_to_dxil"  SPIR-V is translated with Mesa's spirv_to_dxil (only if built with ENABLE_D3D12_SPIRV_TO_DXIL)
namespace D3D12ShaderCompiler
{
	enum class Backend
	{
		FXC,
		DXC,
		SpirvToDXIL,
	};

	// Must be called once before any compilation. Picks the backend
	bool Initialize(D3D_SHADER_MODEL highestShaderModel, std::string& errorOut);
	void Shutdown();

	Backend GetBackend();
	const char* GetBackendName(); // short lowercase name, also used to name the bytecode cache

	// Vulkan GLSL -> shader bytecode for the active backend. remapOut receives the binding -> register table
	bool CompileGLSL(D3D12Const::Stage stage, const std::string& glslSource, std::vector<uint8>& bytecodeOut, D3D12BindingRemap& remapOut, std::string& logOut);

	// HLSL -> bytecode with FXC (DXBC, shader model 5.1) or DXC (DXIL, shader model 6.0), depending on the backend
	bool CompileHLSL(D3D12Const::Stage stage, const std::string& hlslSource, std::vector<uint8>& bytecodeOut, std::string& logOut);

	inline D3D12Const::Stage StageFromShaderType(RendererShader::ShaderType type)
	{
		switch (type)
		{
		case RendererShader::ShaderType::kVertex:
			return D3D12Const::Stage::Vertex;
		case RendererShader::ShaderType::kFragment:
			return D3D12Const::Stage::Pixel;
		case RendererShader::ShaderType::kGeometry:
			return D3D12Const::Stage::Geometry;
		}
		return D3D12Const::Stage::Vertex;
	}
}
