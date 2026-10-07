// Draw-based copies: presenting the TV/DRC image to the swap chain and depth <-> color surface copies.
// Both reuse the GLSL shaders written for the Vulkan backend.

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12InternalPipelines.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"


// same GLSL as _vkGenSurfaceCopyShader_vs / _ps_depthToColor / _ps_colorToDepth in VulkanSurfaceCopy.cpp
static const char* s_copySurfaceVS =
	"#version 450\r\n"
	"layout(location = 0) out flat ivec2 passSrcTexelOffset;\r\n"
	"layout(push_constant) uniform pushConstants {\r\n"
	"vec2 vertexOffsets[4];\r\n"
	"ivec2 srcTexelOffset;\r\n"
	"}uf_pushConstants;\r\n"
	"\r\n"
	"void main(){\r\n"
	"vec2 tPOS;\r\n"
	"switch(gl_VertexIndex)"
	"{\r\n"
	"case 0: tPOS = uf_pushConstants.vertexOffsets[0].xy; break;\r\n"
	"case 1: tPOS = uf_pushConstants.vertexOffsets[1].xy; break;\r\n"
	"case 2: tPOS = uf_pushConstants.vertexOffsets[3].xy; break;\r\n"
	"case 3: tPOS = uf_pushConstants.vertexOffsets[0].xy; break;\r\n"
	"case 4: tPOS = uf_pushConstants.vertexOffsets[2].xy; break;\r\n"
	"case 5: tPOS = uf_pushConstants.vertexOffsets[3].xy; break;\r\n"
	"}"
	"passSrcTexelOffset = uf_pushConstants.srcTexelOffset;\r\n"
	"gl_Position = vec4(tPOS, 0, 1.0);\r\n"
	"}\r\n";

static const char* s_copySurfacePSColorToDepth =
	"#version 450\r\n"
	"layout(location = 0) in flat ivec2 passSrcTexelOffset;\r\n"
	"layout(binding = 0) uniform sampler2D textureSrc;\r\n"
	"in vec4 gl_FragCoord;\r\n"
	"\r\n"
	"void main(){\r\n"
	"gl_FragDepth = texelFetch(textureSrc, passSrcTexelOffset + ivec2(gl_FragCoord.xy), 0).r;\r\n"
	"}\r\n";

static const char* s_copySurfacePSDepthToColor =
	"#version 450\r\n"
	"layout(location = 0) in flat ivec2 passSrcTexelOffset;\r\n"
	"layout(binding = 0) uniform sampler2D textureSrc;\r\n"
	"layout(location = 0) out vec4 colorOut0;\r\n"
	"in vec4 gl_FragCoord;\r\n"
	"\r\n"
	"void main(){\r\n"
	"colorOut0.r = texelFetch(textureSrc, passSrcTexelOffset + ivec2(gl_FragCoord.xy), 0).r;\r\n"
	"}\r\n";

void D3D12Renderer::surfaceCopy_init()
{
	m_internal = std::make_unique<InternalPipelines>();
	m_internal->copyVS.reset(CreateInternalShader(RendererShader::ShaderType::kVertex, s_copySurfaceVS));
	m_internal->copyPSDepthToColor.reset(CreateInternalShader(RendererShader::ShaderType::kFragment, s_copySurfacePSDepthToColor));
	m_internal->copyPSColorToDepth.reset(CreateInternalShader(RendererShader::ShaderType::kFragment, s_copySurfacePSColorToDepth));
}

void D3D12Renderer::surfaceCopy_shutdown()
{
	m_internal.reset();
}

void D3D12Renderer::surfaceCopy_notifyViewRelease(LatteTextureViewD3D12* view)
{
	// surface copies use temporary descriptors, nothing is cached per view
}

static D3D12_SAMPLER_DESC _MakeSampler(bool linear)
{
	D3D12_SAMPLER_DESC s{};
	s.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
	s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	s.MaxAnisotropy = 1;
	s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	s.MaxLOD = D3D12_FLOAT32_MAX;
	return s;
}

