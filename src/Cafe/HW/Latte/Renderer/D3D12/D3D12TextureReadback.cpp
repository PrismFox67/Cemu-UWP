#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureReadback.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "config/ActiveSettings.h"

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
	m_isR16 = baseTexture->format == Latte::E_GX2SURFFMT::R16_UNORM;
	m_textureView = nullptr;
	// #418 A/B: with <user>/readback_sync.txt the core waits for every readback at its first IsFinished() check
	static bool s_sync = false;
	static uint32 s_syncCheck = 0;
	if ((s_syncCheck++ % 256) == 0)
	{
		std::error_code ec;
		const bool on = fs::exists(ActiveSettings::GetUserDataPath("readback_sync.txt"), ec);
		if (on != s_sync || s_syncCheck == 1)
			cemuLog_log(LogType::Force, "D3D12: texture readbacks {}", on ? "synchronous (readback_sync.txt)" : "asynchronous");
		s_sync = on;
	}
	m_sync = s_sync;
	if (m_sync)
		return; // IsFinished() waits for it
	// reduce readback latency by submitting soon
	m_renderer->RequestSubmitSoon();
}

bool LatteTextureReadbackInfoD3D12::IsFinished()
{
	if (m_sync)
		ForceFinish();
	if (m_submissionId > m_renderer->m_lastSubmittedFenceValue)
		return false; // still in the command list that is being recorded
	const uint64 completed = m_renderer->GetCompletedSubmissionId();
	if (completed < m_submissionId)
		return false;
	// #418: timing of the R16 (BOTW rune targeting) readbacks
	static uint32 s_timingLogs = 0;
	if (m_isR16 && s_timingLogs < 64)
	{
		s_timingLogs++;
		cemuLog_log(LogType::Force, "D3D12: R16 readback finished: copy in submission {}, last submitted {}, completed {}", m_submissionId, m_renderer->m_lastSubmittedFenceValue, completed);
	}
	return true;
}

void LatteTextureReadbackInfoD3D12::ForceFinish()
{
	if (m_submissionId > m_renderer->m_lastSubmittedFenceValue)
		m_renderer->SubmitCommandList(false);
	m_renderer->WaitForSubmission(m_submissionId);
}

// Map with the read range of this readback (like the query results, #71/#394): the runtime then makes the GPU's copy
// visible to the CPU. The persistently mapped pointer alone is only reliable on cache coherent hardware
uint8* LatteTextureReadbackInfoD3D12::GetData()
{
	ID3D12Resource* buffer = m_renderer->m_readbackRing->GetResource();
	const D3D12_RANGE readRange{ (SIZE_T)m_bufferOffset, (SIZE_T)(m_bufferOffset + m_image_size) };
	void* mapped = nullptr;
	if (SUCCEEDED(buffer->Map(0, &readRange, &mapped)))
	{
		m_mapped = true;
		return (uint8*)mapped + m_bufferOffset;
	}
	return m_renderer->m_readbackRing->GetCPUPtr(m_bufferOffset);
}

void LatteTextureReadbackInfoD3D12::ReleaseData()
{
	if (!m_mapped)
		return;
	const D3D12_RANGE noWrite{ 0, 0 };
	m_renderer->m_readbackRing->GetResource()->Unmap(0, &noWrite);
	m_mapped = false;
}
