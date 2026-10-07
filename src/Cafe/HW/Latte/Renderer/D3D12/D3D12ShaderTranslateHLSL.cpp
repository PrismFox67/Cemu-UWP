#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderTranslate.h"

#include <spirv_cross/spirv_hlsl.hpp>

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
	return true;
}
