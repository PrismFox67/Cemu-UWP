#include "Cafe/HW/Latte/Renderer/D3D12/D3D12FrameInterpolator.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"

#include <d3dcompiler.h>

namespace
{
	constexpr uint32 kDescriptorsPerSet = 5; // t0 previous, t1 current, t2 motion, u0 motion, u1 output

	// Block matching: each thread takes one block of the current frame (32x32 pixels at 1080p, scaled with the output
	// height so the search covers the same part of the screen at 4K), samples its luma on an 8x8 grid and searches the
	// previous frame for the best match, up to about two blocks in each direction.
	// Interpolation: for each output pixel the motion of the surrounding blocks is blended bilinearly; the halfway frame
	// takes the previous frame half a vector ahead and the current frame half a vector back. Where they disagree (an
	// occlusion or a wrong vector) it uses the current frame instead of a blend, which judders but doesn't ghost
	const char* kShaderSource = R"(
cbuffer Params : register(b0) { uint2 size; uint2 blocks; uint blockSize; };
Texture2D<float4> Prev : register(t0);
Texture2D<float4> Cur : register(t1);
Texture2D<int2> MotionIn : register(t2);
RWTexture2D<int2> MotionOut : register(u0);
RWTexture2D<unorm float4> Output : register(u1);
SamplerState LinearClamp : register(s0);

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float CurL(int2 p) { return Luma(Cur.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0)).rgb); }
float PrevL(int2 p) { return Luma(Prev.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0)).rgb); }

float BlockSad(float c[64], int2 origin, int spacing, int2 mv)
{
	// small penalty per pixel of motion: flat areas and static overlays (HUD) prefer zero motion
	float sad = 0.004 * float(abs(mv.x) + abs(mv.y)) / float(spacing);
	[unroll] for (int i = 0; i < 64; i++)
		sad += abs(c[i] - PrevL(origin + int2(i % 8, i / 8) * spacing + mv));
	return sad;
}

void Search(float c[64], int2 origin, int spacing, int2 center, int step, int radius, inout float best, inout int2 bestMv)
{
	[loop] for (int dy = -radius; dy <= radius; dy++)
	{
		[loop] for (int dx = -radius; dx <= radius; dx++)
		{
			const int2 mv = center + int2(dx, dy) * step;
			const float sad = BlockSad(c, origin, spacing, mv);
			if (sad < best) { best = sad; bestMv = mv; }
		}
	}
}

[numthreads(8, 8, 1)]
void MotionCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= blocks.x || id.y >= blocks.y)
		return;
	const int spacing = int(blockSize) / 8;
	const int2 origin = int2(id.xy) * int(blockSize) + spacing / 2;
	float c[64];
	[unroll] for (int i = 0; i < 64; i++)
		c[i] = CurL(origin + int2(i % 8, i / 8) * spacing);
	const float zeroSad = BlockSad(c, origin, spacing, int2(0, 0));
	float best = zeroSad;
	int2 bestMv = int2(0, 0);
	// three step search: +-16 samples in steps of 4, +-3 samples around the best, then half a sample
	Search(c, origin, spacing, int2(0, 0), spacing * 4, 4, best, bestMv);
	Search(c, origin, spacing, bestMv, spacing, 3, best, bestMv);
	if (spacing > 1)
		Search(c, origin, spacing, bestMv, max(spacing / 2, 1), 1, best, bestMv);
	// only move if it is clearly better than standing still
	if (best > zeroSad * 0.85)
		bestMv = int2(0, 0);
	MotionOut[id.xy] = bestMv;
}

