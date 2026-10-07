#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ImGui.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"

#include <imgui.h>

static const char* s_imguiVS = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(push_constant) uniform PushConstants
{
	vec2 uScale;
	vec2 uTranslate;
} pc;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
void main()
{
	vColor = aColor;
	vUV = aUV;
	gl_Position = vec4(aPos * pc.uScale + pc.uTranslate, 0.0, 1.0);
}
)";

static const char* s_imguiPS = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D sTexture;
layout(location = 0) out vec4 fColor;
void main()
{
	fColor = vColor * texture(sTexture, vUV);
}
)";

D3D12ImGuiRenderer::D3D12ImGuiRenderer(D3D12Renderer* renderer)
	: m_renderer(renderer)
{
	m_vs.reset(renderer->CreateInternalShader(RendererShader::ShaderType::kVertex, s_imguiVS));
	m_ps.reset(renderer->CreateInternalShader(RendererShader::ShaderType::kFragment, s_imguiPS));
}

D3D12ImGuiRenderer::~D3D12ImGuiRenderer()
{
	DestroyFontTexture();
	if (m_pipeline)
		m_renderer->ReleaseObjectDeferred(m_pipeline);
}

ID3D12PipelineState* D3D12ImGuiRenderer::GetPipeline(DXGI_FORMAT rtvFormat)
{
	if (m_pipeline && m_pipelineFormat == rtvFormat)
		return m_pipeline.Get();
	if (!m_vs->IsValid() || !m_ps->IsValid())
		return nullptr;
	if (m_pipeline)
		m_renderer->ReleaseObjectDeferred(m_pipeline);
	m_pipeline.Reset();

	D3D12_INPUT_ELEMENT_DESC elements[] = {
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)offsetof(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_renderer->GetRootSignature();
	desc.VS = m_vs->GetBytecode();
	desc.PS = m_ps->GetBytecode();
	desc.InputLayout = { elements, (UINT)std::size(elements) };
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	auto& rt = desc.BlendState.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = D3D12_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D12_BLEND_ONE;
	rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = 0xFFFFFFFF;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.StencilEnable = FALSE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = rtvFormat;
	desc.SampleDesc.Count = 1;
	HRESULT hr = m_renderer->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pipeline));
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: Failed to create ImGui pipeline: {}", D3D12_HResultToString(hr));
		return nullptr;
	}
	m_pipelineFormat = rtvFormat;
	return m_pipeline.Get();
}

void* D3D12ImGuiRenderer::CreateTexture(const uint8* rgba, sint32 width, sint32 height)
{
	auto* tex = new Texture();
	ID3D12Device* device = m_renderer->GetDevice();
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = (UINT64)std::max(width, 1);
	desc.Height = (UINT)std::max(height, 1);
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex->resource))))
	{
		delete tex;
		return nullptr;
	}
	D3D12_SetDebugName(tex->resource.Get(), "ImGuiTexture");

	const uint32 rowBytes = (uint32)width * 4;
	const uint32 rowPitch = AlignUp<uint32>(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	D3D12UploadAllocation upload = m_renderer->AllocateUpload((uint64)rowPitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	for (sint32 y = 0; y < height; y++)
		memcpy(upload.cpuPtr + (size_t)y * rowPitch, rgba + (size_t)y * rowBytes, rowBytes);
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = tex->resource.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = upload.resource;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = upload.offset;
	src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)width, (UINT)height, 1, rowPitch };
	auto* cmdList = m_renderer->GetCommandList();
	m_renderer->FlushBarriers();
	cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	D3D12_RESOURCE_BARRIER barrier = D3D12_TransitionBarrier(tex->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cmdList->ResourceBarrier(1, &barrier);
	m_renderer->m_hasRecordedWork = true;

	tex->srv = m_renderer->GetStagingViewHeap().Allocate();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(tex->resource.Get(), &srvDesc, tex->srv);
	return tex;
}

void D3D12ImGuiRenderer::DestroyTexture(void* texture)
{
	auto* tex = (Texture*)texture;
	if (!tex)
		return;
	m_renderer->ReleaseResourceDeferred(std::move(tex->resource));
	m_renderer->GetStagingViewHeap().Free(tex->srv);
	delete tex;
}

void D3D12ImGuiRenderer::EnsureFontTexture()
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.Fonts->TexID != nullptr && m_fontTexture)
		return;
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	m_fontTexture = (Texture*)CreateTexture(pixels, width, height);
	io.Fonts->TexID = (ImTextureID)m_fontTexture;
}

void D3D12ImGuiRenderer::DestroyFontTexture()
{
	if (!m_fontTexture)
		return;
	if (ImGui::GetCurrentContext())
	{
		ImGuiIO& io = ImGui::GetIO();
		if (io.Fonts && io.Fonts->TexID == (ImTextureID)m_fontTexture)
			io.Fonts->TexID = nullptr;
	}
	DestroyTexture(m_fontTexture);
	m_fontTexture = nullptr;
}

