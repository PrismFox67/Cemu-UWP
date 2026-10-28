// Draw path of the D3D12 renderer: uniform data, descriptor tables, samplers, pipeline binding and draw submission.
// Structure and register interpretation follow VulkanRendererCore.cpp.

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/RendererCore.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"

#ifdef CEMU_D3D12_SPIRV_TO_DXIL
extern "C"
{
#include "spirv_to_dxil.h"
}
#endif

extern bool hasValidFramebufferAttached;

#ifdef CEMU_D3D12_SPIRV_TO_DXIL
static_assert(sizeof(dxil_spirv_vertex_runtime_data) <= D3D12Const::kRuntimeDataDwords * 4, "runtime data root constants too small");
#endif

/* --- sequence handling --- */

void D3D12Renderer::draw_beginSequence()
{
	ServicePendingPresent(false); // frame interpolation without vsync: the previous real frame may be due
	LatteTC_ReleaseForMemoryPressureIfDue();
	m_state.skipDrawSequence = false;
	const bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;

	LatteSHRC_UpdateActiveShaders();
	if (LatteGPUState.activeShaderHasError)
	{
		cemuLog_logDebugOnce(LogType::Force, "Skipping drawcalls due to shader error");
		m_state.skipDrawSequence = true;
		return;
	}

	LatteGPUState.requiresTextureBarrier = false;
	while (true)
	{
		LatteGPUState.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO())
		{
			m_state.skipDrawSequence = true;
			return;
		}
		if (!hasValidFramebufferAttached && !streamoutEnable)
		{
			m_state.skipDrawSequence = true;
			return;
		}
		LatteTexture_updateTextures();
		if (!LatteGPUState.repeatTextureInitialization)
			break;
	}
	LatteMRT::ApplyCurrentState();
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();

	bool rasterizerEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() == false;
	if (!LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerEnable = true;
	if (!rasterizerEnable && !streamoutEnable)
		m_state.skipDrawSequence = true;
}

void D3D12Renderer::draw_endSequence()
{
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	if (pixelShader)
		LatteRenderTarget_trackUpdates();
	const bool hasReadback = LatteTextureReadback_Update();
	if (m_drawsInCommandList >= m_submitThreshold || hasReadback || m_uploadRing->GetRecordingOverflowBytes() > 0)
		SubmitCommandList(false);
}

void D3D12Renderer::draw_handleSpecialState5()
{
	LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();
	LatteTextureView* colorBuffer = LatteMRT::GetColorAttachment(0);
	LatteTextureView* depthBuffer = LatteMRT::GetDepthAttachment();
	if (!colorBuffer || !depthBuffer)
		return;
	sint32 vpWidth, vpHeight;
	LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);
	surfaceCopy_copySurfaceWithFormatConversion(
		depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
		colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
		vpWidth, vpHeight);
}

/* --- uniforms --- */