// how well the previous frame half a vector ahead matches the current frame half a vector back, around this pixel
float CandidateCost(float2 uv, float2 texel, float2 mv)
{
	const float2 halfMv = mv * 0.5 * texel;
	const float2 offsets[5] = { float2(0, 0), float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2) };
	float cost = 0;
	[unroll] for (int i = 0; i < 5; i++)
	{
		const float2 p = uv + offsets[i] * texel;
		const float3 a = Prev.SampleLevel(LinearClamp, p + halfMv, 0).rgb;
		const float3 b = Cur.SampleLevel(LinearClamp, p - halfMv, 0).rgb;
		const float3 d = abs(a - b);
		cost += d.r + d.g + d.b;
	}
	return cost;
}

[numthreads(8, 8, 1)]
void InterpolateCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= size.x || id.y >= size.y)
		return;
	const float2 texel = 1.0 / float2(size);
	const float2 uv = (float2(id.xy) + 0.5) * texel;
	// candidates: the smooth (bilinear) field, the vectors of this block and its eight neighbors, and no motion. Each pixel
	// takes the one under which both frames agree, so objects keep their own motion up to their edges instead of being
	// cut into blocks
	const float2 bp = (float2(id.xy) + 0.5) / float(blockSize) - 0.5;
	const int2 b0 = clamp(int2(floor(bp)), int2(0, 0), int2(blocks) - 1);
	const int2 b1 = clamp(b0 + 1, int2(0, 0), int2(blocks) - 1);
	const float2 f = saturate(bp - floor(bp));
	const float2 m00 = float2(MotionIn.Load(int3(b0, 0))), m10 = float2(MotionIn.Load(int3(b1.x, b0.y, 0)));
	const float2 m01 = float2(MotionIn.Load(int3(b0.x, b1.y, 0))), m11 = float2(MotionIn.Load(int3(b1, 0)));
	float2 bestMv = lerp(lerp(m00, m10, f.x), lerp(m01, m11, f.x), f.y);
	float best = CandidateCost(uv, texel, bestMv) * 0.9; // slight preference for the smooth field
	const float zeroCost = CandidateCost(uv, texel, float2(0, 0)) * 0.95;
	if (zeroCost < best) { best = zeroCost; bestMv = float2(0, 0); }
	const int2 home = clamp(int2(id.xy) / int(blockSize), int2(0, 0), int2(blocks) - 1);
	[loop] for (int i = 0; i < 9; i++)
	{
		const int2 b = clamp(home + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), int2(blocks) - 1);
		const float2 mv = float2(MotionIn.Load(int3(b, 0)));
		const float cost = CandidateCost(uv, texel, mv);
		if (cost < best) { best = cost; bestMv = mv; }
	}
	// current pixel x came from previous x + mv: halfway, the content at q was at q + mv/2 before and is at q - mv/2 now
	const float2 halfMv = bestMv * 0.5 * texel;
	const float4 a = Prev.SampleLevel(LinearClamp, uv + halfMv, 0);
	const float4 b = Cur.SampleLevel(LinearClamp, uv - halfMv, 0);
	// nothing fits (occlusion, effects, a scene cut): show the current frame there, which judders but doesn't ghost
	const float4 current = Cur.Load(int3(id.xy, 0));
	Output[id.xy] = lerp(0.5 * (a + b), current, saturate((best / 15.0 - 0.06) * 10.0));
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
	ranges[0].NumDescriptors = 3;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 2;
	ranges[1].OffsetInDescriptorsFromTableStart = 3;
	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.Num32BitValues = 5;
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
	ComPtr<ID3DBlob> motion = _Compile("MotionCS"), interpolate = _Compile("InterpolateCS");
	if (!motion || !interpolate)
		return;
	D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
	pso.pRootSignature = m_rootSignature.Get();
	pso.CS = { interpolate->GetBufferPointer(), interpolate->GetBufferSize() };
	if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pipelineInterpolate))))
		return;
	pso.CS = { motion->GetBufferPointer(), motion->GetBufferSize() };
	if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pipelineMotion))))
	{
		m_pipelineMotion.Reset();
		return;
	}
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorsPerSet * 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap))))
	{
		m_pipelineMotion.Reset();
		return;
	}
	cemuLog_log(LogType::Force, "D3D12: Frame interpolation ready");
}

