#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Memory.h"

#include <stdexcept>

static ComPtr<ID3D12Resource> _CreateMappedBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, uint64 size, uint8** cpuPtrOut, const std::string& debugName)
{
	ComPtr<ID3D12Resource> buffer;
	D3D12_HEAP_PROPERTIES heapProps = D3D12_HeapProps(heapType);
	D3D12_RESOURCE_DESC desc = D3D12_BufferDesc(size);
	D3D12_RESOURCE_STATES initialState = (heapType == D3D12_HEAP_TYPE_UPLOAD) ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
	D3D12_ThrowIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&buffer)), "CreateCommittedResource (mapped buffer)");
	D3D12_SetDebugName(buffer.Get(), debugName);
	D3D12_RANGE readRange{ 0, 0 };
	void* mapped = nullptr;
	if (heapType == D3D12_HEAP_TYPE_READBACK)
		D3D12_ThrowIfFailed(buffer->Map(0, nullptr, &mapped), "Map (readback buffer)");
	else
		D3D12_ThrowIfFailed(buffer->Map(0, &readRange, &mapped), "Map (upload buffer)");
	*cpuPtrOut = (uint8*)mapped;
	return buffer;
}

/* D3D12UploadRing */

D3D12UploadRing::D3D12UploadRing(ID3D12Device* device, D3D12SubmissionTracker* tracker, uint64 size, const char* debugName)
	: m_device(device), m_tracker(tracker), m_size(size)
{
	m_buffer = _CreateMappedBuffer(device, D3D12_HEAP_TYPE_UPLOAD, size, &m_cpuBase, debugName);
	m_gpuBase = m_buffer->GetGPUVirtualAddress();
}

D3D12UploadRing::~D3D12UploadRing()
{
	if (m_buffer)
		m_buffer->Unmap(0, nullptr);
}

std::optional<uint64> D3D12UploadRing::TryAllocate(uint64 size, uint64 alignment)
{
	if (size > m_size)
		return std::nullopt;
	if (IsEmpty())
	{
		m_head = 0;
		m_tail = 0;
		return 0;
	}
	if (m_head == m_tail)
		return std::nullopt; // ring is completely full
	if (m_head > m_tail)
	{
		// free space is [head, size) and [0, tail)
		uint64 start = AlignUp(m_head, alignment);
		if (start + size <= m_size)
			return start;
		if (size < m_tail) // strictly smaller so head never catches up to tail exactly (which would look like "empty")
			return 0;
		return std::nullopt;
	}
	// head < tail, free space is [head, tail)
	uint64 start = AlignUp(m_head, alignment);
	if (start + size < m_tail)
		return start;
	return std::nullopt;
}

D3D12UploadAllocation D3D12UploadRing::Allocate(uint64 size, uint64 alignment)
{
	cemu_assert_debug(alignment > 0);
	size = std::max<uint64>(size, 1);
	std::optional<uint64> offset = TryAllocate(size, alignment);
	while (!offset.has_value() && !m_markers.empty())
	{
		// wait for the oldest submitted data to be consumed by the GPU
		m_tracker->WaitForSubmission(m_markers.front().submissionId);
		Retire(m_tracker->GetCompletedSubmissionId());
		offset = TryAllocate(size, alignment);
	}
	D3D12UploadAllocation allocation{};
	if (!offset.has_value())
	{
		// the command list that is currently being recorded already uses the entire ring. Use a dedicated buffer
		Overflow overflow;
		overflow.submissionId = m_tracker->GetRecordingSubmissionId();
		uint8* cpuPtr = nullptr;
		overflow.buffer = _CreateMappedBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, AlignUp<uint64>(size, 256), &cpuPtr, "UploadRingOverflow");
		allocation.cpuPtr = cpuPtr;
		allocation.gpuAddress = overflow.buffer->GetGPUVirtualAddress();
		allocation.resource = overflow.buffer.Get();
		allocation.offset = 0;
		allocation.size = size;
		m_overflow.emplace_back(std::move(overflow));
		cemuLog_logDebug(LogType::Force, "D3D12: Upload ring overflow ({} bytes requested)", size);
		return allocation;
	}
	m_head = offset.value() + size;
	m_recordingHasData = true;
	allocation.cpuPtr = m_cpuBase + offset.value();
	allocation.gpuAddress = m_gpuBase + offset.value();
	allocation.resource = m_buffer.Get();
	allocation.offset = offset.value();
	allocation.size = size;
	return allocation;
}

