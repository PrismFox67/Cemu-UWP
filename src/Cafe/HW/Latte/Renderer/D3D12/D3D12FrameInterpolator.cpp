#include "Cafe/HW/Latte/Renderer/D3D12/D3D12FrameInterpolator.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"

#include <d3dcompiler.h>

namespace
{
	constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT; // pixels, one vector per block
	constexpr DXGI_FORMAT kTileFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // vector, misfit
	constexpr DXGI_FORMAT kLumaFormat = DXGI_FORMAT_R16_FLOAT;
	constexpr uint32 kSRVs = 6;
	constexpr uint32 kUAVs = 5;

	// Four passes. Sizes scale with the output height so the work and the covered part of the screen stay the same at 4K:
	// tiles are 4x4 pixels at 1080p, blocks 8x8 tiles.
	// LumaCS: average luma of both frames per tile.
	// MotionCS: block matching on the tile luma, one vector per block.
	// SelectCS: per tile, picks the best fitting block vector from the neighborhood, kept close to the neighborhood median.
	// InterpolateCS: per pixel, the previous frame half a vector ahead blended with the current frame half a vector back,
	// with fallbacks where they don't agree.
	const char* kShaderSource = R"(
cbuffer Params : register(b0) { uint2 size; uint2 blocks; uint blockSize; uint tileSize; uint2 tiles; };
Texture2D<float4> Prev : register(t0);
Texture2D<float4> Cur : register(t1);
Texture2D<float2> MotionIn : register(t2);
Texture2D<float4> TilesIn : register(t3);
Texture2D<float> PrevLuma : register(t4);
Texture2D<float> CurLuma : register(t5);
RWTexture2D<float2> MotionOut : register(u0);
RWTexture2D<unorm float4> Output : register(u1);
RWTexture2D<float4> TilesOut : register(u2);
RWTexture2D<float> PrevLumaOut : register(u3);
RWTexture2D<float> CurLumaOut : register(u4);
SamplerState LinearClamp : register(s0);

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// luma of both frames averaged over each tile (4x4 pixels at 1080p): block matching on this doesn't alias on fine,
// noisy textures (asphalt, grass) like point samples of the full frame do
[numthreads(8, 8, 1)]
void LumaCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= tiles.x || id.y >= tiles.y)
		return;
	// each bilinear fetch between four pixel centers averages a 2x2 quad
	const float2 origin = float2(id.xy) * float(tileSize);
	const float2 texel = 1.0 / float2(size);
	float p = 0, c = 0;
	[loop] for (uint y = 0; y < tileSize; y += 2)
	{
		[loop] for (uint x = 0; x < tileSize; x += 2)
		{
			const float2 uv = (origin + float2(x, y) + 1.0) * texel;
			p += Luma(Prev.SampleLevel(LinearClamp, uv, 0).rgb);
			c += Luma(Cur.SampleLevel(LinearClamp, uv, 0).rgb);
		}
	}
	const float n = float(tileSize * tileSize) * 0.25;
	PrevLumaOut[id.xy] = p / n;
	CurLumaOut[id.xy] = c / n;
}

// in tile units; mv can be fractional (the previous frame's luma is sampled bilinearly)
float BlockSad(float c[64], int2 origin, float2 mv)
{
	// small penalty per tile of motion: flat areas and static overlays (HUD) prefer zero motion
	float sad = 0.02 * (abs(mv.x) + abs(mv.y));
	const float2 invTiles = 1.0 / float2(tiles);
	[unroll] for (int i = 0; i < 64; i++)
		sad += abs(c[i] - PrevLuma.SampleLevel(LinearClamp, (float2(origin + int2(i % 8, i / 8)) + 0.5 + mv) * invTiles, 0));
	return sad;
}

void Search(float c[64], int2 origin, float2 center, float step, int radius, inout float best, inout float2 bestMv)
{
	[loop] for (int dy = -radius; dy <= radius; dy++)
	{
		[loop] for (int dx = -radius; dx <= radius; dx++)
		{
			const float2 mv = center + float2(dx, dy) * step;
			const float sad = BlockSad(c, origin, mv);
			if (sad < best) { best = sad; bestMv = mv; }
		}
	}
}

