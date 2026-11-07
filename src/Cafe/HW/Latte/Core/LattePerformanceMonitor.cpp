#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include <chrono>
#include <thread>
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#ifdef ENABLE_D3D12
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"
#endif

std::atomic<const char*> g_gpuThreadPhase{"commands"};

namespace
{
	// #733: BOTW freezes of 5.3 s that no Stutter bucket accounts for. While no frame finishes for over a second this logs,
	// from its own thread, what the GPU thread is in and which guest threads run. The guest state is read without locks,
	// it can be slightly out of date
	std::atomic<sint64> s_lastFrameMs{0};
	sint64 FreezeWatchNowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void FreezeWatchThread()
	{
		uint32 logs = 0;
		while (logs < 60)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			const sint64 last = s_lastFrameMs.load();
			const sint64 stalled = FreezeWatchNowMs() - last;
			if (stalled < 1000 || stalled > 15000 || !CafeSystem::IsTitleRunning()) // guest memory goes away when the title stops
				continue;
			logs++;
			std::string running;
			for (sint32 i = 0; i < activeThreadCount; i++)
			{
				OSThread_t* thread = (OSThread_t*)memory_getPointerFromVirtualOffset(activeThread[i]);
				if (thread->state != OSThread_t::THREAD_STATE::STATE_RUNNING)
					continue;
				running += fmt::format(" {:08x}/{:08x}", thread->context.srr0, _swapEndianU32(thread->context.lr)); // #789: srr0 is stored host-endian, lr big-endian
			}
			cemuLog_log(LogType::Force, "Freeze watch: no frame for {} ms, GPU thread in '{}', running guest threads IP/LR:{}", stalled, g_gpuThreadPhase.load(), running);
		}
	}

	uint32 s_cacheLogCountdown = 59;

	// every 60 s while a game runs: texture cache and descriptor sizes, so a tester log from a long session shows whether
	// state that only re-entering a stage clears (GitHub #10, SLW foliage after ~3 h) grows over time (#556/#563)
	void LogCacheStats()
	{
		uint32 textureCount = 0, viewCount = 0;
		for (LatteTexture* tex : LatteTexture::GetAllTextures())
		{
			if (!tex)
				continue;
			textureCount++;
			viewCount += (uint32)tex->views.size();
		}
		uint32 srv = 0, rtv = 0, dsv = 0;
#ifdef ENABLE_D3D12
		if (g_renderer && g_renderer->GetType() == RendererAPI::D3D12)
		{
			D3D12Renderer* r = D3D12Renderer::GetInstance();
			srv = r->GetStagingViewHeap().GetAllocatedCount();
			rtv = r->GetStagingRTVHeap().GetAllocatedCount();
			dsv = r->GetStagingDSVHeap().GetAllocatedCount();
		}
#endif
		cemuLog_log(LogType::Force, "Cache: {} textures, {} views, staging descriptors SRV {} RTV {} DSV {}", textureCount, viewCount, srv, rtv, dsv);
	}
}

performanceMonitor_t performanceMonitor{};
std::atomic<uint64> g_pipelineDriverCreateMs{ 0 }; // D3D12 pipelines created by the driver since the last frame, in ms