void D3D12Renderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
	sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	if (!AcquireBackbuffer(!padView))
		return;
	D3D12SwapChain* swapChain = GetSwapChainObj(!padView);
	auto* view = static_cast<LatteTextureViewD3D12*>(texView);
	auto* vs = static_cast<RendererShaderD3D12*>(shader->GetVertexShader());
	auto* ps = static_cast<RendererShaderD3D12*>(shader->GetFragmentShader());
	ID3D12PipelineState* pso = m_pipelineCache->GetInternalPipeline(vs, ps, D3D12SwapChain::kFormat, DXGI_FORMAT_UNKNOWN, false, false);
	if (!pso)
		return;
	// acquire the sampler table before recording any state (see GetSamplerTable)
	D3D12_SAMPLER_DESC sampler = _MakeSampler(useLinearTexFilter);
	const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = GetSamplerTable(&sampler, 1);

	TransitionTexture(view->GetBaseTexture(), view->firstMip, view->numMip, view->firstSlice, view->numSlice, LatteTextureD3D12::kShaderReadState);
	FlushBarriers();

	if (!m_state.rootSignatureBound)
		BindRootSignatureAndHeaps();
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain->GetCurrentRTV();
	m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	if (clearBackground)
	{
		const float clearColor[4] = { 0, 0, 0, 0 };
		m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
	}
	D3D12_VIEWPORT vp{ (float)imageX, (float)imageY, (float)imageWidth, (float)imageHeight, 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, (LONG)swapChain->GetWidth(), (LONG)swapChain->GetHeight() };
	m_cmdList->RSSetViewports(1, &vp);
	m_cmdList->RSSetScissorRects(1, &scissor);
	m_cmdList->SetPipelineState(pso);
	m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	auto outputUniforms = shader->FillUniformBlockBuffer(*texView, { imageWidth, imageHeight }, padView);
	D3D12UploadAllocation ub = AllocateUpload(AlignUp<uint64>(sizeof(outputUniforms), D3D12Const::kCBVAlignment), D3D12Const::kCBVAlignment);
	memcpy(ub.cpuPtr, &outputUniforms, sizeof(outputUniforms));

	const D3D12BindingRemap& remap = ps->GetBindingRemap();
	D3D12StageTableDesc table;
	if (sint32 reg = remap.GetCBV(1); reg >= 0) // "parameters" block, binding 1 in the Vulkan variant
	{
		table.cbvAddress[reg] = ub.gpuAddress;
		table.cbvSize[reg] = (uint32)sizeof(outputUniforms);
	}
	if (sint32 reg = remap.GetSRV(0); reg >= 0)
		table.srv[reg] = view->GetSRVRGBA();
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageTable(D3D12Const::Stage::Pixel), WriteStageTable(table));
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageSamplers(D3D12Const::Stage::Pixel), samplerTable);
	uint32 runtimeData[D3D12Const::kRuntimeDataDwords]{};
	m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamRuntimeData, D3D12Const::kRuntimeDataDwords, runtimeData, 0);

	m_cmdList->DrawInstanced(6, 1, 0, 0);

	swapChain->SetDefinedContent();
	InvalidateDrawState();
	m_hasRecordedWork = true;
}

