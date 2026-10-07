#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderTranslate.h"

#include <d3dcompiler.h>
#include <dxcapi.h>

#ifdef CEMU_D3D12_SPIRV_TO_DXIL
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
#endif

#include <mutex>

namespace
{
	// GUIDs from dxcapi.h, defined here so we don't rely on __uuidof support or on a particular SDK version
	const GUID kCLSID_DxcCompiler = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
	const GUID kCLSID_DxcLibrary = { 0x6245d6af, 0x66e0, 0x48fd, { 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c } };
	const GUID kIID_IDxcCompiler = { 0x8c210bf3, 0x011f, 0x4422, { 0x8d, 0x70, 0x6f, 0x9a, 0xcb, 0x8d, 0xb6, 0x17 } };
	const GUID kIID_IDxcLibrary = { 0xe5204dc7, 0xd18c, 0x4c3c, { 0xbd, 0xfb, 0x85, 0x16, 0x73, 0x98, 0x0f, 0xe7 } };
#ifdef CEMU_D3D12_SPIRV_TO_DXIL
	const GUID kCLSID_DxcValidator = { 0x8ca3e215, 0xf728, 0x4cf3, { 0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1 } };
	const GUID kIID_IDxcValidator = { 0xa6e82bd2, 0x1fd7, 0x4826, { 0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a } };
	const GUID kIID_IDxcBlob = { 0x8ba5fb08, 0x5195, 0x40e2, { 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02 } };
#endif

	struct
	{
		D3D12ShaderCompiler::Backend backend = D3D12ShaderCompiler::Backend::FXC;
		D3D_SHADER_MODEL highestShaderModel = D3D_SHADER_MODEL_5_1;
		HMODULE dxcModule = nullptr;
		HMODULE dxilModule = nullptr;
		DxcCreateInstanceProc dxcCreateInstance = nullptr; // dxcompiler.dll (DXC backend)
		DxcCreateInstanceProc dxilCreateInstance = nullptr; // dxil.dll (spirv_to_dxil backend, signing)
	} s_compiler;

	HMODULE _LoadDLL(const wchar_t* name)
	{
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
		return LoadLibraryW(name);
#else
		// UWP apps can only load DLLs that are part of their package
		return LoadPackagedLibrary(name, 0);
#endif
	}

	std::string _GetEnv(const char* name)
	{
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
		char* value = nullptr;
		size_t len = 0;
		std::string result;
		if (_dupenv_s(&value, &len, name) == 0 && value)
		{
			result = value;
			free(value);
		}
		return result;
#else
		(void)name;
		return {};
#endif
	}

	bool _InitDXC(std::string& errorOut)
	{
		if ((uint32)s_compiler.highestShaderModel < 0x60)
		{
			errorOut = "the device does not support shader model 6.0";
			return false;
		}
		// dxcompiler.dll loads dxil.dll from the same directory to sign the generated DXIL
		s_compiler.dxcModule = _LoadDLL(L"dxcompiler.dll");
		if (!s_compiler.dxcModule)
		{
			errorOut = "dxcompiler.dll not found";
			return false;
		}
		s_compiler.dxilModule = _LoadDLL(L"dxil.dll");
		if (!s_compiler.dxilModule)
		{
			errorOut = "dxil.dll not found (D3D12 only accepts signed DXIL)";
			FreeLibrary(s_compiler.dxcModule);
			s_compiler.dxcModule = nullptr;
			return false;
		}
		s_compiler.dxcCreateInstance = (DxcCreateInstanceProc)(void*)GetProcAddress(s_compiler.dxcModule, "DxcCreateInstance");
		if (!s_compiler.dxcCreateInstance)
		{
			errorOut = "dxcompiler.dll does not export DxcCreateInstance";
			return false;
		}
		return true;
	}

	// --- FXC ---

