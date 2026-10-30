#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Core/LatteQueryObject.h"

// diagnostic (#231): draws the game issued while one of its occlusion queries was active and how many of them reached the
// GPU. A query that reads 0 because its draws were dropped looks the same to the game as one the GPU counted as hidden
static uint32 s_queryDrawsRequested = 0;
static uint32 s_queryDrawsIssued = 0;
// user/query_sync.txt at boot (A/B switch for #231): results are only read after the GPU went idle, not as soon as the
// fence of their submission passed
static bool s_querySyncReads = false;
bool D3D12_UserDataFileExists(const char* fileName); // D3D12Renderer.cpp

// true while the last 10 s window read (nearly) only zeros: D3D12RendererCore then logs the query draws' state
static bool s_queriesMostlyZero = false;
bool D3D12Query_MostlyZero() { return s_queriesMostlyZero; }

void D3D12Query_CountDraw(bool issued)
{
	if (issued)
		s_queryDrawsIssued++;
	else
		s_queryDrawsRequested++;
}

// D3D12 queries cannot span command lists. Like the Vulkan backend, a Latte query is split into fragments, one per
// command list, and the results are summed up once all fragments have completed.
class LatteQueryObjectD3D12 final : public LatteQueryObject
{
	friend class D3D12Renderer;

public:
	explicit LatteQueryObjectD3D12(D3D12Renderer* renderer) : m_renderer(renderer) {}

	bool getResult(uint64& numSamplesPassed) override
	{
		if (!m_ended)
			return false;
		if (s_querySyncReads)
		{
			if (m_finishSubmission > m_renderer->m_lastSubmittedFenceValue)
				m_renderer->SubmitCommandList(false);
			m_renderer->WaitForSubmission(m_renderer->m_lastSubmittedFenceValue);
		}
		if (m_finishSubmission > m_renderer->m_lastSubmittedFenceValue || m_renderer->GetCompletedSubmissionId() < m_finishSubmission)
			return false;
		HandleFinishedFragments();
		cemu_assert_debug(m_fragments.empty());
		numSamplesPassed = m_accumulatedSum;
		return true;
	}

	void begin() override
	{
		m_ended = false;
		m_hasActiveQuery = true;
		BeginFragment();
	}

	void end() override
	{
		if (m_hasActiveFragment)
			EndFragment();
		m_ended = true;
		m_hasActiveQuery = false;
		m_finishSubmission = m_renderer->GetRecordingSubmissionId();
		m_renderer->m_occlusionQueries.lastSubmission = m_finishSubmission;
		m_renderer->RequestSubmitSoon();
	}

	void BeginFragment()
	{
		HandleFinishedFragments();
		auto& oq = m_renderer->m_occlusionQueries;
		if (oq.freeIndices.empty())
		{
			cemuLog_log(LogType::Force, "D3D12: Exhausted occlusion query heap");
			return;
		}
		Fragment f;
		f.queryIndex = oq.freeIndices.back();
		oq.freeIndices.pop_back();
		m_renderer->GetCommandList()->BeginQuery(oq.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, f.queryIndex);
		m_fragments.push_back(f);
		m_hasActiveFragment = true;
	}

	void EndFragment()
	{
		auto& oq = m_renderer->m_occlusionQueries;
		if (m_fragments.empty())
		{
			m_hasActiveFragment = false;
			return;
		}
		Fragment& f = m_fragments.back();
		auto* cmdList = m_renderer->GetCommandList();
		cmdList->EndQuery(oq.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, f.queryIndex);
		cmdList->ResolveQueryData(oq.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, f.queryIndex, 1, oq.resultBuffer.Get(), (UINT64)f.queryIndex * sizeof(uint64));
		f.submission = m_renderer->GetRecordingSubmissionId();
		f.finished = true;
		m_renderer->m_hasRecordedWork = true;
		m_hasActiveFragment = false;
	}

	void HandleFinishedFragments()
	{
		auto& oq = m_renderer->m_occlusionQueries;
		const uint64 completed = m_renderer->GetCompletedSubmissionId();
		while (!m_fragments.empty())
		{
			Fragment& f = m_fragments.front();
			if (!f.finished || f.submission > completed)
				break;
			m_accumulatedSum += ReadResult(f.queryIndex);
			oq.freeIndices.push_back(f.queryIndex);
			m_fragments.erase(m_fragments.begin());
		}
	}