D3D12FrameInterpolator::~D3D12FrameInterpolator() = default;

bool D3D12FrameInterpolator::EnsureSize(uint32 width, uint32 height)
{
	if (width == m_width && height == m_height && m_output)
		return true;
	m_width = width;
	m_height = height;
	m_blockSize = 32 * std::max<uint32>(1, (height + 540) / 1080);
	m_blocksX = (width + m_blockSize - 1) / m_blockSize;
	m_blocksY = (height + m_blockSize - 1) / m_blockSize;
	m_hasPrevious = false;
	auto create = [this](uint32 w, uint32 h, DXGI_FORMAT format, bool uav, ComPtr<ID3D12Resource>& out) {
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
		out.Reset();
		return SUCCEEDED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)));
	};
	const bool ok = create(width, height, D3D12SwapChain::kFormat, false, m_frames[0]) && create(width, height, D3D12SwapChain::kFormat, false, m_frames[1]) &&
		create(m_blocksX, m_blocksY, DXGI_FORMAT_R16G16_SINT, true, m_motion) && create(width, height, D3D12SwapChain::kFormat, true, m_output);
	m_frameStates[0] = m_frameStates[1] = m_motionState = m_outputState = D3D12_RESOURCE_STATE_COMMON;
	if (!ok)
	{
		cemuLog_log(LogType::Force, "D3D12: Frame interpolation can't create its {}x{} textures", width, height);
		m_output.Reset();
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
		// set N is used while frame N is the current one
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (SIZE_T)set * kDescriptorsPerSet * increment;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		srv.Format = D3D12SwapChain::kFormat;
		m_device->CreateShaderResourceView(m_frames[1 - set].Get(), &srv, h);
		h.ptr += increment;
		m_device->CreateShaderResourceView(m_frames[set].Get(), &srv, h);
		h.ptr += increment;
		srv.Format = DXGI_FORMAT_R16G16_SINT;
		m_device->CreateShaderResourceView(m_motion.Get(), &srv, h);
		h.ptr += increment;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Format = DXGI_FORMAT_R16G16_SINT;
		m_device->CreateUnorderedAccessView(m_motion.Get(), nullptr, &uav, h);
		h.ptr += increment;
		uav.Format = D3D12SwapChain::kFormat;
		m_device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uav, h);
	}
}

void D3D12FrameInterpolator::Interpolate(ID3D12GraphicsCommandList* cmdList, const std::function<void(ID3D12Resource*, D3D12_RESOURCE_STATES&, D3D12_RESOURCE_STATES)>& transition,
	const std::function<void()>& flushBarriers)
{
	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	transition(GetPreviousFrame(), GetPreviousFrameState(), read);
	transition(GetCurrentFrame(), GetCurrentFrameState(), read);
	transition(m_motion.Get(), m_motionState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	transition(m_output.Get(), m_outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	flushBarriers();

	ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
	cmdList->SetDescriptorHeaps(1, heaps);
	cmdList->SetComputeRootSignature(m_rootSignature.Get());
	const uint32 constants[5] = { m_width, m_height, m_blocksX, m_blocksY, m_blockSize };
	cmdList->SetComputeRoot32BitConstants(0, 5, constants, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE table = m_heap->GetGPUDescriptorHandleForHeapStart();
	table.ptr += (UINT64)m_current * kDescriptorsPerSet * m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	cmdList->SetComputeRootDescriptorTable(1, table);

	cmdList->SetPipelineState(m_pipelineMotion.Get());
	cmdList->Dispatch((m_blocksX + 7) / 8, (m_blocksY + 7) / 8, 1);
	transition(m_motion.Get(), m_motionState, read);
	flushBarriers();
	cmdList->SetPipelineState(m_pipelineInterpolate.Get());
	cmdList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
}

void D3D12FrameInterpolator::Advance()
{
	m_current = 1 - m_current;
	m_hasPrevious = true;
}