// one thread per block (8x8 tiles, 32x32 pixels at 1080p): search the previous frame's tile luma for the best match,
// +-16 tiles in steps of 4, +-3 around the best, then half and quarter tiles
[numthreads(8, 8, 1)]
void MotionCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= blocks.x || id.y >= blocks.y)
		return;
	const int2 origin = int2(id.xy) * 8;
	float c[64];
	[unroll] for (int i = 0; i < 64; i++)
		c[i] = CurLuma.Load(int3(min(origin + int2(i % 8, i / 8), int2(tiles) - 1), 0));
	const float zeroSad = BlockSad(c, origin, float2(0, 0));
	float best = zeroSad;
	float2 bestMv = float2(0, 0);
	Search(c, origin, float2(0, 0), 4.0, 4, best, bestMv);
	Search(c, origin, bestMv, 1.0, 3, best, bestMv);
	Search(c, origin, bestMv, 0.5, 1, best, bestMv);
	Search(c, origin, bestMv, 0.25, 1, best, bestMv);
	// only move if it is clearly better than standing still
	if (best > zeroSad * 0.85)
		bestMv = float2(0, 0);
	MotionOut[id.xy] = bestMv * float(tileSize);
}

// how well the previous frame half a vector ahead matches the current frame half a vector back, on a 4x4 grid around
// center (pixel coordinates). Average absolute difference per color channel
float TileCost(float2 center, float step, float2 texel, float2 mv)
{
	const float2 halfMv = mv * 0.5;
	float cost = 0;
	// 8 of the 4x4 grid in a checkerboard
	[unroll] for (int i = 0; i < 8; i++)
	{
		const float2 p = center + (float2((i % 2) * 2 + (i / 2) % 2, i / 2) - 1.5) * step;
		const float3 a = Prev.SampleLevel(LinearClamp, (p + halfMv) * texel, 0).rgb;
		const float3 b = Cur.SampleLevel(LinearClamp, (p - halfMv) * texel, 0).rgb;
		const float3 d = abs(a - b);
		cost += d.r + d.g + d.b;
	}
	return cost / 24.0;
}

// one thread per tile (4x4 pixels at 1080p). Candidates: the vectors of the tile's block and its eight neighbors, their
// median, the smooth (bilinear) block field and no motion. The tile takes the one under which both frames agree best on
// a patch twice its size, plus a penalty for straying from the median: on dark, low contrast or repetitive surfaces many
// vectors fit about equally and block matching alone picks noise. Objects still keep their own motion up to their edges
// instead of being cut into blocks. Stores the vector and how badly it fits
[numthreads(8, 8, 1)]
void SelectCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= tiles.x || id.y >= tiles.y)
		return;
	const float2 texel = 1.0 / float2(size);
	const float2 center = (float2(id.xy) + 0.5) * float(tileSize);
	const float step = float(tileSize) * 0.5;
	const int2 home = clamp(int2(center) / int(blockSize), int2(0, 0), int2(blocks) - 1);
	float2 cand[12];
	[unroll] for (int i = 0; i < 9; i++)
		cand[i] = MotionIn.Load(int3(clamp(home + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), int2(blocks) - 1), 0));
	// vector median: the neighbor vector closest to all others
	float bestSum = 1e30;
	float2 median = cand[4];
	[unroll] for (int k = 0; k < 9; k++)
	{
		float sum = 0;
		[unroll] for (int j = 0; j < 9; j++)
			sum += abs(cand[k].x - cand[j].x) + abs(cand[k].y - cand[j].y);
		if (sum < bestSum) { bestSum = sum; median = cand[k]; }
	}
	const float2 bp = center / float(blockSize) - 0.5;
	const int2 b0 = clamp(int2(floor(bp)), int2(0, 0), int2(blocks) - 1);
	const int2 b1 = clamp(b0 + 1, int2(0, 0), int2(blocks) - 1);
	const float2 f = saturate(bp - floor(bp));
	const float2 m00 = MotionIn.Load(int3(b0, 0)), m10 = MotionIn.Load(int3(b1.x, b0.y, 0));
	const float2 m01 = MotionIn.Load(int3(b0.x, b1.y, 0)), m11 = MotionIn.Load(int3(b1, 0));
	cand[9] = median;
	cand[10] = lerp(lerp(m00, m10, f.x), lerp(m01, m11, f.x), f.y);
	cand[11] = float2(0, 0);
	const float lambda = 0.0015 / float(tileSize) * 4.0; // per pixel of deviation from the median (1080p)
	const float shortness = 0.005 / float(tileSize) * 4.0; // per pixel of motion: on ambiguous content (fine stripes, flat areas) prefer standing still
	float best = 1e30, bestFit = 0;
	float2 bestMv = median;
	[unroll] for (int n = 0; n < 12; n++)
	{
		// neighbors mostly share their vector: test each distinct one once
		bool seen = false;
		[unroll] for (int m = 0; m < n; m++)
			seen = seen || all(cand[m] == cand[n]);
		if (seen)
			continue;
		const float fit = TileCost(center, step, texel, cand[n]);
		const float cost = fit + lambda * (abs(cand[n].x - median.x) + abs(cand[n].y - median.y)) + shortness * (abs(cand[n].x) + abs(cand[n].y));
		if (cost < best) { best = cost; bestFit = fit; bestMv = cand[n]; }
	}
	TilesOut[id.xy] = float4(bestMv, bestFit, 0);
}