	// Map with a read range for every result: the runtime then makes the GPU's write visible to the CPU. Reading the
	// persistently mapped pointer alone is only reliable on cache coherent hardware, on the Xbox BOTW's rune targeting
	// (an occlusion query) never found a target (#71)
	static uint64 ReadResult(uint32 queryIndex)
	{
		auto& oq = D3D12Renderer::GetInstance()->m_occlusionQueries;
		const D3D12_RANGE readRange{ (SIZE_T)queryIndex * sizeof(uint64), (SIZE_T)(queryIndex + 1) * sizeof(uint64) };
		void* mapped = nullptr;
		uint64 value = oq.results[queryIndex];
		if (SUCCEEDED(oq.resultBuffer->Map(0, &readRange, &mapped)))
		{
			value = ((const uint64*)mapped)[queryIndex];
			const D3D12_RANGE noWrite{ 0, 0 };
			oq.resultBuffer->Unmap(0, &noWrite);
		}
		// diagnostic: how many results titles read and how many were zero, every 10 s while queries are used
		static uint32 s_reads = 0, s_zeros = 0;
		static auto s_nextLog = std::chrono::steady_clock::now();
		s_reads++;
		if (value == 0)
			s_zeros++;
		const auto now = std::chrono::steady_clock::now();
		if (now >= s_nextLog)
		{
			s_nextLog = now + std::chrono::seconds(10);
			cemuLog_log(LogType::Force, "D3D12: occlusion query fragments read: {}, zero: {}, last {}; draws inside queries: {}, issued to the GPU: {}{}",
				s_reads, s_zeros, value, s_queryDrawsRequested, s_queryDrawsIssued, s_querySyncReads ? " (query_sync.txt: reads wait for GPU idle)" : "");
			s_queriesMostlyZero = s_reads >= 50 && s_zeros * 10 >= s_reads * 9;
			s_reads = s_zeros = 0;
			s_queryDrawsRequested = s_queryDrawsIssued = 0;
		}
		return value;
	}

	void Reset()
	{
		auto& oq = m_renderer->m_occlusionQueries;
		for (auto& f : m_fragments)
			oq.freeIndices.push_back(f.queryIndex);
		m_fragments.clear();
		queryEnded = false;
		queryEventStart = 0;
		queryEventEnd = 0;
		m_ended = false;
		m_hasActiveQuery = false;
		m_hasActiveFragment = false;
		m_accumulatedSum = 0;
		m_finishSubmission = 0;
	}

private:
	struct Fragment
	{
		uint32 queryIndex = 0;
		uint64 submission = 0;
		bool finished = false;
	};
	D3D12Renderer* m_renderer;
	std::vector<Fragment> m_fragments;
	bool m_ended = false;
	bool m_hasActiveQuery = false;
	bool m_hasActiveFragment = false;
	uint64 m_finishSubmission = 0;
	uint64 m_accumulatedSum = 0;
};

LatteQueryObject* D3D12Renderer::occlusionQuery_create()
{
	auto& oq = m_occlusionQueries;
	if (!oq.heap)
	{
		D3D12_QUERY_HEAP_DESC desc{};
		desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
		desc.Count = kOcclusionQueryCount;
		D3D12_ThrowIfFailed(m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&oq.heap)), "CreateQueryHeap");
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_READBACK);
		D3D12_RESOURCE_DESC bufferDesc = D3D12_BufferDesc((uint64)kOcclusionQueryCount * sizeof(uint64));
		D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&oq.resultBuffer)), "CreateCommittedResource (query results)");
		void* mapped = nullptr;
		D3D12_ThrowIfFailed(oq.resultBuffer->Map(0, nullptr, &mapped), "Map (query results)");
		oq.results = (uint64*)mapped;
		oq.freeIndices.reserve(kOcclusionQueryCount);
		s_querySyncReads = D3D12_AbToggle("query_sync"); // query_sync.txt or the word in magnesis_ab.txt (#449)
		if (s_querySyncReads)
			cemuLog_log(LogType::Force, "D3D12: query_sync found, occlusion query reads wait for the GPU to go idle");
		for (uint32 i = kOcclusionQueryCount; i > 0; i--)
			oq.freeIndices.push_back(i - 1);
	}
	LatteQueryObjectD3D12* query;
	if (oq.cached.empty())
		query = new LatteQueryObjectD3D12(this);
	else
	{
		query = oq.cached.back();
		oq.cached.pop_back();
	}
	query->Reset();
	oq.active.push_back(query);
	return query;
}

void D3D12Renderer::occlusionQuery_destroy(LatteQueryObject* queryObj)
{
	auto* query = static_cast<LatteQueryObjectD3D12*>(queryObj);
	auto& oq = m_occlusionQueries;
	oq.active.erase(std::remove(oq.active.begin(), oq.active.end(), query), oq.active.end());
	if (query->m_hasActiveFragment)
		query->EndFragment();
	query->Reset();
	oq.cached.push_back(query);
}

void D3D12Renderer::occlusionQuery_flush()
{
	auto& oq = m_occlusionQueries;
	if (oq.lastSubmission > m_lastSubmittedFenceValue)
		SubmitCommandList(false);
	if (oq.lastSubmission)
		WaitForSubmission(oq.lastSubmission);
}

void D3D12Renderer::occlusionQuery_updateState()
{
	ProcessFinishedSubmissions();
}

void D3D12Renderer::occlusionQuery_notifyEndCommandList()
{
	for (auto* q : m_occlusionQueries.active)
		if (q->m_hasActiveQuery && q->m_hasActiveFragment)
			q->EndFragment();
}

void D3D12Renderer::occlusionQuery_notifyBeginCommandList()
{
	for (auto* q : m_occlusionQueries.active)
		if (q->m_hasActiveQuery)
			q->BeginFragment();
}

void D3D12Renderer::occlusionQuery_destroyAll()
{
	auto& oq = m_occlusionQueries;
	for (auto* q : oq.active)
		delete q;
	for (auto* q : oq.cached)
		delete q;
	oq.active.clear();
	oq.cached.clear();
	if (oq.resultBuffer && oq.results)
		oq.resultBuffer->Unmap(0, nullptr);
	oq.results = nullptr;
}
