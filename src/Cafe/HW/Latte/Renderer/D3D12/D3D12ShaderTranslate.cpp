#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderTranslate.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>

// glslang resource limits, identical to the ones used by RendererShaderVk so both backends accept the same GLSL
static consteval TBuiltInResource _GetDefaultBuiltInResourceD3D12()
{
	TBuiltInResource defaultResource = {};
	defaultResource.maxLights = 32;
	defaultResource.maxClipPlanes = 6;
	defaultResource.maxTextureUnits = 32;
	defaultResource.maxTextureCoords = 32;
	defaultResource.maxVertexAttribs = 64;
	defaultResource.maxVertexUniformComponents = 4096;
	defaultResource.maxVaryingFloats = 64;
	defaultResource.maxVertexTextureImageUnits = 32;
	defaultResource.maxCombinedTextureImageUnits = 80;
	defaultResource.maxTextureImageUnits = 32;
	defaultResource.maxFragmentUniformComponents = 4096;
	defaultResource.maxDrawBuffers = 32;
	defaultResource.maxVertexUniformVectors = 128;
	defaultResource.maxVaryingVectors = 8;
	defaultResource.maxFragmentUniformVectors = 16;
	defaultResource.maxVertexOutputVectors = 16;
	defaultResource.maxFragmentInputVectors = 15;
	defaultResource.minProgramTexelOffset = -8;
	defaultResource.maxProgramTexelOffset = 7;
	defaultResource.maxClipDistances = 8;
	defaultResource.maxComputeWorkGroupCountX = 65535;
	defaultResource.maxComputeWorkGroupCountY = 65535;
	defaultResource.maxComputeWorkGroupCountZ = 65535;
	defaultResource.maxComputeWorkGroupSizeX = 1024;
	defaultResource.maxComputeWorkGroupSizeY = 1024;
	defaultResource.maxComputeWorkGroupSizeZ = 64;
	defaultResource.maxComputeUniformComponents = 1024;
	defaultResource.maxComputeTextureImageUnits = 16;
	defaultResource.maxComputeImageUniforms = 8;
	defaultResource.maxComputeAtomicCounters = 8;
	defaultResource.maxComputeAtomicCounterBuffers = 1;
	defaultResource.maxVaryingComponents = 60;
	defaultResource.maxVertexOutputComponents = 64;
	defaultResource.maxGeometryInputComponents = 64;
	defaultResource.maxGeometryOutputComponents = 128;
	defaultResource.maxFragmentInputComponents = 128;
	defaultResource.maxImageUnits = 8;
	defaultResource.maxCombinedImageUnitsAndFragmentOutputs = 8;
	defaultResource.maxCombinedShaderOutputResources = 8;
	defaultResource.maxImageSamples = 0;
	defaultResource.maxVertexImageUniforms = 0;
	defaultResource.maxTessControlImageUniforms = 0;
	defaultResource.maxTessEvaluationImageUniforms = 0;
	defaultResource.maxGeometryImageUniforms = 0;
	defaultResource.maxFragmentImageUniforms = 8;
	defaultResource.maxCombinedImageUniforms = 8;
	defaultResource.maxGeometryTextureImageUnits = 16;
	defaultResource.maxGeometryOutputVertices = 256;
	defaultResource.maxGeometryTotalOutputComponents = 1024;
	defaultResource.maxGeometryUniformComponents = 1024;
	defaultResource.maxGeometryVaryingComponents = 64;
	defaultResource.maxTessControlInputComponents = 128;
	defaultResource.maxTessControlOutputComponents = 128;
	defaultResource.maxTessControlTextureImageUnits = 16;
	defaultResource.maxTessControlUniformComponents = 1024;
	defaultResource.maxTessControlTotalOutputComponents = 4096;
	defaultResource.maxTessEvaluationInputComponents = 128;
	defaultResource.maxTessEvaluationOutputComponents = 128;
	defaultResource.maxTessEvaluationTextureImageUnits = 16;
	defaultResource.maxTessEvaluationUniformComponents = 1024;
	defaultResource.maxTessPatchComponents = 120;
	defaultResource.maxPatchVertices = 32;
	defaultResource.maxTessGenLevel = 64;
	defaultResource.maxViewports = 16;
	defaultResource.maxVertexAtomicCounters = 0;
	defaultResource.maxTessControlAtomicCounters = 0;
	defaultResource.maxTessEvaluationAtomicCounters = 0;
	defaultResource.maxGeometryAtomicCounters = 0;
	defaultResource.maxFragmentAtomicCounters = 8;
	defaultResource.maxCombinedAtomicCounters = 8;
	defaultResource.maxAtomicCounterBindings = 1;
	defaultResource.maxVertexAtomicCounterBuffers = 0;
	defaultResource.maxTessControlAtomicCounterBuffers = 0;
	defaultResource.maxTessEvaluationAtomicCounterBuffers = 0;
	defaultResource.maxGeometryAtomicCounterBuffers = 0;
	defaultResource.maxFragmentAtomicCounterBuffers = 1;
	defaultResource.maxCombinedAtomicCounterBuffers = 1;
	defaultResource.maxAtomicCounterBufferSize = 16384;
	defaultResource.maxTransformFeedbackBuffers = 4;
	defaultResource.maxTransformFeedbackInterleavedComponents = 64;
	defaultResource.maxCullDistances = 8;
	defaultResource.maxCombinedClipAndCullDistances = 8;
	defaultResource.maxSamples = 4;
	defaultResource.maxMeshOutputVerticesNV = 256;
	defaultResource.maxMeshOutputPrimitivesNV = 512;
	defaultResource.maxMeshWorkGroupSizeX_NV = 32;
	defaultResource.maxMeshWorkGroupSizeY_NV = 1;
	defaultResource.maxMeshWorkGroupSizeZ_NV = 1;
	defaultResource.maxTaskWorkGroupSizeX_NV = 32;
	defaultResource.maxTaskWorkGroupSizeY_NV = 1;
	defaultResource.maxTaskWorkGroupSizeZ_NV = 1;
	defaultResource.maxMeshViewCountNV = 4;

	defaultResource.limits = {};
	defaultResource.limits.nonInductiveForLoops = true;
	defaultResource.limits.whileLoops = true;
	defaultResource.limits.doWhileLoops = true;
	defaultResource.limits.generalUniformIndexing = true;
	defaultResource.limits.generalAttributeMatrixVectorIndexing = true;
	defaultResource.limits.generalVaryingIndexing = true;
	defaultResource.limits.generalSamplerIndexing = true;
	defaultResource.limits.generalVariableIndexing = true;
	defaultResource.limits.generalConstantMatrixVectorIndexing = true;
	return defaultResource;
};