void LattePerformanceMonitor_frameEnd()
{
	// per-frame stats
	performanceMonitor.gpuTime_shaderCreate.frameFinished();
	performanceMonitor.gpuTime_frameTime.frameFinished();
	performanceMonitor.gpuTime_idleTime.frameFinished();
	performanceMonitor.gpuTime_fenceTime.frameFinished();

	performanceMonitor.gpuTime_dcStageTextures.frameFinished();
	performanceMonitor.gpuTime_dcStageVertexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageIndexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageMRT.frameFinished();
	performanceMonitor.gpuTime_dcStageDrawcallAPI.frameFinished();
	performanceMonitor.gpuTime_waitForAsync.frameFinished();

	// stutter log (#574): frames that took more than 100 ms of real time, with where the GPU thread spent it, so a tester
	// log names the cause of hitches (new pipelines/shaders, waiting for the CPU or a readback, texture uploads)
	{
		static std::chrono::steady_clock::time_point s_lastFrameEnd{};
		static uint32 s_stutterLogs = 0, s_stuttersSinceSummary = 0;
		static std::chrono::steady_clock::time_point s_lastSummary{};
		const auto now = std::chrono::steady_clock::now();
		const sint64 frameMs = s_lastFrameEnd.time_since_epoch().count() ? std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastFrameEnd).count() : 0;
		s_lastFrameEnd = now;
		s_lastFrameMs = FreezeWatchNowMs();
		static std::once_flag s_watchStarted;
		std::call_once(s_watchStarted, [] { std::thread(FreezeWatchThread).detach(); });
		const uint64 pipelineDriverMs = g_pipelineDriverCreateMs.exchange(0); // on the compile threads, summed
		if (frameMs >= 100 && frameMs < 10000) // longer gaps are pauses and loading, not stutter
		{
			auto ms = [](LattePerfStatTimer& t) { return (double)PPCTimer_tscToMicroseconds(t.getPreviousFrameValue()) / 1000.0; };
			if (s_stutterLogs < 100)
			{
				s_stutterLogs++;
				cemuLog_log(LogType::Force, "Stutter: frame {} ms; GPU thread: shader/pipeline create {:.0f}, waiting for readbacks/queries {:.0f}, fence {:.0f}, texture stage {:.0f}, idle (waiting for the CPU) {:.0f}, draw API {:.0f}; compile threads: driver pipeline create {}",
					frameMs, ms(performanceMonitor.gpuTime_shaderCreate), ms(performanceMonitor.gpuTime_waitForAsync), ms(performanceMonitor.gpuTime_fenceTime),
					ms(performanceMonitor.gpuTime_dcStageTextures), ms(performanceMonitor.gpuTime_idleTime), ms(performanceMonitor.gpuTime_dcStageDrawcallAPI), pipelineDriverMs);
			}
			else
				s_stuttersSinceSummary++;
		}
		if (s_stuttersSinceSummary && now - s_lastSummary >= std::chrono::seconds(60))
		{
			s_lastSummary = now;
			cemuLog_log(LogType::Force, "Stutter: {} more frames over 100 ms in the last minute", s_stuttersSinceSummary);
			s_stuttersSinceSummary = 0;
		}
	}

	uint32 elapsedTime = GetTickCount() - performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate;
	if (elapsedTime >= 1000)
	{
		bool isFirstUpdate = performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate == 0;
		// sum up raw stats
		uint32 totalElapsedTime = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 totalElapsedTimeFPS = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 elapsedFrames = 0;
		uint32 elapsedFrames2S = 0; // elapsed frames for last two entries (seconds)
		uint64 skippedCycles = 0;
		uint64 vertexDataUploaded = 0;
		uint64 vertexDataCached = 0;
		uint64 uniformBankUploadedData = 0;
		uint64 uniformBankUploadedCount = 0;
		uint64 indexDataUploaded = 0;
		uint64 indexDataCached = 0;
		uint32 frameCounter = 0;
		uint32 drawCallCounter = 0;
		uint32 fastDrawCallCounter = 0;
		uint32 shaderBindCounter = 0;
		uint32 recompilerLeaveCount = 0;
		uint32 threadLeaveCount = 0;
		for (sint32 i = 0; i < PERFORMANCE_MONITOR_TRACK_CYCLES; i++)
		{
			elapsedFrames += performanceMonitor.cycle[i].frameCounter;
			skippedCycles += performanceMonitor.cycle[i].skippedCycles;
			vertexDataUploaded += performanceMonitor.cycle[i].vertexDataUploaded;
			vertexDataCached += performanceMonitor.cycle[i].vertexDataCached;
			uniformBankUploadedData += performanceMonitor.cycle[i].uniformBankUploadedData;
			uniformBankUploadedCount += performanceMonitor.cycle[i].uniformBankUploadedCount;
			indexDataUploaded += performanceMonitor.cycle[i].indexDataUploaded;
			indexDataCached += performanceMonitor.cycle[i].indexDataCached;
			frameCounter += performanceMonitor.cycle[i].frameCounter;
			drawCallCounter += performanceMonitor.cycle[i].drawCallCounter;
			fastDrawCallCounter += performanceMonitor.cycle[i].fastDrawCallCounter;
			shaderBindCounter += performanceMonitor.cycle[i].shaderBindCount;
			recompilerLeaveCount += performanceMonitor.cycle[i].recompilerLeaveCount;
			threadLeaveCount += performanceMonitor.cycle[i].threadLeaveCount;
		}
		elapsedFrames = std::max<uint32>(elapsedFrames, 1);
		elapsedFrames2S = performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 0) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S += performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S = std::max<uint32>(elapsedFrames2S, 1);
		// calculate stats
		uint64 passedCycles = PPCInterpreter_getMainCoreCycleCounter() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastCycleCount;
		passedCycles -= skippedCycles;
		uint64 vertexDataUploadPerFrame = (vertexDataUploaded / (uint64)elapsedFrames);
		vertexDataUploadPerFrame /= 1024ULL;
		uint64 vertexDataCachedPerFrame = (vertexDataCached / (uint64)elapsedFrames);
		vertexDataCachedPerFrame /= 1024ULL;
		uint64 uniformBankDataUploadedPerFrame = (uniformBankUploadedData / (uint64)elapsedFrames);
		uniformBankDataUploadedPerFrame /= 1024ULL;
		uint32 uniformBankCountUploadedPerFrame = (uint32)(uniformBankUploadedCount / (uint64)elapsedFrames);
		uint64 indexDataUploadPerFrame = (indexDataUploaded / (uint64)elapsedFrames);

		double fps = (double)elapsedFrames2S * 1000.0 / (double)totalElapsedTimeFPS;
		uint32 shaderBindsPerFrame = shaderBindCounter / elapsedFrames;
		passedCycles = passedCycles * 1000ULL / totalElapsedTime;
		uint32 rlps = (uint32)((uint64)recompilerLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		uint32 tlps = (uint32)((uint64)threadLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		// set stats
		performanceMonitor.stats.indexDataUploadPerFrame = indexDataUploadPerFrame;
		// next counter cycle
		sint32 nextCycleIndex = (performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES;
		performanceMonitor.cycle[nextCycleIndex].drawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].fastDrawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].frameCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].shaderBindCount = 0;
		performanceMonitor.cycle[nextCycleIndex].lastCycleCount = PPCInterpreter_getMainCoreCycleCounter();
		performanceMonitor.cycle[nextCycleIndex].skippedCycles = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedData = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedCount = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].recompilerLeaveCount = 0;
		performanceMonitor.cycle[nextCycleIndex].threadLeaveCount = 0;
		performanceMonitor.cycleIndex = nextCycleIndex;

		// next update in 1 second
		performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate = GetTickCount();

		if (isFirstUpdate)
		{
			LatteOverlay_updateStats(0.0, 0, 0);
			WindowSystem::UpdateWindowTitles(false, false, 0.0);
		}
		else
		{
			LatteOverlay_updateStats(fps, drawCallCounter / elapsedFrames, fastDrawCallCounter / elapsedFrames);
			WindowSystem::UpdateWindowTitles(false, false, fps);
			if (s_cacheLogCountdown-- == 0)
			{
				s_cacheLogCountdown = 59;
				LogCacheStats();
			}
		}
	}
}

void LattePerformanceMonitor_frameBegin()
{
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();
}