void D3D12Renderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
	sint32 effectiveCopyWidth = width;
	sint32 effectiveCopyHeight = height;
	LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);

	auto* srcTex = static_cast<LatteTextureD3D12*>(sourceTexture);
	auto* dstTex = static_cast<LatteTextureD3D12*>(destinationTexture);
	if (!LatteTexture_doesEffectiveRescaleRatioMatch(srcTex, srcMip, dstTex, dstMip))
	{
		cemuLog_logDebug(LogType::Force, "surfaceCopy_copySurfaceWithFormatConversion(): Mismatching dimensions");
		return;
	}
	if (Latte::GetFormatBits(srcTex->format) != Latte::GetFormatBits(dstTex->format))
	{
		cemuLog_logDebug(LogType::Force, "surfaceCopy_copySurfaceWithFormatConversion(): Mismatching BPP");
		return;
	}
	cemu_assert_debug(srcTex != dstTex);

	RendererShaderD3D12* ps = dstTex->isDepth ? m_internal->copyPSColorToDepth.get() : m_internal->copyPSDepthToColor.get();
	const DXGI_FORMAT rtvFormat = dstTex->isDepth ? DXGI_FORMAT_UNKNOWN : dstTex->GetFormatInfo().rtvFormat;
	const DXGI_FORMAT dsvFormat = dstTex->isDepth ? dstTex->GetFormatInfo().dsvFormat : DXGI_FORMAT_UNKNOWN;
	if (!dstTex->isDepth && rtvFormat == DXGI_FORMAT_UNKNOWN)
	{
		cemuLog_logDebugOnce(LogType::Force, "D3D12: Surface copy into non-renderable format {:04x}", (uint32)dstTex->format);
		return;
	}
	ID3D12PipelineState* pso = m_pipelineCache->GetInternalPipeline(m_internal->copyVS.get(), ps, rtvFormat, dsvFormat, dstTex->isDepth, false);
	if (!pso || !srcTex->GetResource() || !dstTex->GetResource())
		return;
	// acquire the sampler table before recording any state (see GetSamplerTable)
	D3D12_SAMPLER_DESC sampler = _MakeSampler(false);
	const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = GetSamplerTable(&sampler, 1);

	// source SRV of a single mip/slice. Depth textures are read through their depth SRV format
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = srcTex->GetFormatInfo().srvFormat;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = srcMip;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = srcSlice;
	srvDesc.Texture2DArray.ArraySize = 1;
	if (srcSlice == 0)
	{
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = srcMip;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.PlaneSlice = 0;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE srv = m_stagingViewHeap->Allocate();
	m_device->CreateShaderResourceView(srcTex->GetResource(), &srvDesc, srv);

	TransitionTexture(srcTex, srcMip, 1, srcSlice, 1, LatteTextureD3D12::kShaderReadState);
	TransitionTexture(dstTex, dstMip, 1, dstSlice, 1, dstTex->isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
	FlushBarriers();

	D3D12_CPU_DESCRIPTOR_HANDLE target{};
	if (dstTex->isDepth)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = dsvFormat;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.MipSlice = dstMip;
		dsvDesc.Texture2DArray.FirstArraySlice = dstSlice;
		dsvDesc.Texture2DArray.ArraySize = 1;
		target = m_stagingDSVHeap->Allocate();
		m_device->CreateDepthStencilView(dstTex->GetResource(), &dsvDesc, target);
		m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &target);
	}
	else
	{
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = rtvFormat;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		rtvDesc.Texture2DArray.MipSlice = dstMip;
		rtvDesc.Texture2DArray.FirstArraySlice = dstSlice;
		rtvDesc.Texture2DArray.ArraySize = 1;
		target = m_stagingRTVHeap->Allocate();
		m_device->CreateRenderTargetView(dstTex->GetResource(), &rtvDesc, target);
		m_cmdList->OMSetRenderTargets(1, &target, FALSE, nullptr);
	}

	if (!m_state.rootSignatureBound)
		BindRootSignatureAndHeaps();
	D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)effectiveCopyWidth, (float)effectiveCopyHeight, 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, effectiveCopyWidth, effectiveCopyHeight };
	m_cmdList->RSSetViewports(1, &vp);
	m_cmdList->RSSetScissorRects(1, &scissor);
	m_cmdList->SetPipelineState(pso);
	m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	struct
	{
		float vertexOffsets[4 * 2];
		sint32 srcTexelOffset[2];
	} pushConstants = { { -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f }, { 0, 0 } };
	SetPushConstants(&pushConstants, sizeof(pushConstants));

	const D3D12BindingRemap& remap = ps->GetBindingRemap();
	D3D12StageTableDesc table;
	if (sint32 reg = remap.GetSRV(0); reg >= 0)
		table.srv[reg] = srv;
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageTable(D3D12Const::Stage::Pixel), WriteStageTable(table));
	m_cmdList->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageSamplers(D3D12Const::Stage::Pixel), samplerTable);
	uint32 runtimeData[D3D12Const::kRuntimeDataDwords]{};
	m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamRuntimeData, D3D12Const::kRuntimeDataDwords, runtimeData, 0);

	m_cmdList->DrawInstanced(6, 1, 0, 0);

	// descriptors were consumed when recording (RTV/DSV) or copied into the shader visible heap (SRV)
	m_stagingViewHeap->Free(srv);
	if (dstTex->isDepth)
		m_stagingDSVHeap->Free(target);
	else
		m_stagingRTVHeap->Free(target);

	InvalidateDrawState();
	m_hasRecordedWork = true;
	LatteTexture_TrackTextureGPUWrite(dstTex, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
}
