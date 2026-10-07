#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <dxcapi.h>

// Mesa's spirv_to_dxil (src/microsoft/spirv_to_dxil). Written against the API as of Mesa 24.0:
//   bool spirv_to_dxil(const uint32_t* words, size_t word_count, struct dxil_spirv_specialization*, unsigned num_specializations,
//                      dxil_spirv_shader_stage stage, const char* entry_point_name, enum dxil_validator_version validator_version_max,
//                      const struct dxil_spirv_debug_options*, const struct dxil_spirv_runtime_conf*, const struct dxil_spirv_logger*,
//                      struct dxil_spirv_object* out_dxil);
// If you build against a newer Mesa where dxil_spirv_runtime_conf::zero_based_vertex_instance_id was replaced by
// first_vertex_and_base_instance_mode, define CEMU_SPIRV_TO_DXIL_SYSVAL_MODE.
extern "C"
{
#include "spirv_to_dxil.h"
}

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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

namespace
{
	// --- DXIL signing via dxil.dll ---
	// D3D12 only accepts DXIL containers that were signed by the DXIL validator. spirv_to_dxil produces unsigned
	// containers, so we run them through IDxcValidator with in-place signing. dxil.dll ships with the Windows SDK and
	// the DirectXShaderCompiler releases and has to be packaged with the app (UWP apps can only load packaged DLLs).

	// GUIDs from dxcapi.h, defined here so we don't rely on __uuidof support
	const GUID kCLSID_DxcValidator = { 0x8ca3e215, 0xf728, 0x4cf3, { 0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1 } };
	const GUID kIID_IDxcValidator = { 0xa6e82bd2, 0x1fd7, 0x4826, { 0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a } };
	const GUID kIID_IDxcBlob = { 0x8ba5fb08, 0x5195, 0x40e2, { 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02 } };

	// minimal IDxcBlob that wraps a std::vector without copying
	class VectorBlob final : public IDxcBlob
	{
	public:
		explicit VectorBlob(std::vector<uint8>& data) : m_data(data) {}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if (IsEqualGUID(riid, kIID_IDxcBlob) || IsEqualGUID(riid, IID_IUnknown))
			{
				*ppv = static_cast<IDxcBlob*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
		ULONG STDMETHODCALLTYPE Release() override { return --m_refCount; } // stack allocated, never deleted through Release()
		LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return m_data.data(); }
		SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return m_data.size(); }

	private:
		std::vector<uint8>& m_data;
		std::atomic<ULONG> m_refCount{ 1 };
	};

	struct
	{
		HMODULE dxilModule = nullptr;
		DxcCreateInstanceProc createInstance = nullptr;
		D3D_SHADER_MODEL highestShaderModel = D3D_SHADER_MODEL_6_0;
		std::mutex validatorMutex; // IDxcValidator instances are not documented as thread safe, we use one per call anyway
		bool glslangInitialized = false;
	} s_compiler;

	bool SignDXIL(std::vector<uint8>& dxil, std::string& logOut)
	{
		if (!s_compiler.createInstance)
		{
			logOut = "dxil.dll is not loaded, cannot sign shader";
			return false;
		}
		ComPtr<IDxcValidator> validator;
		HRESULT hr = s_compiler.createInstance(kCLSID_DxcValidator, kIID_IDxcValidator, (void**)validator.GetAddressOf());
		if (FAILED(hr))
		{
			logOut = "Failed to create IDxcValidator: " + D3D12_HResultToString(hr);
			return false;
		}
		VectorBlob blob(dxil);
		ComPtr<IDxcOperationResult> result;
		hr = validator->Validate(&blob, DxcValidatorFlags_InPlaceEdit, result.GetAddressOf());
		if (FAILED(hr) || !result)
		{
			logOut = "IDxcValidator::Validate failed: " + D3D12_HResultToString(hr);
			return false;
		}
		HRESULT status = E_FAIL;
		result->GetStatus(&status);
		if (FAILED(status))
		{
			ComPtr<IDxcBlobEncoding> errors;
			if (SUCCEEDED(result->GetErrorBuffer(errors.GetAddressOf())) && errors && errors->GetBufferSize() > 0)
				logOut.assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
			else
				logOut = "DXIL validation failed";
			return false;
		}
		return true;
	}

}

bool D3D12ShaderCompiler::Initialize(D3D_SHADER_MODEL highestShaderModel, std::string& errorOut)
{
	s_compiler.highestShaderModel = highestShaderModel;
	if (!s_compiler.glslangInitialized)
	{
		glslang::InitializeProcess();
		s_compiler.glslangInitialized = true;
	}
	if (s_compiler.dxilModule)
		return true;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
	s_compiler.dxilModule = LoadLibraryW(L"dxil.dll");
#else
	s_compiler.dxilModule = LoadPackagedLibrary(L"dxil.dll", 0);
#endif
	if (!s_compiler.dxilModule)
	{
		errorOut = "Unable to load dxil.dll. It is required to sign shaders and must be placed next to the executable (or packaged with the app on UWP)";
		return false;
	}
	s_compiler.createInstance = (DxcCreateInstanceProc)(void*)GetProcAddress(s_compiler.dxilModule, "DxcCreateInstance");
	if (!s_compiler.createInstance)
	{
		errorOut = "dxil.dll does not export DxcCreateInstance";
		FreeLibrary(s_compiler.dxilModule);
		s_compiler.dxilModule = nullptr;
		return false;
	}
	return true;
}