[numthreads(8, 8, 1)]
void InterpolateCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= size.x || id.y >= size.y)
		return;
	const float2 texel = 1.0 / float2(size);
	const float2 uv = (float2(id.xy) + 0.5) * texel;
	const float4 tile = TilesIn.SampleLevel(LinearClamp, (float2(id.xy) + 0.5) / (float2(tiles) * float(tileSize)), 0);
	// current pixel x came from previous x + mv: halfway, the content at q was at q + mv/2 before and is at q - mv/2 now
	const float2 halfMv = tile.xy * 0.5 * texel;
	const float4 a = Prev.SampleLevel(LinearClamp, uv + halfMv, 0);
	const float4 b = Cur.SampleLevel(LinearClamp, uv - halfMv, 0);
	// where no vector fits (occlusion, effects, fades, a scene cut) a plain blend of the two frames. Measured against real
	// middle frames (dumped triplets) this beat showing the current frame, and warping only one side per pixel
	const float4 plain = 0.5 * (Prev.Load(int3(id.xy, 0)) + Cur.Load(int3(id.xy, 0)));
	Output[id.xy] = lerp(0.5 * (a + b), plain, saturate((tile.z - 0.06) * 10.0));
}
)";

	ComPtr<ID3DBlob> _Compile(const char* entry)
	{
		ComPtr<ID3DBlob> code, errors;
		const HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "frame_interpolation", nullptr, nullptr, entry, "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
		if (FAILED(hr))
		{
			cemuLog_log(LogType::Force, "D3D12: Frame interpolation shader {} failed to compile: {}", entry,
				errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : D3D12_HResultToString(hr));
			return nullptr;
		}
		return code;
	}
}

D3D12FrameInterpolator::D3D12FrameInterpolator(ID3D12Device* device) : m_device(device)
{
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = kSRVs;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = kUAVs;
	ranges[1].OffsetInDescriptorsFromTableStart = kSRVs;
	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.Num32BitValues = 8;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 2;
	params[1].DescriptorTable.pDescriptorRanges = ranges;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = 2;
	desc.pParameters = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &sampler;
	ComPtr<ID3DBlob> blob, error;
	if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error.GetAddressOf())) ||
		FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature))))
	{
		cemuLog_log(LogType::Force, "D3D12: Frame interpolation root signature failed");
		return;
	}
	const char* entries[kPassCount] = { "LumaCS", "MotionCS", "SelectCS", "InterpolateCS" };
	for (uint32 i = 0; i < kPassCount; i++)
	{
		ComPtr<ID3DBlob> code = _Compile(entries[i]);
		if (!code)
			return;
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = m_rootSignature.Get();
		pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
		if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pipelines[i]))))
			return;
	}
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = (kSRVs + kUAVs) * 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap))))
		return;
	m_valid = true;
	cemuLog_log(LogType::Force, "D3D12: Frame interpolation ready");
}

D3D12FrameInterpolator::~D3D12FrameInterpolator() = default;

bool D3D12FrameInterpolator::EnsureSize(uint32 width, uint32 height)
{
	if (HasSize(width, height))
		return true;
	m_width = width;
	m_height = height;
	m_blockSize = 32 * std::max<uint32>(1, (height + 540) / 1080);
	m_tileSize = m_blockSize / 8;
	m_blocksX = (width + m_blockSize - 1) / m_blockSize;
	m_blocksY = (height + m_blockSize - 1) / m_blockSize;
	m_tilesX = (width + m_tileSize - 1) / m_tileSize;
	m_tilesY = (height + m_tileSize - 1) / m_tileSize;
	m_hasPrevious = false;
	auto create = [this](uint32 w, uint32 h, DXGI_FORMAT format, bool uav, Texture& out) {
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
		out.resource.Reset();
		out.state = D3D12_RESOURCE_STATE_COMMON;
		return SUCCEEDED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out.resource)));
	};
	const bool ok = create(width, height, D3D12SwapChain::kFormat, false, m_frames[0]) && create(width, height, D3D12SwapChain::kFormat, false, m_frames[1]) &&
		create(m_blocksX, m_blocksY, kMotionFormat, true, m_motion) && create(m_tilesX, m_tilesY, kTileFormat, true, m_tiles) &&
		create(m_tilesX, m_tilesY, kLumaFormat, true, m_luma[0]) && create(m_tilesX, m_tilesY, kLumaFormat, true, m_luma[1]) &&
		create(width, height, D3D12SwapChain::kFormat, true, m_output);
	if (!ok)
	{
		cemuLog_log(LogType::Force, "D3D12: Frame interpolation can't create its {}x{} textures", width, height);
		m_output.resource.Reset();
		m_width = m_height = 0;
		return false;
	}
	CreateDescriptors();
	return true;
}