	bool _CompileFXC(D3D12Const::Stage stage, const std::string& hlsl, std::vector<uint8>& bytecodeOut, std::string& logOut)
	{
		const char* target = "vs_5_1";
		if (stage == D3D12Const::Stage::Pixel)
			target = "ps_5_1";
		else if (stage == D3D12Const::Stage::Geometry)
			target = "gs_5_1";
		// IEEE strictness keeps FXC from folding NaN checks and x*0 patterns that the decompiled shaders rely on
		const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS;
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), "cemu_shader", nullptr, nullptr, "main", target, flags, 0, code.GetAddressOf(), errors.GetAddressOf());
		if (errors && errors->GetBufferSize() > 0)
			logOut.assign((const char*)errors->GetBufferPointer(), strnlen((const char*)errors->GetBufferPointer(), errors->GetBufferSize()));
		if (FAILED(hr) || !code)
		{
			if (logOut.empty())
				logOut = "D3DCompile failed: " + D3D12_HResultToString(hr);
			return false;
		}
		bytecodeOut.assign((const uint8*)code->GetBufferPointer(), (const uint8*)code->GetBufferPointer() + code->GetBufferSize());
		return true;
	}

	// --- DXC ---

	bool _CompileDXC(D3D12Const::Stage stage, const std::string& hlsl, std::vector<uint8>& bytecodeOut, std::string& logOut)
	{
		const wchar_t* target = L"vs_6_0";
		if (stage == D3D12Const::Stage::Pixel)
			target = L"ps_6_0";
		else if (stage == D3D12Const::Stage::Geometry)
			target = L"gs_6_0";
		// one compiler instance per call, IDxcCompiler is not thread safe
		ComPtr<IDxcLibrary> library;
		ComPtr<IDxcCompiler> compiler;
		HRESULT hr = s_compiler.dxcCreateInstance(kCLSID_DxcLibrary, kIID_IDxcLibrary, (void**)library.GetAddressOf());
		if (SUCCEEDED(hr))
			hr = s_compiler.dxcCreateInstance(kCLSID_DxcCompiler, kIID_IDxcCompiler, (void**)compiler.GetAddressOf());
		if (FAILED(hr))
		{
			logOut = "Failed to create DXC instance: " + D3D12_HResultToString(hr);
			return false;
		}
		ComPtr<IDxcBlobEncoding> source;
		hr = library->CreateBlobWithEncodingFromPinned(hlsl.data(), (UINT32)hlsl.size(), DXC_CP_UTF8, source.GetAddressOf());
		if (FAILED(hr))
		{
			logOut = "IDxcLibrary::CreateBlobWithEncodingFromPinned failed: " + D3D12_HResultToString(hr);
			return false;
		}
		// -HV 2018: SPIRV-Cross output targets classic HLSL semantics. -Gis: IEEE strictness
		LPCWSTR args[] = { L"-HV", L"2018", L"-Gis", L"-O1", L"-Qstrip_debug", L"-Qstrip_reflect" };
		ComPtr<IDxcOperationResult> result;
		hr = compiler->Compile(source.Get(), L"cemu_shader.hlsl", L"main", target, args, (UINT32)std::size(args), nullptr, 0, nullptr, result.GetAddressOf());
		if (FAILED(hr) || !result)
		{
			logOut = "IDxcCompiler::Compile failed: " + D3D12_HResultToString(hr);
			return false;
		}
		HRESULT status = E_FAIL;
		result->GetStatus(&status);
		ComPtr<IDxcBlobEncoding> errors;
		if (SUCCEEDED(result->GetErrorBuffer(errors.GetAddressOf())) && errors && errors->GetBufferSize() > 0)
			logOut.assign((const char*)errors->GetBufferPointer(), strnlen((const char*)errors->GetBufferPointer(), errors->GetBufferSize()));
		if (FAILED(status))
		{
			if (logOut.empty())
				logOut = "DXC compilation failed: " + D3D12_HResultToString(status);
			return false;
		}
		ComPtr<IDxcBlob> code;
		if (FAILED(result->GetResult(code.GetAddressOf())) || !code)
		{
			logOut = "DXC returned no bytecode";
			return false;
		}
		bytecodeOut.assign((const uint8*)code->GetBufferPointer(), (const uint8*)code->GetBufferPointer() + code->GetBufferSize());
		return true;
	}