void D3D12UploadRing::OnSubmit(uint64 submissionId)
{
	if (!m_recordingHasData)
		return;
	m_markers.push_back({ submissionId, m_head });
	m_recordingHasData = false;
}

void D3D12UploadRing::Retire(uint64 completedSubmissionId)
{
	while (!m_markers.empty() && m_markers.front().submissionId <= completedSubmissionId)
	{
		m_tail = m_markers.front().end;
		m_markers.pop_front();
	}
	while (!m_overflow.empty() && m_overflow.front().submissionId <= completedSubmissionId)
		m_overflow.pop_front();
}

uint64 D3D12UploadRing::GetUsedBytes() const
{
	if (IsEmpty())
		return 0;
	if (m_head > m_tail)
		return m_head - m_tail;
	return (m_size - m_tail) + m_head;
}

/* D3D12UploadHeapAllocator */

D3D12UploadHeapAllocator::D3D12UploadHeapAllocator(ID3D12Device* device, D3D12SubmissionTracker* tracker, uint32 chunkSize, const char* debugName)
	: m_device(device), m_tracker(tracker), m_chunkSize(chunkSize), m_debugName(debugName)
{
}

D3D12UploadHeapAllocator::~D3D12UploadHeapAllocator()
{
	for (auto& pending : m_pendingFrees)
		delete pending.allocation;
	m_pendingFrees.clear();
	for (auto& chunk : m_d3dChunks)
	{
		if (chunk.buffer)
			chunk.buffer->Unmap(0, nullptr);
	}
}

uint32 D3D12UploadHeapAllocator::allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize)
{
	uint32 size = std::max(m_chunkSize, AlignUp<uint32>(minimumAllocationSize, 256));
	if (m_d3dChunks.size() <= chunkIndex)
		m_d3dChunks.resize(chunkIndex + 1);
	D3DChunk& chunk = m_d3dChunks[chunkIndex];
	try
	{
		chunk.buffer = _CreateMappedBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, size, &chunk.cpuPtr, fmt::format("{}_chunk{}", m_debugName, chunkIndex));
	}
	catch (const std::exception& e)
	{
		cemuLog_log(LogType::Force, "D3D12: Failed to allocate upload heap chunk: {}", e.what());
		return 0;
	}
	chunk.gpuAddress = chunk.buffer->GetGPUVirtualAddress();
	return size;
}

D3D12UploadHeapAllocator::Allocation* D3D12UploadHeapAllocator::Allocate(uint32 size, uint32 alignment)
{
	CHAddr addr = alloc(size, alignment);
	if (!addr.isValid())
	{
		// try again after retiring pending frees
		Retire(m_tracker->GetCompletedSubmissionId());
		addr = alloc(size, alignment);
		if (!addr.isValid())
		{
			cemuLog_log(LogType::Force, "D3D12: Upload heap allocation of {} bytes failed", size);
			return nullptr;
		}
	}
	Allocation* allocation = new Allocation();
	allocation->addr = addr;
	allocation->size = size;
	const D3DChunk& chunk = m_d3dChunks[addr.chunkIndex];
	allocation->cpuPtr = chunk.cpuPtr + addr.offset;
	allocation->gpuAddress = chunk.gpuAddress + addr.offset;
	return allocation;
}

void D3D12UploadHeapAllocator::Free(Allocation* allocation)
{
	if (!allocation)
		return;
	m_pendingFrees.push_back({ m_tracker->GetRecordingSubmissionId(), allocation });
}

void D3D12UploadHeapAllocator::Retire(uint64 completedSubmissionId)
{
	while (!m_pendingFrees.empty() && m_pendingFrees.front().submissionId <= completedSubmissionId)
	{
		Allocation* allocation = m_pendingFrees.front().allocation;
		free(allocation->addr);
		delete allocation;
		m_pendingFrees.pop_front();
	}
}

/* D3D12ReadbackRing */

D3D12ReadbackRing::D3D12ReadbackRing(ID3D12Device* device, uint64 size, const char* debugName)
	: m_size(size)
{
	m_buffer = _CreateMappedBuffer(device, D3D12_HEAP_TYPE_READBACK, size, &m_cpuBase, debugName);
}

D3D12ReadbackRing::~D3D12ReadbackRing()
{
	if (m_buffer)
		m_buffer->Unmap(0, nullptr);
}

uint64 D3D12ReadbackRing::Allocate(uint64 size, uint64 alignment)
{
	cemu_assert(size <= m_size);
	uint64 start = AlignUp(m_head, alignment);
	if (start + size > m_size)
		start = 0;
	m_head = start + size;
	return start;
}
