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

	bool IsValid() const { return m_valid; }

	// (re)creates the frame textures for this size. Returns false if they can't be created. Waiting for the GPU before a
	// size change is up to the caller
	bool EnsureSize(uint32 width, uint32 height);
	bool HasSize(uint32 width, uint32 height) const { return m_output.resource && width == m_width && height == m_height; }

	ID3D12Resource* GetCurrentFrame() const { return m_frames[m_current].resource.Get(); }
	D3D12_RESOURCE_STATES& GetCurrentFrameState() { return m_frames[m_current].state; }
	ID3D12Resource* GetPreviousFrame() const { return m_frames[1 - m_current].resource.Get(); }
	D3D12_RESOURCE_STATES& GetPreviousFrameState() { return m_frames[1 - m_current].state; }
	ID3D12Resource* GetOutput() const { return m_output.resource.Get(); }
	D3D12_RESOURCE_STATES& GetOutputState() { return m_output.state; }

	bool HasPreviousFrame() const { return m_hasPrevious; }

	// records the compute passes that fill GetOutput(). Barriers go through the callbacks so the renderer's state tracking
	// stays correct. Changes the command list's descriptor heaps and root signature: submit afterwards, the renderer sets
	// its own again for the next command list
	void Interpolate(ID3D12GraphicsCommandList* cmdList, const std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES&, D3D12_RESOURCE_STATES)>& transition,
		const std::function<void()>& flushBarriers);

	// the current frame becomes the previous one
	void Advance();
	// forget the previous frame (after a size change, a pause, or a scene that wasn't presented)
	void Reset() { m_hasPrevious = false; }

private:
	void CreateDescriptors();

	struct Texture
	{
		ComPtr<ID3D12Resource> resource;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	};
	static constexpr uint32 kPassCount = 4;

	ID3D12Device* m_device;
	bool m_valid = false;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelines[kPassCount]; // luma, motion, select, interpolate
	ComPtr<ID3D12DescriptorHeap> m_heap; // two sets (which frame is the previous one)

	uint32 m_width = 0, m_height = 0;
	uint32 m_blockSize = 32, m_blocksX = 0, m_blocksY = 0;
	uint32 m_tileSize = 4, m_tilesX = 0, m_tilesY = 0;
	Texture m_frames[2];
	Texture m_motion; // one vector per block
	Texture m_tiles; // vector and misfit per tile
	Texture m_luma[2]; // previous and current frame, per tile
	Texture m_output;
	uint32 m_current = 0;
	bool m_hasPrevious = false;
};
