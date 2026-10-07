#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderTranslate.h"

#include <spirv_cross/spirv_hlsl.hpp>

#include <unordered_set>

// SPIRV-Cross renames identifiers that are HLSL keywords but not ones that collide with HLSL intrinsic functions.
// GLSL code can legally use these as variable or function names (e.g. "vec2 frac = fract(x);" in Cemu's output
// shaders), which FXC rejects
static void _RenameHLSLIntrinsicCollisions(spirv_cross::CompilerHLSL& compiler)
{
	static const std::unordered_set<std::string> kIntrinsics = {
		"abort", "abs", "acos", "all", "AllMemoryBarrier", "AllMemoryBarrierWithGroupSync", "any", "asdouble", "asfloat",
		"asin", "asint", "asuint", "atan", "atan2", "ceil", "CheckAccessFullyMapped", "clamp", "clip", "cos", "cosh",
		"countbits", "cross", "D3DCOLORtoUBYTE4", "ddx", "ddx_coarse", "ddx_fine", "ddy", "ddy_coarse", "ddy_fine",
		"degrees", "determinant", "DeviceMemoryBarrier", "DeviceMemoryBarrierWithGroupSync", "distance", "dot", "dst",
		"errorf", "EvaluateAttributeAtCentroid", "EvaluateAttributeAtSample", "EvaluateAttributeSnapped", "exp", "exp2",
		"f16tof32", "f32tof16", "faceforward", "firstbithigh", "firstbitlow", "floor", "fma", "fmod", "frac", "frexp",
		"fwidth", "GetRenderTargetSampleCount", "GetRenderTargetSamplePosition", "GroupMemoryBarrier",
		"GroupMemoryBarrierWithGroupSync", "InterlockedAdd", "InterlockedAnd", "InterlockedCompareExchange",
		"InterlockedCompareStore", "InterlockedExchange", "InterlockedMax", "InterlockedMin", "InterlockedOr",
		"InterlockedXor", "isfinite", "isinf", "isnan", "ldexp", "length", "lerp", "lit", "log", "log10", "log2", "mad",
		"max", "min", "modf", "msad4", "mul", "noise", "normalize", "pow", "printf", "radians", "rcp", "reflect",
		"refract", "reversebits", "round", "rsqrt", "saturate", "sign", "sin", "sincos", "sinh", "smoothstep", "sqrt",
		"step", "tan", "tanh", "tex1D", "tex1Dbias", "tex1Dgrad", "tex1Dlod", "tex1Dproj", "tex2D", "tex2Dbias",
		"tex2Dgrad", "tex2Dlod", "tex2Dproj", "tex3D", "tex3Dbias", "tex3Dgrad", "tex3Dlod", "tex3Dproj", "texCUBE",
		"texCUBEbias", "texCUBEgrad", "texCUBElod", "texCUBEproj", "transpose", "trunc",
	};
	const uint32_t bound = compiler.get_current_id_bound();
	for (uint32_t id = 1; id < bound; id++)
	{
		const std::string& name = compiler.get_name(id);
		if (!name.empty() && kIntrinsics.count(name) != 0)
			compiler.set_name(id, name + "_");
	}
}

// SPIRV-Cross declares the geometry shader's per-vertex position input as "gl_PositionIn[]" (and fills it from the
// input struct) but emits accesses through the GLSL block syntax "gl_in[i].gl_Position", which doesn't compile.
// Only the generated RECT emulation geometry shader reads gl_in
static void _FixupGeometryShaderPositionInput(std::string& hlsl)
{
	static const std::string kPrefix = "gl_in[";
	static const std::string kSuffix = "].gl_Position";
	size_t pos = 0;
	while ((pos = hlsl.find(kPrefix, pos)) != std::string::npos)
	{
		// find the matching ']' (the index can be an expression with brackets)
		size_t i = pos + kPrefix.size();
		int depth = 1;
		while (i < hlsl.size() && depth > 0)
		{
			if (hlsl[i] == '[')
				depth++;
			else if (hlsl[i] == ']')
				depth--;
			i++;
		}
		const size_t closing = i - 1;
		if (depth != 0 || hlsl.compare(closing, kSuffix.size(), kSuffix) != 0)
		{
			pos += kPrefix.size();
			continue;
		}
		const std::string index = hlsl.substr(pos + kPrefix.size(), closing - (pos + kPrefix.size()));
		const std::string replacement = "gl_PositionIn[" + index + "]";
		hlsl.replace(pos, closing + kSuffix.size() - pos, replacement);
		pos += replacement.size();
	}
}

bool D3D12ShaderTranslate::SPIRVToHLSL(const std::vector<uint32>& spirv, D3D12Const::Stage stage, std::string& hlslOut, std::string& logOut)
{
	hlslOut.clear();
	try
	{
		spirv_cross::CompilerHLSL compiler(spirv.data(), spirv.size());

		const spv::ExecutionModel model = compiler.get_execution_model();
		const bool stageMatches = (stage == D3D12Const::Stage::Vertex && model == spv::ExecutionModelVertex) ||
								  (stage == D3D12Const::Stage::Pixel && model == spv::ExecutionModelFragment) ||
								  (stage == D3D12Const::Stage::Geometry && model == spv::ExecutionModelGeometry);
		if (!stageMatches)
		{
			logOut = "SPIR-V execution model does not match the shader stage";
			return false;
		}

		spirv_cross::CompilerGLSL::Options common = compiler.get_common_options();
		// the GLSL targets Vulkan clip space, which D3D shares (z in [0,1]). Y is handled with a positive D3D viewport
		common.vertex.fixup_clipspace = false;
		common.vertex.flip_vert_y = false;
		compiler.set_common_options(common);

		spirv_cross::CompilerHLSL::Options options;
		options.shader_model = kHLSLShaderModel;
		// gl_PointSize is written for point primitives. D3D10+ has no point size, points are always one pixel
		options.point_size_compat = true;
		options.point_coord_compat = true;
		// Vulkan's gl_VertexIndex/gl_InstanceIndex include the base vertex/instance, SV_VertexID/SV_InstanceID don't
		options.support_nonzero_base_vertex_base_instance = (stage == D3D12Const::Stage::Vertex);
		// the transform feedback SSBO is always bound as a UAV
		options.force_storage_buffer_as_uav = true;
		compiler.set_hlsl_options(options);

		if (stage == D3D12Const::Stage::Vertex)
			compiler.set_hlsl_aux_buffer_binding(spirv_cross::HLSL_AUX_BINDING_BASE_VERTEX_INSTANCE, 0, D3D12Const::kSpaceRuntimeData);

		// push constants (internal shaders only) map to the root constants in kSpacePushConstants
		std::vector<spirv_cross::RootConstants> rootConstants(1);
		rootConstants[0].start = 0;
		rootConstants[0].end = D3D12Const::kPushConstantDwords * 4;
		rootConstants[0].binding = 0;
		rootConstants[0].space = D3D12Const::kSpacePushConstants;
		compiler.set_root_constant_layouts(rootConstants);

		_RenameHLSLIntrinsicCollisions(compiler);
		hlslOut = compiler.compile();
	}
	catch (const std::exception& e)
	{
		logOut = std::string("SPIRV-Cross: ") + e.what();
		return false;
	}
	if (hlslOut.empty())
	{
		logOut = "SPIRV-Cross produced no output";
		return false;
	}
	if (stage == D3D12Const::Stage::Geometry)
		_FixupGeometryShaderPositionInput(hlslOut);
	return true;
}