#ifdef CEMU_D3D12_SPIRV_TO_DXIL
	// --- spirv_to_dxil ---
	// D3D12 only accepts DXIL containers that were signed by the DXIL validator. spirv_to_dxil produces unsigned
	// containers, so we run them through IDxcValidator with in-place signing.

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

	bool _SignDXIL(std::vector<uint8>& dxil, std::string& logOut)
	{
		ComPtr<IDxcValidator> validator;
		HRESULT hr = s_compiler.dxilCreateInstance(kCLSID_DxcValidator, kIID_IDxcValidator, (void**)validator.GetAddressOf());
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

	bool _InitSpirvToDXIL(std::string& errorOut)
	{
		if ((uint32)s_compiler.highestShaderModel < 0x60)
		{
			errorOut = "the device does not support shader model 6.0";
			return false;
		}
		s_compiler.dxilModule = _LoadDLL(L"dxil.dll");
		if (!s_compiler.dxilModule)
		{
			errorOut = "dxil.dll not found. It is required to sign shaders";
			return false;
		}
		s_compiler.dxilCreateInstance = (DxcCreateInstanceProc)(void*)GetProcAddress(s_compiler.dxilModule, "DxcCreateInstance");
		if (!s_compiler.dxilCreateInstance)
		{
			errorOut = "dxil.dll does not export DxcCreateInstance";
			return false;
		}
		return true;
	}

	bool _CompileSpirvToDXIL(const std::vector<uint32>& spirv, D3D12Const::Stage stage, std::vector<uint8>& dxilOut, std::string& logOut)
	{
		// numeric values of enum dxil_spirv_shader_stage (stable across Mesa versions): VERTEX=0, GEOMETRY=3, FRAGMENT=4
		dxil_spirv_shader_stage dxilStage = (dxil_spirv_shader_stage)0;
		if (stage == D3D12Const::Stage::Pixel)
			dxilStage = (dxil_spirv_shader_stage)4;
		else if (stage == D3D12Const::Stage::Geometry)
			dxilStage = (dxil_spirv_shader_stage)3;

		dxil_spirv_runtime_conf conf{};
		conf.runtime_data_cbv.register_space = D3D12Const::kSpaceRuntimeData;
		conf.runtime_data_cbv.base_shader_register = 0;
		conf.push_constant_cbv.register_space = D3D12Const::kSpacePushConstants;
		conf.push_constant_cbv.base_shader_register = 0;
		// gl_VertexIndex/gl_InstanceIndex include the base vertex/instance in Vulkan but SV_VertexID/SV_InstanceID don't.
		// spirv_to_dxil reads first_vertex/base_instance from the runtime data CBV which the renderer fills per draw
#ifdef CEMU_SPIRV_TO_DXIL_SYSVAL_MODE
		conf.first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA;
#else
		conf.zero_based_vertex_instance_id = false;
#endif
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
		bool success = spirv_to_dxil(spirv.data(), spirv.size(), nullptr, 0, dxilStage, "main", (dxil_validator_version)0 /* NO_DXIL_VALIDATION */, &debugOptions, &conf, &logger, &object);
		if (!success)
		{
			if (logOut.empty())
				logOut = "spirv_to_dxil failed";
			return false;
		}
		dxilOut.assign((const uint8*)object.binary.buffer, (const uint8*)object.binary.buffer + object.binary.size);
		spirv_to_dxil_free(&object);
		return _SignDXIL(dxilOut, logOut);
	}
#endif
}