void D3D12FrameInterpolator::CreateDescriptors()
{
	const uint32 increment = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (uint32 set = 0; set < 2; set++)
	{
		// set N is used while frame N is the current one. Layout matches the shader registers: t0 previous frame, t1 current
		// frame, t2 motion, t3 tiles, t4/t5 previous/current luma, then u0 motion, u1 output, u2 tiles, u3/u4 luma
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (SIZE_T)set * (kSRVs + kUAVs) * increment;
		auto srv = [&](ID3D12Resource* resource, DXGI_FORMAT format) {
			D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.Texture2D.MipLevels = 1;
			desc.Format = format;
			m_device->CreateShaderResourceView(resource, &desc, h);
			h.ptr += increment;
		};
		auto uav = [&](ID3D12Resource* resource, DXGI_FORMAT format) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			desc.Format = format;
			m_device->CreateUnorderedAccessView(resource, nullptr, &desc, h);
			h.ptr += increment;
		};
		srv(m_frames[1 - set].resource.Get(), D3D12SwapChain::kFormat);
		srv(m_frames[set].resource.Get(), D3D12SwapChain::kFormat);
		srv(m_motion.resource.Get(), kMotionFormat);
		srv(m_tiles.resource.Get(), kTileFormat);
		srv(m_luma[0].resource.Get(), kLumaFormat);
		srv(m_luma[1].resource.Get(), kLumaFormat);
		uav(m_motion.resource.Get(), kMotionFormat);
		uav(m_output.resource.Get(), D3D12SwapChain::kFormat);
		uav(m_tiles.resource.Get(), kTileFormat);
		uav(m_luma[0].resource.Get(), kLumaFormat);
		uav(m_luma[1].resource.Get(), kLumaFormat);
	}
}

void D3D12FrameInterpolator::Interpolate(ID3D12GraphicsCommandList* cmdList, const std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES&, D3D12_RESOURCE_STATES)>& transition,
	const std::function<void()>& flushBarriers)
{
	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	auto to = [&](Texture& texture, D3D12_RESOURCE_STATES state) { transition(texture.resource.Get(), texture.state, state); };

	ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
	cmdList->SetDescriptorHeaps(1, heaps);
	cmdList->SetComputeRootSignature(m_rootSignature.Get());
	const uint32 constants[8] = { m_width, m_height, m_blocksX, m_blocksY, m_blockSize, m_tileSize, m_tilesX, m_tilesY };
	cmdList->SetComputeRoot32BitConstants(0, 8, constants, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE table = m_heap->GetGPUDescriptorHandleForHeapStart();
	table.ptr += (UINT64)m_current * (kSRVs + kUAVs) * m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	cmdList->SetComputeRootDescriptorTable(1, table);
	auto dispatch = [&](uint32 pass, uint32 threadsX, uint32 threadsY) {
		flushBarriers();
		cmdList->SetPipelineState(m_pipelines[pass].Get());
		cmdList->Dispatch((threadsX + 7) / 8, (threadsY + 7) / 8, 1);
	};

	to(m_frames[0], read);
	to(m_frames[1], read);
	to(m_luma[0], write);
	to(m_luma[1], write);
	dispatch(0, m_tilesX, m_tilesY); // luma of both frames per tile
	to(m_luma[0], read);
	to(m_luma[1], read);
	to(m_motion, write);
	dispatch(1, m_blocksX, m_blocksY); // block motion
	to(m_motion, read);
	to(m_tiles, write);
	dispatch(2, m_tilesX, m_tilesY); // motion per tile
	to(m_tiles, read);
	to(m_output, write);
	dispatch(3, m_width, m_height); // the halfway frame
}

void D3D12FrameInterpolator::Advance()
{
	m_current = 1 - m_current;
	m_hasPrevious = true;
}
