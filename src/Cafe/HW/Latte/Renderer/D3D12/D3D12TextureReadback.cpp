#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureReadback.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"

LatteTextureReadbackInfoD3D12::LatteTextureReadbackInfoD3D12(D3D12Renderer* renderer, LatteTextureView* textureView)
	: LatteTextureReadbackInfo(textureView, textureView->firstMip), m_renderer(renderer)
{
	auto* baseTexture = static_cast<LatteTextureD3D12*>(textureView->baseTexture);
	if (!baseTexture->GetResource() || baseTexture->IsAlternateFormat() || baseTexture->IsCompressedFormat())
	{
		// same restriction as the Vulkan backend: the core expects the host data to use the guest format's layout
		cemuLog_logDebug(LogType::Force, "D3D12 does not support readback of texture format 0x{:x}", (uint32)baseTexture->format);
		m_image_size = 0;
		return;
	}
	m_rowPitch = GetReadbackRowPitch(textureView, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	m_image_size = GetReadbackImageSize(textureView, m_rowPitch);
}

void LatteTextureReadbackInfoD3D12::StartTransfer()
{
	cemu_assert(m_textureView);
	auto* baseTexture = static_cast<LatteTextureD3D12*>(m_textureView->baseTexture);
	cemu_assert_debug(baseTexture->dim != Latte::E_DIM::DIM_3D);

	m_bufferOffset = m_renderer->m_readbackRing->Allocate(m_image_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	m_renderer->TransitionTexture(baseTexture, m_firstMip, 1, m_firstSlice, 1, D3D12_RESOURCE_STATE_COPY_SOURCE);
	m_renderer->FlushBarriers();

	const uint32 sub = baseTexture->GetSubresourceIndex(m_firstMip, m_firstSlice, 0);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	UINT numRows;
	UINT64 rowSize, totalBytes;
	m_renderer->GetDevice()->GetCopyableFootprints(&baseTexture->GetDesc(), sub, 1, 0, &layout, &numRows, &rowSize, &totalBytes);

	const uint32 width = baseTexture->GetMipWidth(m_firstMip);
	const uint32 height = baseTexture->GetMipHeight(m_firstMip);

	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = baseTexture->GetResource();
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = sub;
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = m_renderer->m_readbackRing->GetResource();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Offset = m_bufferOffset;
	dst.PlacedFootprint.Footprint.Format = layout.Footprint.Format;
	dst.PlacedFootprint.Footprint.Width = std::min<uint32>(width, layout.Footprint.Width);
	dst.PlacedFootprint.Footprint.Height = std::min<uint32>(height, layout.Footprint.Height);
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = m_rowPitch;
	D3D12_BOX box{ 0, 0, 0, dst.PlacedFootprint.Footprint.Width, dst.PlacedFootprint.Footprint.Height, 1 };
	m_renderer->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

	m_submissionId = m_renderer->GetRecordingSubmissionId();
	m_renderer->m_hasRecordedWork = true;
	m_textureView = nullptr;
	// reduce readback latency by submitting soon
	m_renderer->RequestSubmitSoon();
}

bool LatteTextureReadbackInfoD3D12::IsFinished()
{
	if (m_submissionId > m_renderer->m_lastSubmittedFenceValue)
		return false; // still in the command list that is being recorded
	return m_renderer->GetCompletedSubmissionId() >= m_submissionId;
}

void LatteTextureReadbackInfoD3D12::ForceFinish()
{
	if (m_submissionId > m_renderer->m_lastSubmittedFenceValue)
		m_renderer->SubmitCommandList(false);
	m_renderer->WaitForSubmission(m_submissionId);
}

uint8* LatteTextureReadbackInfoD3D12::GetData()
{
	return m_renderer->m_readbackRing->GetCPUPtr(m_bufferOffset);
}
