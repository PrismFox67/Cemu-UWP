#pragma once

// The one root signature shared by every pipeline (see the binding model in D3D12BindingModel.h).
// Header-only and free of Cemu dependencies so standalone tests can build exactly the same root signature.

#include <d3d12.h>
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12BindingModel.h"

// Serializes the root signature. On failure *errorOut may hold the serializer's message
inline HRESULT D3D12_SerializeCemuRootSignature(ID3DBlob** blobOut, ID3DBlob** errorOut)
{
	using namespace D3D12Const;
	D3D12_DESCRIPTOR_RANGE1 viewRanges[3][3]{};
	D3D12_DESCRIPTOR_RANGE1 samplerRanges[3]{};
	D3D12_ROOT_PARAMETER1 params[kRootParamCount]{};
	static const D3D12_SHADER_VISIBILITY visibility[3] = { D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_GEOMETRY };
	// descriptors are rewritten for every draw, the driver can't make any assumptions about their content
	const D3D12_DESCRIPTOR_RANGE_FLAGS viewFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
	for (uint32 s = 0; s < 3; s++)
	{
		const Stage stage = (Stage)s;
		auto& r = viewRanges[s];
		r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		r[0].NumDescriptors = kMaxCBVsPerStage;
		r[0].BaseShaderRegister = 0;
		r[0].RegisterSpace = RegisterSpace(stage, BindingClass::CBV);
		r[0].Flags = viewFlags;
		r[0].OffsetInDescriptorsFromTableStart = kStageTableOffsetCBV;
		r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		r[1].NumDescriptors = kMaxSRVsPerStage;
		r[1].BaseShaderRegister = 0;
		r[1].RegisterSpace = RegisterSpace(stage, BindingClass::SRV);
		r[1].Flags = viewFlags;
		r[1].OffsetInDescriptorsFromTableStart = kStageTableOffsetSRV;
		r[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		r[2].NumDescriptors = kMaxUAVsPerStage;
		r[2].BaseShaderRegister = 0;
		r[2].RegisterSpace = RegisterSpace(stage, BindingClass::UAV);
		r[2].Flags = viewFlags;
		r[2].OffsetInDescriptorsFromTableStart = kStageTableOffsetUAV;

		auto& tableParam = params[RootParamStageTable(stage)];
		tableParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		tableParam.DescriptorTable.NumDescriptorRanges = 3;
		tableParam.DescriptorTable.pDescriptorRanges = r;
		tableParam.ShaderVisibility = visibility[s];

		auto& sr = samplerRanges[s];
		sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		sr.NumDescriptors = kMaxSamplersPerStage;
		sr.BaseShaderRegister = 0;
		sr.RegisterSpace = RegisterSpace(stage, BindingClass::SRV); // the sampler of a combined image sampler shares space and register number with its texture
		sr.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
		sr.OffsetInDescriptorsFromTableStart = 0;
		auto& samplerParam = params[RootParamStageSamplers(stage)];
		samplerParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		samplerParam.DescriptorTable.NumDescriptorRanges = 1;
		samplerParam.DescriptorTable.pDescriptorRanges = &sr;
		samplerParam.ShaderVisibility = visibility[s];
	}
	auto& runtimeData = params[kRootParamRuntimeData];
	runtimeData.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	runtimeData.Constants.ShaderRegister = 0;
	runtimeData.Constants.RegisterSpace = kSpaceRuntimeData;
	runtimeData.Constants.Num32BitValues = kRuntimeDataDwords;
	runtimeData.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	auto& pushConstants = params[kRootParamPushConstants];
	pushConstants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	pushConstants.Constants.ShaderRegister = 0;
	pushConstants.Constants.RegisterSpace = kSpacePushConstants;
	pushConstants.Constants.Num32BitValues = kPushConstantDwords;
	pushConstants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = kRootParamCount;
	desc.Desc_1_1.pParameters = params;
	desc.Desc_1_1.NumStaticSamplers = 0;
	desc.Desc_1_1.pStaticSamplers = nullptr;
	desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	return D3D12SerializeVersionedRootSignature(&desc, blobOut, errorOut);
}
