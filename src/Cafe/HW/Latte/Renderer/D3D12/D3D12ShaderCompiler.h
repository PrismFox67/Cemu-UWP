#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/RendererShader.h"

namespace D3D12ShaderCompiler
{
	// Must be called once before any compilation. Loads dxil.dll (needed to sign the generated DXIL)
	bool Initialize(D3D_SHADER_MODEL highestShaderModel, std::string& errorOut);
	void Shutdown();

	// Vulkan GLSL -> SPIR-V (same glslang configuration as the Vulkan backend)
	bool CompileGLSLToSPIRV(RendererShader::ShaderType type, const std::string& glslSource, std::vector<uint32>& spirvOut, std::string& logOut);

	// SPIR-V -> signed DXIL
	bool CompileSPIRVToDXIL(const std::vector<uint32>& spirv, RendererShader::ShaderType type, std::vector<uint8>& dxilOut, std::string& logOut);

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