void D3D12ShaderTranslate::Initialize()
{
	// glslang reference counts InitializeProcess(), but do it only once from our side
	static std::once_flag s_initFlag;
	std::call_once(s_initFlag, []() { glslang::InitializeProcess(); });
	// glslang::FinalizeProcess() is intentionally never called, other backends may share the process state
}

bool D3D12ShaderTranslate::GLSLToSPIRV(D3D12Const::Stage stage, const std::string& glslSource, std::vector<uint32>& spirvOut, std::string& logOut)
{
	EShLanguage language;
	switch (stage)
	{
	case D3D12Const::Stage::Vertex:
		language = EShLangVertex;
		break;
	case D3D12Const::Stage::Pixel:
		language = EShLangFragment;
		break;
	case D3D12Const::Stage::Geometry:
		language = EShLangGeometry;
		break;
	default:
		logOut = "unknown shader stage";
		return false;
	}
	// The source is Vulkan GLSL. Using the Vulkan client also defines the VULKAN macro, which selects the Vulkan code
	// paths in the GLSL emitted by Cemu's shader decompiler
	glslang::TShader shader(language);
	const char* cstr = glslSource.c_str();
	shader.setStrings(&cstr, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetClientVersion::EShTargetVulkan_1_1);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetLanguageVersion::EShTargetSpv_1_3);

	static constexpr TBuiltInResource resources = _GetDefaultBuiltInResourceD3D12();
	glslang::TShader::ForbidIncluder includer;
	const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
	std::string preprocessed;
	if (!shader.preprocess(&resources, 450, ENoProfile, false, false, messages, &preprocessed, includer))
	{
		logOut = std::string("GLSL preprocessing failed: ") + shader.getInfoLog();
		return false;
	}
	const char* preprocessedCStr = preprocessed.c_str();
	shader.setStrings(&preprocessedCStr, 1);
	if (!shader.parse(&resources, 100, false, messages))
	{
		logOut = std::string("GLSL parsing failed: ") + shader.getInfoLog();
		return false;
	}
	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages) || !program.mapIO())
	{
		logOut = std::string("GLSL linking failed: ") + program.getInfoLog();
		return false;
	}
	glslang::SpvOptions spvOptions;
	// No spirv-opt: its dead code elimination drops stage inputs that the shader declares but never reads. D3D12
	// matches the output signature of one stage against the input signature of the next by register, so every declared
	// input/output has to survive (see LatteDecompilerOptions::declareAllPSInputs). FXC optimizes the HLSL anyway.
	// This also makes the output independent of whether glslang was built with the optimizer (Cemu's vcpkg build isn't)
	spvOptions.disableOptimizer = true;
	spvOptions.validate = false;
	spvOptions.optimizeSize = false;
	spv::SpvBuildLogger logger;
	spirvOut.clear();
	glslang::GlslangToSpv(*program.getIntermediate(language), spirvOut, &logger, &spvOptions);
	if (spirvOut.empty())
	{
		logOut = "GlslangToSpv produced no output: " + logger.getAllMessages();
		return false;
	}
	return true;
}

bool D3D12ShaderTranslate::GLSLToHLSL(D3D12Const::Stage stage, const std::string& glslSource, std::string& hlslOut, D3D12BindingRemap& remapOut, std::string& logOut)
{
	std::vector<uint32> spirv;
	std::string log;
	if (!GLSLToSPIRV(stage, glslSource, spirv, log))
	{
		logOut = "GLSL to SPIR-V: " + log;
		return false;
	}
	if (!D3D12_RemapSpirvBindings(spirv, stage, remapOut, log))
	{
		logOut = "Binding remap: " + log;
		return false;
	}
	if (!SPIRVToHLSL(spirv, stage, hlslOut, log))
	{
		logOut = "SPIR-V to HLSL: " + log;
		return false;
	}
	return true;
}