void D3D12ImGuiRenderer::Render(ImDrawData* drawData, D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT rtvFormat, uint32 targetWidth, uint32 targetHeight)
{
	if (!drawData || drawData->TotalVtxCount == 0)
		return;
	const float fbWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
	const float fbHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
	if (fbWidth <= 0 || fbHeight <= 0)
		return;
	ID3D12PipelineState* pso = GetPipeline(rtvFormat);
	if (!pso)
		return;

	// geometry
	const size_t vtxBytes = (size_t)drawData->TotalVtxCount * sizeof(ImDrawVert);
	const size_t idxBytes = (size_t)drawData->TotalIdxCount * sizeof(ImDrawIdx);
	D3D12UploadAllocation vb = m_renderer->AllocateUpload(vtxBytes, 16);
	D3D12UploadAllocation ib = m_renderer->AllocateUpload(std::max<size_t>(idxBytes, 4), 16);
	{
		uint8* vtxDst = vb.cpuPtr;
		uint8* idxDst = ib.cpuPtr;
		for (int n = 0; n < drawData->CmdListsCount; n++)
		{
			const ImDrawList* cmdList = drawData->CmdLists[n];
			memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
			memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
			vtxDst += cmdList->VtxBuffer.Size * sizeof(ImDrawVert);
			idxDst += cmdList->IdxBuffer.Size * sizeof(ImDrawIdx);
		}
	}

	// acquire the sampler table before recording any state (see D3D12Renderer::GetSamplerTable)
	D3D12_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = m_renderer->GetSamplerTable(&sampler, std::max<uint32>(m_ps->GetBindingRemap().srvCount, 1));

	m_renderer->FlushBarriers();
	auto* cmd = m_renderer->GetCommandList();
	m_renderer->BindRootSignatureAndHeaps();
	cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)targetWidth, (float)targetHeight, 0.0f, 1.0f };
	cmd->RSSetViewports(1, &vp);
	cmd->SetPipelineState(pso);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	D3D12_VERTEX_BUFFER_VIEW vbv{ vb.gpuAddress, (UINT)vtxBytes, sizeof(ImDrawVert) };
	cmd->IASetVertexBuffers(0, 1, &vbv);
	D3D12_INDEX_BUFFER_VIEW ibv{ ib.gpuAddress, (UINT)std::max<size_t>(idxBytes, 4), sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
	cmd->IASetIndexBuffer(&ibv);
	const float blendFactor[4] = { 0, 0, 0, 0 };
	cmd->OMSetBlendFactor(blendFactor);

	// orthographic projection. D3D clip space has +Y pointing up
	const float L = drawData->DisplayPos.x;
	const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
	const float T = drawData->DisplayPos.y;
	const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
	const float pushConstants[4] = { 2.0f / (R - L), -2.0f / (B - T), -(R + L) / (R - L), (T + B) / (B - T) };
	m_renderer->SetPushConstants(pushConstants, sizeof(pushConstants));
	uint32 runtimeData[D3D12Const::kRuntimeDataDwords]{};
	cmd->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamRuntimeData, D3D12Const::kRuntimeDataDwords, runtimeData, 0);

	const sint32 srvReg = m_ps->GetBindingRemap().GetSRV(0);
	cmd->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageSamplers(D3D12Const::Stage::Pixel), samplerTable);

	const ImVec2 clipOff = drawData->DisplayPos;
	const ImVec2 clipScale = drawData->FramebufferScale;
	uint32 globalVtxOffset = 0;
	uint32 globalIdxOffset = 0;
	void* boundTexture = nullptr;
	for (int n = 0; n < drawData->CmdListsCount; n++)
	{
		const ImDrawList* cmdList = drawData->CmdLists[n];
		for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
		{
			const ImDrawCmd* pcmd = &cmdList->CmdBuffer[i];
			if (pcmd->UserCallback)
			{
				if (pcmd->UserCallback != ImDrawCallback_ResetRenderState)
					pcmd->UserCallback(cmdList, pcmd);
				continue;
			}
			ImVec2 clipMin((pcmd->ClipRect.x - clipOff.x) * clipScale.x, (pcmd->ClipRect.y - clipOff.y) * clipScale.y);
			ImVec2 clipMax((pcmd->ClipRect.z - clipOff.x) * clipScale.x, (pcmd->ClipRect.w - clipOff.y) * clipScale.y);
			clipMin.x = std::max(clipMin.x, 0.0f);
			clipMin.y = std::max(clipMin.y, 0.0f);
			clipMax.x = std::min(clipMax.x, (float)targetWidth);
			clipMax.y = std::min(clipMax.y, (float)targetHeight);
			if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
				continue;
			D3D12_RECT scissor{ (LONG)clipMin.x, (LONG)clipMin.y, (LONG)clipMax.x, (LONG)clipMax.y };
			cmd->RSSetScissorRects(1, &scissor);
			void* texId = (void*)pcmd->TextureId;
			if (texId != boundTexture && srvReg >= 0)
			{
				auto* tex = (Texture*)texId;
				D3D12StageTableDesc table;
				table.srv[srvReg] = tex ? tex->srv : D3D12_CPU_DESCRIPTOR_HANDLE{};
				cmd->SetGraphicsRootDescriptorTable(D3D12Const::RootParamStageTable(D3D12Const::Stage::Pixel), m_renderer->WriteStageTable(table));
				boundTexture = texId;
			}
			cmd->DrawIndexedInstanced(pcmd->ElemCount, 1, pcmd->IdxOffset + globalIdxOffset, (INT)(pcmd->VtxOffset + globalVtxOffset), 0);
		}
		globalIdxOffset += cmdList->IdxBuffer.Size;
		globalVtxOffset += cmdList->VtxBuffer.Size;
	}
	// the game's draw state has to be re-applied
	m_renderer->InvalidateDrawState();
	m_renderer->m_hasRecordedWork = true;
}