bool D3D12ShaderCompiler::Initialize(D3D_SHADER_MODEL highestShaderModel, std::string& errorOut)
{
	s_compiler.highestShaderModel = highestShaderModel;
	D3D12ShaderTranslate::Initialize();

	const std::string requested = _GetEnv("CEMU_D3D12_SHADER_COMPILER");
	s_compiler.backend = Backend::FXC;
	if (requested == "dxc")
	{
		std::string err;
		if (_InitDXC(err))
			s_compiler.backend = Backend::DXC;
		else
			cemuLog_log(LogType::Force, "D3D12: DXC was requested but can't be used ({}), falling back to FXC", err);
	}
	else if (requested == "spirv_to_dxil")
	{
#ifdef CEMU_D3D12_SPIRV_TO_DXIL
		std::string err;
		if (_InitSpirvToDXIL(err))
			s_compiler.backend = Backend::SpirvToDXIL;
		else
			cemuLog_log(LogType::Force, "D3D12: spirv_to_dxil was requested but can't be used ({}), falling back to FXC", err);
#else
		cemuLog_log(LogType::Force, "D3D12: spirv_to_dxil was requested but Cemu was built without it, using FXC");
#endif
	}
	else if (!requested.empty() && requested != "fxc")
	{
		cemuLog_log(LogType::Force, "D3D12: Unknown shader compiler \"{}\" in CEMU_D3D12_SHADER_COMPILER, using FXC", requested);
	}
	cemuLog_log(LogType::Force, "D3D12: Shader compiler: {}", GetBackendName());
	return true;
}

void D3D12ShaderCompiler::Shutdown()
{
	s_compiler.dxcCreateInstance = nullptr;
	s_compiler.dxilCreateInstance = nullptr;
	if (s_compiler.dxcModule)
	{
		FreeLibrary(s_compiler.dxcModule);
		s_compiler.dxcModule = nullptr;
	}
	if (s_compiler.dxilModule)
	{
		FreeLibrary(s_compiler.dxilModule);
		s_compiler.dxilModule = nullptr;
	}
}

D3D12ShaderCompiler::Backend D3D12ShaderCompiler::GetBackend()
{
	return s_compiler.backend;
}

const char* D3D12ShaderCompiler::GetBackendName()
{
	switch (s_compiler.backend)
	{
	case Backend::FXC:
		return "fxc";
	case Backend::DXC:
		return "dxc";
	case Backend::SpirvToDXIL:
		return "spirv_to_dxil";
	}
	return "unknown";
}

bool D3D12ShaderCompiler::CompileHLSL(D3D12Const::Stage stage, const std::string& hlslSource, std::vector<uint8>& bytecodeOut, std::string& logOut)
{
	if (s_compiler.backend == Backend::DXC)
		return _CompileDXC(stage, hlslSource, bytecodeOut, logOut);
	return _CompileFXC(stage, hlslSource, bytecodeOut, logOut);
}

bool D3D12ShaderCompiler::CompileGLSL(D3D12Const::Stage stage, const std::string& glslSource, std::vector<uint8>& bytecodeOut, D3D12BindingRemap& remapOut, std::string& logOut)
{
	bytecodeOut.clear();
#ifdef CEMU_D3D12_SPIRV_TO_DXIL
	if (s_compiler.backend == Backend::SpirvToDXIL)
	{
		std::vector<uint32> spirv;
		std::string log;
		if (!D3D12ShaderTranslate::GLSLToSPIRV(stage, glslSource, spirv, log))
		{
			logOut = "GLSL to SPIR-V: " + log;
			return false;
		}
		if (!D3D12_RemapSpirvBindings(spirv, stage, remapOut, log))
		{
			logOut = "Binding remap: " + log;
			return false;
		}
		if (!_CompileSpirvToDXIL(spirv, stage, bytecodeOut, log))
		{
			logOut = "SPIR-V to DXIL: " + log;
			return false;
		}
		return true;
	}
#endif
	std::string hlsl;
	if (!D3D12ShaderTranslate::GLSLToHLSL(stage, glslSource, hlsl, remapOut, logOut))
		return false;
	std::string log;
	if (!CompileHLSL(stage, hlsl, bytecodeOut, log))
	{
		logOut = std::string(GetBackendName()) + ": " + log + "\nHLSL source:\n" + hlsl;
		return false;
	}
	return true;
}
