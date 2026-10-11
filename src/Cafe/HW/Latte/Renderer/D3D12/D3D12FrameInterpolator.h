#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

// Frame interpolation for presenting at twice the game's frame rate (Settings → "Frame interpolation"): from the
// previous and the current output frame it estimates motion per block (block matching on luma) and builds the
// frame halfway between them, which is presented before the current one. Games like Mario Kart 8 count game time in
// frames, so they can't be patched to run their logic at 120 FPS; this doubles the displayed rate without touching
// the game, at the cost of one extra frame of latency and artifacts where the motion estimate is wrong.
//
// Frames are copies of the swap chain backbuffer (D3D12SwapChain::kFormat). The renderer copies each new frame into
// GetCurrentFrame(), calls Interpolate() once a previous frame exists, copies GetOutput() to a backbuffer and presents,
// then presents the current frame, then calls Advance()
class D3D12FrameInterpolator
{
public:
	explicit D3D12FrameInterpolator(ID3D12Device* device);
	~D3D12FrameInterpolator();

	bool IsValid() const { return m_pipelineMotion != nullptr; }

	// (re)creates the frame textures for this size. Returns false if they can't be created. Waiting for the GPU before a
	// size change is up to the caller
	bool EnsureSize(uint32 width, uint32 height);
	bool HasSize(uint32 width, uint32 height) const { return m_output && width == m_width && height == m_height; }

	ID3D12Resource* GetCurrentFrame() const { return m_frames[m_current].Get(); }
	D3D12_RESOURCE_STATES& GetCurrentFrameState() { return m_frameStates[m_current]; }
	ID3D12Resource* GetPreviousFrame() const { return m_frames[1 - m_current].Get(); }
	D3D12_RESOURCE_STATES& GetPreviousFrameState() { return m_frameStates[1 - m_current]; }
	ID3D12Resource* GetOutput() const { return m_output.Get(); }
	D3D12_RESOURCE_STATES& GetOutputState() { return m_outputState; }
	D3D12_RESOURCE_STATES& GetMotionState() { return m_motionState; }
	ID3D12Resource* GetMotion() const { return m_motion.Get(); }

	bool HasPreviousFrame() const { return m_hasPrevious; }

	// records the two compute passes. Expects both frames in NON_PIXEL_SHADER_RESOURCE, the motion texture and the output
	// in UNORDERED_ACCESS; transitions the motion texture to NON_PIXEL_SHADER_RESOURCE in between (barriers via the
	// callback so the renderer's state tracking stays correct). Changes the command list's descriptor heaps and root
	// signature: submit afterwards, the renderer sets its own again for the next command list
	void Interpolate(ID3D12GraphicsCommandList* cmdList, const std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES&, D3D12_RESOURCE_STATES)>& transition,
		const std::function<void()>& flushBarriers);

	// the current frame becomes the previous one
	void Advance();
	// forget the previous frame (after a size change, a pause, or a scene that wasn't presented)
	void Reset() { m_hasPrevious = false; }

private:
	void CreateDescriptors();

	ID3D12Device* m_device;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineMotion;
	ComPtr<ID3D12PipelineState> m_pipelineInterpolate;
	ComPtr<ID3D12DescriptorHeap> m_heap; // two sets (which frame is the previous one), 5 descriptors each

	uint32 m_width = 0, m_height = 0, m_blocksX = 0, m_blocksY = 0, m_blockSize = 32;
	ComPtr<ID3D12Resource> m_frames[2];
	D3D12_RESOURCE_STATES m_frameStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
	ComPtr<ID3D12Resource> m_motion; // R16G16_SINT, one vector per block
	D3D12_RESOURCE_STATES m_motionState = D3D12_RESOURCE_STATE_COMMON;
	ComPtr<ID3D12Resource> m_output;
	D3D12_RESOURCE_STATES m_outputState = D3D12_RESOURCE_STATE_COMMON;
	uint32 m_current = 0;
	bool m_hasPrevious = false;
};