void D3D12ShaderCompiler::Shutdown()
{
	s_compiler.createInstance = nullptr;
	if (s_compiler.dxilModule)
	{
		FreeLibrary(s_compiler.dxilModule);
		s_compiler.dxilModule = nullptr;
	}
	// glslang::FinalizeProcess() is intentionally not called since the Vulkan backend may share the process state
}

bool D3D12ShaderCompiler::CompileGLSLToSPIRV(RendererShader::ShaderType type, const std::string& glslSource, std::vector<uint32>& spirvOut, std::string& logOut)
{
	EShLanguage state;
	switch (type)
	{
	case RendererShader::ShaderType::kVertex:
		state = EShLangVertex;
		break;
	case RendererShader::ShaderType::kFragment:
		state = EShLangFragment;
		break;
	case RendererShader::ShaderType::kGeometry:
		state = EShLangGeometry;
		break;
	default:
		logOut = "unknown shader type";
		return false;
	}
	// the source is Vulkan GLSL. Using the Vulkan client also defines the VULKAN macro, which selects the Vulkan code
	// paths in the GLSL emitted by Cemu's shader decompiler
	glslang::TShader shader(state);
	const char* cstr = glslSource.c_str();
	shader.setStrings(&cstr, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, state, glslang::EShClientVulkan, 100);
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
	spvOptions.disableOptimizer = false;
	spvOptions.validate = false;
	spvOptions.optimizeSize = true;
	spv::SpvBuildLogger logger;
	spirvOut.clear();
	glslang::GlslangToSpv(*program.getIntermediate(state), spirvOut, &logger, &spvOptions);
	if (spirvOut.empty())
	{
		logOut = "GlslangToSpv produced no output: " + logger.getAllMessages();
		return false;
	}
	return true;
}

bool D3D12ShaderCompiler::CompileSPIRVToDXIL(const std::vector<uint32>& spirv, RendererShader::ShaderType type, std::vector<uint8>& dxilOut, std::string& logOut)
{
	// numeric values of enum dxil_spirv_shader_stage (stable across Mesa versions): VERTEX=0, GEOMETRY=3, FRAGMENT=4
	dxil_spirv_shader_stage stage;
	switch (type)
	{
	case RendererShader::ShaderType::kVertex:
		stage = (dxil_spirv_shader_stage)0;
		break;
	case RendererShader::ShaderType::kFragment:
		stage = (dxil_spirv_shader_stage)4;
		break;
	case RendererShader::ShaderType::kGeometry:
		stage = (dxil_spirv_shader_stage)3;
		break;
	default:
		logOut = "unknown shader type";
		return false;
	}

	dxil_spirv_runtime_conf conf{};
	conf.runtime_data_cbv.register_space = D3D12Const::kSpaceRuntimeData;
	conf.runtime_data_cbv.base_shader_register = 0;
	conf.push_constant_cbv.register_space = D3D12Const::kSpacePushConstants;
	conf.push_constant_cbv.base_shader_register = 0;
	// gl_VertexIndex/gl_InstanceIndex include the base vertex/instance in Vulkan but SV_VertexID/SV_InstanceID don't.
	// Let spirv_to_dxil read first_vertex/base_instance from the runtime data CBV which the renderer fills per draw
#ifdef CEMU_SPIRV_TO_DXIL_SYSVAL_MODE
	conf.first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA;
#else
	conf.zero_based_vertex_instance_id = false;
#endif
	// Cemu's Vulkan backend flips Y with a negative viewport height. A positive D3D12 viewport produces the same
	// orientation for an unmodified shader, so no flipping is needed here
	conf.yz_flip.mode = (dxil_spirv_yz_flip_mode)0; // DXIL_SPIRV_YZ_FLIP_NONE
	conf.yz_flip.y_mask = 0;
	conf.yz_flip.z_mask = 0;
	conf.declared_read_only_images_as_srvs = true;
	conf.inferred_read_only_images_as_srvs = true;
	// D3D_SHADER_MODEL encodes 6.5 as 0x65, dxil_shader_model encodes it as 0x60005
	const uint32 smMajor = ((uint32)s_compiler.highestShaderModel >> 4) & 0xF;
	const uint32 smMinor = (uint32)s_compiler.highestShaderModel & 0xF;
	conf.shader_model_max = (dxil_shader_model)std::min<uint32>((smMajor << 16) | smMinor, 0x60005);

	struct LoggerCtx
	{
		std::string* log;
	} loggerCtx{ &logOut };
	dxil_spirv_logger logger{};
	logger.priv = &loggerCtx;
	logger.log = [](void* priv, const char* msg) {
		auto* ctx = (LoggerCtx*)priv;
		ctx->log->append(msg);
		ctx->log->append("\n");
	};

	dxil_spirv_debug_options debugOptions{};
	dxil_spirv_object object{};
	// validation (signing) is done by us through dxil.dll, see SignDXIL()
	bool success = spirv_to_dxil(spirv.data(), spirv.size(), nullptr, 0, stage, "main", (dxil_validator_version)0 /* NO_DXIL_VALIDATION */, &debugOptions, &conf, &logger, &object);
	if (!success)
	{
		if (logOut.empty())
			logOut = "spirv_to_dxil failed";
		return false;
	}
	dxilOut.assign((const uint8*)object.binary.buffer, (const uint8*)object.binary.buffer + object.binary.size);
	spirv_to_dxil_free(&object);
	return SignDXIL(dxilOut, logOut);
}