bool D3D12Renderer::draw_prepareUniformVars(LatteDecompilerShader* shader, D3D12_GPU_VIRTUAL_ADDRESS& addressOut, uint32& sizeOut)
{
	addressOut = 0;
	sizeOut = 0;
	if (shader->resourceMapping.uniformVarsBufferBindingPoint < 0)
		return false;
	const uint32 rangeSize = shader->uniform.uniformRangeSize;
	if (m_uniformScratch.size() < AlignUp<uint32>(rangeSize, 16))
		m_uniformScratch.resize(AlignUp<uint32>(rangeSize, 16) + 256);
	float* uniformBuf = (float*)m_uniformScratch.data();
	auto GET_UNIFORM_DATA_PTR = [uniformBuf](size_t index) { return uniformBuf + (index / 4); };

	for (auto& entry : shader->uniform.list_ufTexRescale)
	{
		float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
		memcpy(entry.currentValue, xyScale, sizeof(float) * 2);
		memcpy(GET_UNIFORM_DATA_PTR(entry.uniformLocation), xyScale, sizeof(float) * 2);
	}
	if (shader->uniform.loc_alphaTestRef >= 0)
		*GET_UNIFORM_DATA_PTR(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
	if (shader->uniform.loc_pointSize >= 0)
	{
		const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
		float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
		if (pointWidth == 0.0f)
			pointWidth = 1.0f / 8.0f;
		*GET_UNIFORM_DATA_PTR(shader->uniform.loc_pointSize) = pointWidth;
	}
	if (shader->uniform.loc_remapped >= 0)
		LatteBufferCache_LoadRemappedUniforms(shader, GET_UNIFORM_DATA_PTR(shader->uniform.loc_remapped), true, (1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
	if (shader->uniform.loc_uniformRegister >= 0)
	{
		sint32 shaderAluConst = 0;
		if (shader->shaderType == LatteConst::ShaderType::Vertex)
			shaderAluConst = 0x400;
		uint32* uniformRegData = (uint32*)(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
		memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
	}
	if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
	{
		sint32 viewportWidth, viewportHeight;
		LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight);
		float* v = GET_UNIFORM_DATA_PTR(shader->uniform.loc_windowSpaceToClipSpaceTransform);
		v[0] = 2.0f / (float)viewportWidth;
		v[1] = 2.0f / (float)viewportHeight;
	}
	if (shader->uniform.loc_fragCoordScale >= 0)
		LatteMRT::GetCurrentFragCoordScale(GET_UNIFORM_DATA_PTR(shader->uniform.loc_fragCoordScale));
	if (shader->uniform.loc_verticesPerInstance >= 0)
	{
		*(int*)GET_UNIFORM_DATA_PTR(shader->uniform.loc_verticesPerInstance) = (int)m_state.verticesPerInstance;
		for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
		{
			if (shader->uniform.loc_streamoutBufferBase[b] >= 0)
				*(uint32*)GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferBase[b]) = m_state.streamoutBuffer[b].ringBufferOffset;
		}
	}
	D3D12UploadAllocation upload = AllocateUpload(AlignUp<uint32>(std::max<uint32>(rangeSize, 16), D3D12Const::kCBVAlignment), D3D12Const::kCBVAlignment);
	memcpy(upload.cpuPtr, uniformBuf, rangeSize);
	addressOut = upload.gpuAddress;
	sizeOut = rangeSize;
	return true;
}

/* --- samplers --- */

void D3D12Renderer::draw_buildSamplerDesc(LatteDecompilerShader* shader, uint32 relativeTextureUnit, LatteTextureViewD3D12* view, D3D12_SAMPLER_DESC& desc)
{
	desc = {};
	desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	desc.MaxAnisotropy = 1;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	desc.MinLOD = 0.0f;
	desc.MaxLOD = 0.0f; // matches the zero initialized VkSamplerCreateInfo the Vulkan backend uses when no sampler is assigned

	const uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relativeTextureUnit];
	if (stageSamplerIndex == LATTE_DECOMPILER_SAMPLER_NONE)
		return;
	LatteTextureD3D12* baseTexture = view ? view->GetBaseTexture() : nullptr;
	const uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
	const _LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;

	// LOD
	const uint32 iMinLOD = samplerWords->WORD1.get_MIN_LOD();
	const uint32 iMaxLOD = samplerWords->WORD1.get_MAX_LOD();
	sint32 iLodBias = samplerWords->WORD1.get_LOD_BIAS();
	if (baseTexture)
	{
		if (baseTexture->overwriteInfo.hasRelativeLodBias)
			iLodBias += baseTexture->overwriteInfo.relativeLodBias;
		if (baseTexture->overwriteInfo.hasLodBias)
			iLodBias = baseTexture->overwriteInfo.lodBias;
	}
	using ZF = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER;
	using XYF = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER;
	const auto filterMip = samplerWords->WORD0.get_MIP_FILTER();
	D3D12_FILTER_TYPE mipFilter = D3D12_FILTER_TYPE_LINEAR;
	if (filterMip == ZF::NONE)
	{
		mipFilter = D3D12_FILTER_TYPE_POINT;
		desc.MinLOD = 0.0f;
		desc.MaxLOD = 0.25f;
	}
	else
	{
		mipFilter = (filterMip == ZF::POINT) ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
		desc.MinLOD = (float)iMinLOD / 64.0f;
		desc.MaxLOD = (float)iMaxLOD / 64.0f;
	}
	const auto filterMin = samplerWords->WORD0.get_XY_MIN_FILTER();
	const auto filterMag = samplerWords->WORD0.get_XY_MAG_FILTER();
	const D3D12_FILTER_TYPE minFilter = (filterMin == XYF::POINT || filterMin == XYF::ANISO_POINT) ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
	const D3D12_FILTER_TYPE magFilter = (filterMag == XYF::POINT || filterMag == XYF::ANISO_POINT) ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;

	desc.AddressU = D3D12Format::GetAddressMode(samplerWords->WORD0.get_CLAMP_X());
	desc.AddressV = D3D12Format::GetAddressMode(samplerWords->WORD0.get_CLAMP_Y());
	desc.AddressW = D3D12Format::GetAddressMode(samplerWords->WORD0.get_CLAMP_Z());

	sint32 maxAniso = samplerWords->WORD0.get_MAX_ANISO_RATIO();
	if (baseTexture && baseTexture->overwriteInfo.anisotropicLevel >= 0)
		maxAniso = baseTexture->overwriteInfo.anisotropicLevel;
	desc.MipLODBias = std::clamp((float)iLodBias / 64.0f, -16.0f, 15.99f);

	const bool depthCompare = shader->textureUsesDepthCompare[relativeTextureUnit];
	const D3D12_FILTER_REDUCTION_TYPE reduction = depthCompare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
	if (depthCompare)
		desc.ComparisonFunc = D3D12Format::GetCompareFunc((uint32)samplerWords->WORD0.get_DEPTH_COMPARE_FUNCTION());

	if (maxAniso > 0 && minFilter == D3D12_FILTER_TYPE_LINEAR)
	{
		desc.Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
		desc.MaxAnisotropy = std::min<UINT>(1u << maxAniso, 16);
	}
	else
	{
		desc.Filter = D3D12_ENCODE_BASIC_FILTER(minFilter, magFilter, mipFilter, reduction);
		desc.MaxAnisotropy = 1;
	}

	// border color. D3D12 supports arbitrary border colors natively
	const auto borderType = samplerWords->WORD0.get_BORDER_COLOR_TYPE();
	using BT = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE;
	if (borderType == BT::TRANSPARENT_BLACK)
	{
		desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 0.0f;
	}
	else if (borderType == BT::OPAQUE_BLACK)
	{
		desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = 0.0f;
		desc.BorderColor[3] = 1.0f;
	}
	else if (borderType == BT::OPAQUE_WHITE)
	{
		desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 1.0f;
	}
	else
	{
		_LatteRegisterSetSamplerBorderColor* borderColorReg;
		if (shader->shaderType == LatteConst::ShaderType::Vertex)
			borderColorReg = LatteGPUState.contextNew.TD_VS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		else if (shader->shaderType == LatteConst::ShaderType::Pixel)
			borderColorReg = LatteGPUState.contextNew.TD_PS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		else
			borderColorReg = LatteGPUState.contextNew.TD_GS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
		desc.BorderColor[0] = borderColorReg->red.get_channelValue();
		desc.BorderColor[1] = borderColorReg->green.get_channelValue();
		desc.BorderColor[2] = borderColorReg->blue.get_channelValue();
		desc.BorderColor[3] = borderColorReg->alpha.get_channelValue();
	}
}

/* --- descriptor tables --- */

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::WriteStageTable(const D3D12StageTableDesc& tableDesc)
{
	using namespace D3D12Const;
	auto table = m_gpuViewHeap->AllocateTable(kStageTableSize);
	for (uint32 i = 0; i < kMaxCBVsPerStage; i++)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE dst = m_gpuViewHeap->OffsetCPU(table.cpu, kStageTableOffsetCBV + i);
		if (tableDesc.cbvAddress[i])
			WriteCBV(dst, tableDesc.cbvAddress[i], tableDesc.cbvSize[i]);
		else
			m_device->CopyDescriptorsSimple(1, dst, m_nullCBV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
	for (uint32 i = 0; i < kMaxSRVsPerStage; i++)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE dst = m_gpuViewHeap->OffsetCPU(table.cpu, kStageTableOffsetSRV + i);
		m_device->CopyDescriptorsSimple(1, dst, tableDesc.srv[i].ptr ? tableDesc.srv[i] : m_nullSRV2D, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
	D3D12_CPU_DESCRIPTOR_HANDLE uavDst = m_gpuViewHeap->OffsetCPU(table.cpu, kStageTableOffsetUAV);
	if (tableDesc.uavResource)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = tableDesc.uavSize / 4;
		uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		m_device->CreateUnorderedAccessView(tableDesc.uavResource, nullptr, &uav, uavDst);
	}
	else
		m_device->CopyDescriptorsSimple(1, uavDst, m_nullUAV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return table.gpu;
}

static LatteTextureViewD3D12* _GetBoundTextureForShader(LatteTextureViewD3D12** boundTextures, LatteDecompilerShader* shader, uint32 relativeTextureUnit, uint32& texUnitRegIndexOut)
{
	uint32 hostTextureUnit = relativeTextureUnit;
	uint32 texUnitRegIndex = relativeTextureUnit * 7;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
		texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
		texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
		texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
		break;
	default:
		break;
	}
	texUnitRegIndexOut = texUnitRegIndex;
	return boundTextures[hostTextureUnit];
}

void D3D12Renderer::draw_transitionInputTextures(LatteDecompilerShader* shader)
{
	const sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (sint32 i = 0; i < textureCount; i++)
	{
		const uint32 relativeTextureUnit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
		uint32 regIndex;
		LatteTextureViewD3D12* view = _GetBoundTextureForShader(m_state.boundTexture, shader, relativeTextureUnit, regIndex);
		if (!view)
			continue;
		TransitionTexture(view->GetBaseTexture(), view->firstMip, view->numMip, view->firstSlice, view->numSlice, LatteTextureD3D12::kShaderReadState);
	}
}

// #398/#402: does the sampled view overlap a render target of the active FBO? (DanP's ResolveFeedbackLoopsForDraw).
// Depth is only counted when the draw can write it; read-only depth is handled with a read-only DSV (#394)
static bool _ViewsOverlap(LatteTextureViewD3D12* sampled, LatteTextureViewD3D12* attachment)
{
	if (!attachment || sampled->GetBaseTexture() != attachment->GetBaseTexture())
		return false;
	const bool mipOverlap = attachment->firstMip >= sampled->firstMip && attachment->firstMip < sampled->firstMip + sampled->numMip;
	const bool sliceOverlap = sampled->firstSlice < attachment->firstSlice + attachment->numSlice && attachment->firstSlice < sampled->firstSlice + sampled->numSlice;
	return mipOverlap && sliceOverlap;
}

static bool _IsFeedbackLoop(CachedFBOD3D12* fbo, LatteTextureViewD3D12* view, bool& isDepth)
{
	isDepth = false;
	if (!fbo)
		return false;
	for (uint32 i = 0; i < 8; i++)
	{
		if (fbo->GetRTVFormat(i) != DXGI_FORMAT_UNKNOWN && _ViewsOverlap(view, fbo->GetColorView(i)))
			return true;
	}
	if (fbo->HasDSV() && _ViewsOverlap(view, fbo->GetDepthView()))
	{
		const auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
		isDepth = true;
		return depthControl.get_Z_WRITE_ENABLE() || depthControl.get_STENCIL_ENABLE();
	}
	return false;
}

void D3D12Renderer::draw_buildStageBindings(LatteDecompilerShader* shader, RendererShaderD3D12* hostShader, const D3D12_GPU_VIRTUAL_ADDRESS uniformVarAddress, uint32 uniformVarSize, PreparedStageBindings& out)
{
	using namespace D3D12Const;
	const Stage stage = D3D12StageFromLatte(shader->shaderType);
	const D3D12BindingRemap& remap = hostShader->GetBindingRemap();
	auto& mapping = shader->resourceMapping; // accessors are not const
	out.stage = stage;
	out.tableDesc = D3D12StageTableDesc();
	D3D12StageTableDesc& tableDesc = out.tableDesc;

	// uniform var block + uniform buffers
	if (mapping.uniformVarsBufferBindingPoint >= 0)
	{
		const sint32 reg = remap.GetCBV(mapping.uniformVarsBufferBindingPoint);
		if (reg >= 0)
		{
			tableDesc.cbvAddress[reg] = uniformVarAddress;
			tableDesc.cbvSize[reg] = uniformVarSize;
		}
	}
	const auto& stageBindings = m_state.stage[(uint32)stage];
	for (uint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
	{
		if (mapping.uniformBuffersBindingPoint[i] < 0)
			continue;
		const sint32 reg = remap.GetCBV(mapping.uniformBuffersBindingPoint[i]);
		if (reg < 0 || !stageBindings.uniformBufferAddress[i])
			continue;
		tableDesc.cbvAddress[reg] = stageBindings.uniformBufferAddress[i];
		tableDesc.cbvSize[reg] = std::min<uint32>(stageBindings.uniformBufferSize[i], D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);
	}

	// textures + samplers
	D3D12_SAMPLER_DESC* samplers = out.samplers;
	for (uint32 i = 0; i < kMaxSamplersPerStage; i++)
	{
		auto& s = samplers[i];
		s = {};
		s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.MaxAnisotropy = 1;
		s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	}
	out.samplerCount = std::max<uint32>(remap.srvCount, 1);
	out.samplerTable = {};
	const sint32 textureBase = mapping.getTextureBaseBindingPoint();
	const sint32 textureCount = mapping.getTextureCount();
	for (sint32 i = 0; i < textureCount && textureBase >= 0; i++)
	{
		const sint32 reg = remap.GetSRV(textureBase + i);
		if (reg < 0)
			continue;
		const uint32 relativeTextureUnit = mapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
		const Latte::E_DIM shaderDim = shader->textureUnitDim[relativeTextureUnit];
		uint32 texUnitRegIndex;
		LatteTextureViewD3D12* view = _GetBoundTextureForShader(m_state.boundTexture, shader, relativeTextureUnit, texUnitRegIndex);
		bool dimMismatch = false;
		if (view)
		{
			if (shaderDim == Latte::E_DIM::DIM_1D && view->dim != Latte::E_DIM::DIM_1D)
				dimMismatch = true;
			else if (shaderDim == Latte::E_DIM::DIM_2D && view->dim != Latte::E_DIM::DIM_2D && view->dim != Latte::E_DIM::DIM_2D_MSAA)
				dimMismatch = true;
		}
		if (!view || dimMismatch)
		{
			tableDesc.srv[reg] = GetNullSRV(shaderDim);
		}
		else
		{
			const uint32 word4 = LatteGPUState.contextRegister[texUnitRegIndex + 4];
			bool feedbackDepth;
			if (_IsFeedbackLoop(m_state.activeFBO, view, feedbackDepth))
			{
				// the copy is off unless <user>/feedback_copy.txt exists: BOTW makes ~3600 such samples per second on PC
				// (whole-texture copies, cost unknown on Xbox) and the effect on rune targeting isn't confirmed yet
				static bool s_copyEnabled = false;
				static uint32 s_copyCheck = 0;
				if ((s_copyCheck++ % 4096) == 0)
				{
					std::error_code ec;
					const bool on = fs::exists(ActiveSettings::GetUserDataPath("feedback_copy.txt"), ec);
					if (on != s_copyEnabled || s_copyCheck == 1)
						cemuLog_log(LogType::Force, "D3D12: feedback copies {}", on ? "on (feedback_copy.txt)" : "off, render targets are sampled directly");
					s_copyEnabled = on;
				}
				tableDesc.srv[reg] = s_copyEnabled ? view->GetFeedbackSRV(word4) : view->GetSRV(word4);
				static uint32 s_copies = 0, s_depthCopies = 0, s_logged = 0;
				static auto s_nextLog = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				s_copies++;
				if (feedbackDepth)
					s_depthCopies++;
				if (s_logged < 32)
				{
					s_logged++;
					cemuLog_log(LogType::Force, "D3D12: feedback loop: {} shader {:016x} samples {:08x} {}x{} fmt {:04x} mip {} slice {} while rendering to it",
						feedbackDepth ? "depth" : "colour", shader->baseHash, view->GetBaseTexture()->physAddress, view->GetBaseTexture()->width, view->GetBaseTexture()->height, (uint32)view->format, view->firstMip, view->firstSlice);
				}
				if (const auto now = std::chrono::steady_clock::now(); now >= s_nextLog)
				{
					s_nextLog = now + std::chrono::seconds(10);
					cemuLog_log(LogType::Force, "D3D12: feedback loops in the last 10 s: {} ({} depth)", s_copies, s_depthCopies);
					s_copies = s_depthCopies = 0;
				}
			}
			else
				tableDesc.srv[reg] = view->GetSRV(word4);
		}
		draw_buildSamplerDesc(shader, relativeTextureUnit, view, samplers[reg]);
	}

	// transform feedback via UAV
	if (mapping.tfStorageBindingPoint >= 0 && remap.GetUAV(mapping.tfStorageBindingPoint) == 0)
	{
		tableDesc.uavResource = m_xfbRingBuffer.Get();
		tableDesc.uavSize = m_xfbRingBufferSize;
	}
}

void D3D12Renderer::draw_acquireSamplerTables(PreparedStageBindings* stages, uint32 count)
{
	// all tables of a draw have to come from the same generation of the sampler heap
	for (int attempt = 0; attempt < 2; attempt++)
	{
		bool ok = true;
		for (uint32 i = 0; i < count && ok; i++)
			ok = TryGetSamplerTable(stages[i].samplers, stages[i].samplerCount, stages[i].samplerTable);
		if (ok)
			return;
		cemuLog_logDebug(LogType::Force, "D3D12: Sampler heap full, resetting");
		SubmitCommandList(true);
		m_gpuSamplerHeap->Reset();
	}
	cemu_assert(false); // a single draw can never exceed the heap
}

/* --- render targets --- */

// #394 BOTW Magnesis/Stasis: a draw can sample the depth buffer that is also bound as its DSV (depth test on, no
// writes). Forcing that texture to DEPTH_WRITE makes the shader read undefined data (stale/compressed depth on AMD), so
// such draws get a read-only DSV and the texture stays readable (DEPTH_READ | shader resource). Vulkan uses a read-only
// depth layout for this, DanP's D3D12 renderer samples a copy
bool D3D12Renderer::draw_samplesBoundDepth(LatteDecompilerShader* shader, LatteTextureViewD3D12* depthView)
{
	if (!shader)
		return false;
	const sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (sint32 i = 0; i < textureCount; i++)
	{
		const uint32 relativeTextureUnit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
		uint32 regIndex;
		LatteTextureViewD3D12* view = _GetBoundTextureForShader(m_state.boundTexture, shader, relativeTextureUnit, regIndex);
		if (!view || view->GetBaseTexture() != depthView->GetBaseTexture())
			continue;
		const bool mipOverlap = depthView->firstMip >= view->firstMip && depthView->firstMip < view->firstMip + view->numMip;
		const bool sliceOverlap = view->firstSlice < depthView->firstSlice + depthView->numSlice && depthView->firstSlice < view->firstSlice + view->numSlice;
		if (mipOverlap && sliceOverlap)
			return true;
	}
	return false;
}

void D3D12Renderer::draw_applyRenderTargets(bool depthReadOnly)
{
	CachedFBOD3D12* fbo = m_state.activeFBO;
	for (uint32 i = 0; i < 8; i++)
	{
		LatteTextureViewD3D12* view = fbo->GetColorView(i);
		if (!view || fbo->GetRTVFormat(i) == DXGI_FORMAT_UNKNOWN)
			continue;
		TransitionTexture(view->GetBaseTexture(), view->firstMip, 1, view->firstSlice, view->numSlice, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	LatteTextureViewD3D12* depthView = fbo->GetDepthView();
	if (!depthView || !fbo->HasDSV())
		depthReadOnly = false;
	if (depthView)
		TransitionTexture(depthView->GetBaseTexture(), depthView->firstMip, 1, depthView->firstSlice, depthView->numSlice,
			depthReadOnly ? (D3D12_RESOURCE_STATES)((UINT)D3D12_RESOURCE_STATE_DEPTH_READ | (UINT)LatteTextureD3D12::kShaderReadState) : D3D12_RESOURCE_STATE_DEPTH_WRITE);
	if (depthReadOnly != m_state.depthReadOnlyBound)
		m_state.renderTargetsDirty = true;
	if (!m_state.renderTargetsDirty)
		return;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = depthReadOnly ? depthView->GetReadOnlyDSV() : fbo->GetDSV();
	m_cmdList->OMSetRenderTargets(fbo->GetNumRTVs(), fbo->GetRTVs(), FALSE, fbo->HasDSV() ? &dsv : nullptr);
	m_state.depthReadOnlyBound = depthReadOnly;
	m_state.renderTargetsDirty = false;
}

/* --- draw --- */

void D3D12Query_CountDraw(bool issued); // D3D12Query.cpp, diagnostic

// diagnostic (#286/#290, BOTW Magnesis grab on Series: every query reads 0 while all its draws reach the GPU): the
// state the draws inside occlusion queries run with - depth/stencil test, the depth buffer they test against, render
// targets, viewport and scissor. Only while the last window's reads were >= 90% zero; each distinct state once per
// 10 s, at most 8 per window
bool D3D12Query_MostlyZero(); // D3D12Query.cpp
static void D3D12Query_LogDrawState(LatteDecompilerShader* vs, LatteDecompilerShader* ps, CachedFBOD3D12* fbo, const D3D12_VIEWPORT& vp, const D3D12_RECT& sc, uint32 count, uint32 instances)
{
	static auto s_windowStart = std::chrono::steady_clock::now();
	static std::unordered_set<uint64> s_seen;
	const auto now = std::chrono::steady_clock::now();
	if (now - s_windowStart > std::chrono::seconds(10))
	{
		s_windowStart = now;
		s_seen.clear();
	}
	if (s_seen.size() >= 8 || !D3D12Query_MostlyZero())
		return;
	const auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
	LatteTextureViewD3D12* depthView = fbo->HasDSV() ? fbo->GetDepthView() : nullptr;
	LatteTexture* depthTex = depthView ? depthView->baseTexture : nullptr;
	uint64 key = vs->baseHash ^ (ps ? ps->baseHash * 31 : 0) ^ ((uint64)LatteGPUState.contextRegister[Latte::REGADDR::DB_DEPTH_CONTROL] << 40) ^ (uint64)(depthTex ? depthTex->physAddress : 0) * 7;
	key ^= ((uint64)(uint32)vp.Width << 20) ^ ((uint64)(uint32)vp.Height << 8) ^ fbo->GetNumRTVs();
	if (!s_seen.insert(key).second)
		return;
	cemuLog_log(LogType::Force, "D3D12: query draw vs {:016x} ps {:016x} {}x{}: depth test {} func {} write {} stencil {}; depth buffer {} {:08x} {}x{} fmt {:04x} dxgi {}; {} RTVs; viewport {:.0f},{:.0f} {:.0f}x{:.0f} z {:.2f}-{:.2f}, scissor {},{}-{},{}",
		vs->baseHash, ps ? ps->baseHash : 0, count, instances,
		depthControl.get_Z_ENABLE(), (uint32)depthControl.get_Z_FUNC(), depthControl.get_Z_WRITE_ENABLE(), depthControl.get_STENCIL_ENABLE(),
		depthTex ? "at" : "none", depthTex ? depthTex->physAddress : 0, depthTex ? depthTex->width : 0, depthTex ? depthTex->height : 0, depthTex ? (uint32)depthTex->format : 0, (uint32)fbo->GetDSVFormat(),
		fbo->GetNumRTVs(), vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth, sc.left, sc.top, sc.right, sc.bottom);
	const auto& lcr = LatteGPUState.contextNew;
	cemuLog_log(LogType::Force, "D3D12: query draw raster: cull front {} back {} front face {}; poly offset {} (offset {} scale {} clamp {}); zclip disabled {}, dx clip space {}, clip cntl {:08x}, vte {:08x}",
		lcr.PA_SU_SC_MODE_CNTL.get_CULL_FRONT(), lcr.PA_SU_SC_MODE_CNTL.get_CULL_BACK(), (uint32)lcr.PA_SU_SC_MODE_CNTL.get_FRONT_FACE(),
		lcr.PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED(), lcr.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET(), lcr.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE(), lcr.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP(),
		lcr.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE(), lcr.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF(),
		LatteGPUState.contextRegister[Latte::REGADDR::PA_CL_CLIP_CNTL], LatteGPUState.contextRegister[Latte::REGADDR::PA_CL_VTE_CNTL]);
	if (depthView)
		static_cast<D3D12Renderer*>(g_renderer.get())->LogDepthContents(depthView, vp);
}

void D3D12Renderer::LogDepthContents(LatteTextureViewD3D12* depthView, const D3D12_VIEWPORT& vp)
{
	LatteTextureD3D12* tex = depthView->GetBaseTexture();
	const D3D12_RESOURCE_DESC& desc = tex->GetDesc();
	if (!tex->GetResource() || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
		return;
	const uint32 sub = tex->GetSubresourceIndex(depthView->firstMip, depthView->firstSlice, 0);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	UINT numRows;
	UINT64 rowSize, totalBytes;
	m_device->GetCopyableFootprints(&desc, sub, 1, 0, &layout, &numRows, &rowSize, &totalBytes);
	const uint32 bpp = (uint32)(rowSize / std::max<uint32>(layout.Footprint.Width, 1));
	if (bpp != 4 && bpp != 2)
	{
		cemuLog_log(LogType::Force, "D3D12: query depth readback: unsupported footprint ({} bytes per texel, format {})", bpp, (uint32)layout.Footprint.Format);
		return;
	}
	const uint64 offset = m_readbackRing->Allocate(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	TransitionTexture(tex, depthView->firstMip, 1, depthView->firstSlice, 1, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushBarriers();
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = tex->GetResource();
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = sub;
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = m_readbackRing->GetResource();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = layout;
	dst.PlacedFootprint.Offset = offset;
	m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	TransitionTexture(tex, depthView->firstMip, 1, depthView->firstSlice, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	FlushBarriers();
	m_hasRecordedWork = true;
	const uint64 id = GetRecordingSubmissionId();
	SubmitCommandList(false);
	WaitForSubmission(id);

	// texels inside the viewport: min, max, how many are exactly 0 / 1, and a 4x4 grid of samples
	const uint8* data = m_readbackRing->GetCPUPtr(offset);
	const uint32 x0 = (uint32)std::max(0.0f, vp.TopLeftX), y0 = (uint32)std::max(0.0f, vp.TopLeftY);
	const uint32 x1 = std::min<uint32>((uint32)(vp.TopLeftX + vp.Width), layout.Footprint.Width);
	const uint32 y1 = std::min<uint32>((uint32)(vp.TopLeftY + vp.Height), numRows);
	auto texel = [&](uint32 x, uint32 y) -> float {
		const uint8* p = data + (uint64)y * layout.Footprint.RowPitch + (uint64)x * bpp;
		if (bpp == 2)
			return *(const uint16*)p / 65535.0f;
		if (layout.Footprint.Format == DXGI_FORMAT_R24G8_TYPELESS || layout.Footprint.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || layout.Footprint.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
			return (*(const uint32*)p & 0xFFFFFF) / 16777215.0f;
		return *(const float*)p;
	};
	float mn = 1e30f, mx = -1e30f;
	uint64 zeros = 0, ones = 0, total = 0;
	for (uint32 y = y0; y < y1; y++)
		for (uint32 x = x0; x < x1; x++)
		{
			const float v = texel(x, y);
			mn = std::min(mn, v);
			mx = std::max(mx, v);
			zeros += v == 0.0f;
			ones += v == 1.0f;
			total++;
		}
	std::string grid;
	for (uint32 gy = 0; gy < 4 && y1 > y0; gy++)
		for (uint32 gx = 0; gx < 4 && x1 > x0; gx++)
			grid += fmt::format("{}{:.5f}", grid.empty() ? "" : " ", texel(x0 + (x1 - x0) * (2 * gx + 1) / 8, y0 + (y1 - y0) * (2 * gy + 1) / 8));
	cemuLog_log(LogType::Force, "D3D12: query depth {:08x} (format {}, {}x{}): {} texels in viewport, min {:.5f} max {:.5f}, {} are 0, {} are 1; 4x4 grid: {}",
		tex->physAddress, (uint32)layout.Footprint.Format, layout.Footprint.Width, numRows, total, mn, mx, zeros, ones, grid);
}

void D3D12Renderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
	LatteGPUState.drawCallCounter++;
	if (LatteQuery_IsOcclusionQueryActive())
		D3D12Query_CountDraw(false);
	if (m_state.skipDrawSequence)
		return;
	if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		return;
	}
	if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
	{
		draw_handleSpecialState5();
		return;
	}

	m_state.verticesPerInstance = count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);

	LatteFetchShader* fetchShader = LatteSHRC_GetActiveFetchShader();
	LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
	if (!vertexShader || !fetchShader)
		return;

	// uniform variable blocks (uploaded once per draw)
	D3D12_GPU_VIRTUAL_ADDRESS ufAddress[3]{};
	uint32 ufSize[3]{};
	if (vertexShader)
		draw_prepareUniformVars(vertexShader, ufAddress[0], ufSize[0]);
	if (pixelShader)
		draw_prepareUniformVars(pixelShader, ufAddress[1], ufSize[1]);
	if (geometryShader)
		draw_prepareUniformVars(geometryShader, ufAddress[2], ufSize[2]);

	// indices (decoded and cached by LatteIndices, stored in upload memory)
	const LattePrimitiveMode primitiveMode = static_cast<LattePrimitiveMode>(LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE]);
	Renderer::INDEX_TYPE hostIndexType;
	uint32 hostIndexCount;
	uint32 indexMax = 0;
	Renderer::IndexAllocation indexAllocation;
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMax, hostIndexType, hostIndexCount, indexAllocation);

	// vertex and uniform buffer cache (may record uploads)
	uint8 stageUniformModifiedMask = 0;
	LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, stageUniformModifiedMask);

	CachedFBOD3D12* fbo = m_state.activeFBO;
	if (!fbo)
		return;

	D3D12PipelineInfo* pipeline = m_pipelineCache->GetOrCreate(fetchShader, vertexShader, geometryShader, pixelShader, fbo, LatteGPUState.contextNew, hostIndexType);
	if (!pipeline->isValid())
		return;

	// descriptor content. Sampler tables are acquired first because a full sampler heap forces a submit, which would
	// discard any command list state recorded for this draw
	PreparedStageBindings stageBindings[3];
	uint32 stageBindingCount = 0;
	draw_buildStageBindings(vertexShader, static_cast<RendererShaderD3D12*>(vertexShader->shader), ufAddress[0], ufSize[0], stageBindings[stageBindingCount++]);
	if (pixelShader)
		draw_buildStageBindings(pixelShader, static_cast<RendererShaderD3D12*>(pixelShader->shader), ufAddress[1], ufSize[1], stageBindings[stageBindingCount++]);
	if (geometryShader)
		draw_buildStageBindings(geometryShader, static_cast<RendererShaderD3D12*>(geometryShader->shader), ufAddress[2], ufSize[2], stageBindings[stageBindingCount++]);
	draw_acquireSamplerTables(stageBindings, stageBindingCount);

	// resource states. Inputs first so render targets win if a texture is both sampled and rendered to
	if (vertexShader)
		draw_transitionInputTextures(vertexShader);
	if (pixelShader)
		draw_transitionInputTextures(pixelShader);
	if (geometryShader)
		draw_transitionInputTextures(geometryShader);
	bool depthReadOnly = false;
	if (LatteTextureViewD3D12* depthView = fbo->GetDepthView(); depthView && fbo->HasDSV() &&
		(draw_samplesBoundDepth(pixelShader, depthView) || draw_samplesBoundDepth(vertexShader, depthView) || draw_samplesBoundDepth(geometryShader, depthView)))
	{
		const auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
		// a read-only DSV can't take depth or stencil writes; those draws keep the old (undefined) behaviour and are logged
		depthReadOnly = !depthControl.get_Z_WRITE_ENABLE() && !depthControl.get_STENCIL_ENABLE();
		static uint32 s_feedbackLogs = 0;
		if (s_feedbackLogs < 128)
		{
			s_feedbackLogs++;
			cemuLog_log(LogType::Force, "D3D12: depth feedback VS {:016x} PS {:016x}: samples bound depth {:08x} {}x{} mip {} (z {} write {} stencil {}) -> {}",
				vertexShader->baseHash, pixelShader ? pixelShader->baseHash : 0, depthView->GetBaseTexture()->physAddress, depthView->GetBaseTexture()->width, depthView->GetBaseTexture()->height, depthView->firstMip,
				depthControl.get_Z_ENABLE(), depthControl.get_Z_WRITE_ENABLE(), depthControl.get_STENCIL_ENABLE(), depthReadOnly ? "read-only DSV" : "NOT handled (writes)");
		}
	}
	draw_applyRenderTargets(depthReadOnly);
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_COPY_SOURCE);
	const bool usesStreamout = vertexShader->resourceMapping.tfStorageBindingPoint >= 0;
	if (usesStreamout)
		TransitionResource(m_xfbRingBuffer.Get(), m_xfbRingBufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	FlushBarriers();

	// pipeline state
	if (!m_state.rootSignatureBound)
		BindRootSignatureAndHeaps();
	if (m_state.currentPSO != pipeline->pso.Get())
	{
		m_cmdList->SetPipelineState(pipeline->pso.Get());
		m_state.currentPSO = pipeline->pso.Get();
	}
	if (m_state.currentTopology != pipeline->topology)
	{
		m_cmdList->IASetPrimitiveTopology(pipeline->topology);
		m_state.currentTopology = pipeline->topology;
	}
	if (m_state.vertexBufferDirtyMask)
	{
		m_cmdList->IASetVertexBuffers(0, LATTE_MAX_VERTEX_BUFFERS, m_state.vertexBuffers);
		m_state.vertexBufferDirtyMask = 0;
	}
	if (m_state.viewportDirty)
	{
		m_cmdList->RSSetViewports(1, &m_state.viewport);
		m_state.viewportDirty = false;
	}
	if (m_state.scissorDirty)
	{
		m_cmdList->RSSetScissorRects(1, &m_state.scissor);
		m_state.scissorDirty = false;
	}
	if (pipeline->usesBlendConstants)
	{
		float blendFactor[4];
		memcpy(blendFactor, LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED, sizeof(blendFactor));
		if (pipeline->blendConstantAlphaOnly)
			blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3];
		if (memcmp(blendFactor, m_state.currentBlendFactor, sizeof(blendFactor)) != 0)
		{
			m_cmdList->OMSetBlendFactor(blendFactor);
			memcpy(m_state.currentBlendFactor, blendFactor, sizeof(blendFactor));
		}
	}
	if (pipeline->usesStencil)
	{
		const uint32 ref = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILREF_F();
		if (ref != m_state.currentStencilRef)
		{
			m_cmdList->OMSetStencilRef(ref);
			m_state.currentStencilRef = ref;
		}
	}

	// index buffer
	const bool isIndexed = hostIndexType != INDEX_TYPE::NONE;
	if (isIndexed)
	{
		auto* allocation = (D3D12UploadHeapAllocator::Allocation*)indexAllocation.rendererInternal;
		if (!allocation)
			return;
		D3D12_INDEX_BUFFER_VIEW ib;
		ib.BufferLocation = allocation->gpuAddress;
		ib.Format = (hostIndexType == INDEX_TYPE::U16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
		ib.SizeInBytes = allocation->size;
		if (memcmp(&ib, &m_state.currentIndexBuffer, sizeof(ib)) != 0)
		{
			m_cmdList->IASetIndexBuffer(&ib);
			m_state.currentIndexBuffer = ib;
		}
	}

	// descriptor tables
	for (uint32 i = 0; i < stageBindingCount; i++)
	{
		const PreparedStageBindings& b = stageBindings[i];
		m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageTable(b.stage), WriteStageTable(b.tableDesc));
		m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageSamplers(b.stage), b.samplerTable);
	}

	// gl_VertexIndex / gl_InstanceIndex include the base vertex/instance in Vulkan but SV_VertexID / SV_InstanceID
	// don't. The translated shaders add these offsets back, see D3D12ShaderTranslate::SPIRVToHLSL
	uint32 runtimeDwords[D3D12Const::kRuntimeDataDwords]{};
#ifdef CEMU_D3D12_SPIRV_TO_DXIL
	if (D3D12ShaderCompiler::GetBackend() == D3D12ShaderCompiler::Backend::SpirvToDXIL)
	{
		dxil_spirv_vertex_runtime_data runtimeData{};
		runtimeData.first_vertex = baseVertex;
		runtimeData.base_instance = baseInstance;
		runtimeData.is_indexed_draw = isIndexed;
		memcpy(runtimeDwords, &runtimeData, sizeof(runtimeData));
	}
	else
#endif
	{
		runtimeDwords[0] = baseVertex; // SPIRV_Cross_BaseVertex
		runtimeDwords[1] = baseInstance; // SPIRV_Cross_BaseInstance
	}
	m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamRuntimeData, D3D12Const::kRuntimeDataDwords, runtimeDwords, 0);

	// #316 BOTW Magnesis/Stasis: the target probes (two draws inside occlusion queries, PS 159823cc52262ba9 = constant
	// color, VS e348d00a1dc9e460 against the main depth, VS a1f4b74d99b962e3 against the small depth copy) fail their
	// LEQUAL test on D3D12 everywhere, although the depth buffers hold the right scene depth and the query results reach
	// the game (forcing them to pass makes the target turn yellow and grab, tested on PC). Until the depth difference is
	// found they are drawn at depth 0 so they always pass: Magnesis/Stasis work, but can also pick a target behind a
	// wall. <user>/magnesis_probe_depth.txt turns it off (for comparing), the other queries are never touched
	if (pixelShader && pixelShader->baseHash == 0x159823cc52262ba9ull && LatteQuery_IsOcclusionQueryActive() &&
		(vertexShader->baseHash == 0xe348d00a1dc9e460ull || vertexShader->baseHash == 0xa1f4b74d99b962e3ull))
	{
		static bool s_probeDepthTest = false;
		static uint32 s_probeCheck = 0;
		if ((s_probeCheck++ % 600) == 0)
		{
			std::error_code ec;
			const bool on = fs::exists(ActiveSettings::GetUserDataPath("magnesis_probe_depth.txt"), ec);
			if (on != s_probeDepthTest || s_probeCheck == 1)
				cemuLog_log(LogType::Force, "D3D12: BOTW target probes {}", on ? "use their depth test (magnesis_probe_depth.txt)" : "always pass");
			s_probeDepthTest = on;
		}
		if (!s_probeDepthTest)
		{
			D3D12_VIEWPORT vp = m_state.viewport;
			vp.MinDepth = vp.MaxDepth = 0.0f;
			m_cmdList->RSSetViewports(1, &vp);
			m_state.viewportDirty = true;
		}
	}

	if (isIndexed)
		m_cmdList->DrawIndexedInstanced(hostIndexCount, instanceCount, 0, (INT)baseVertex, baseInstance);
	else
		m_cmdList->DrawInstanced(count, instanceCount, baseVertex, baseInstance);
	if (LatteQuery_IsOcclusionQueryActive())
	{
		D3D12Query_CountDraw(true);
		D3D12Query_LogDrawState(vertexShader, pixelShader, fbo, m_state.viewport, m_state.scissor, isIndexed ? hostIndexCount : count, instanceCount);
	}

	WriteDrawBreadcrumb(vertexShader->baseHash, pixelShader ? pixelShader->baseHash : 0, isIndexed ? hostIndexCount : count, instanceCount);

	if (usesStreamout)
		UAVBarrier(m_xfbRingBuffer.Get());
	LatteStreamout_FinishDrawcall(false);
	m_drawsInCommandList++;
	m_statDrawsPerFrame++;
	m_hasRecordedWork = true;
}
