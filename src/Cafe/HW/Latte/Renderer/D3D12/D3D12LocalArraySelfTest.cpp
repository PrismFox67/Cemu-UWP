#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"

// #449 D0: boot self-test for the Xbox driver bug behind BOTW's Magnesis (candidate 1). A pixel shader that keeps data in
// a local register array the compiler can't forward in registers (Cemu's emitter declares ivec4 Ri[128] for relative GPR
// writes) reads 0 back on Xbox Series. Renders one pixel per compiler variant into a 1x1 R32G32B32A32_UINT target and
// compares the result with the expected values. Indices and seeds come from root constants so nothing can be folded:
//   x: dynamic store, constant load         -> seed
//   y: constant store, dynamic load         -> seed * 3
//   z: counter kept in the array in a loop  -> 1 + 2 + ... + 16 = 136
//   w: 16-entry table, select loop          -> 5 * 5 + seed
// Any failure (compiler missing, PSO, device) is logged and skipped, the renderer never depends on this test

namespace
{
	constexpr sint32 kSeed = 0x1234;
	constexpr uint32 kExpected[4] = { (uint32)kSeed, (uint32)kSeed * 3, 136, 25 + (uint32)kSeed };
	// c0 = (dynamic store index 37, dynamic load index 90, loop count 16, seed), c1 = (table base 64, table entry 5, counter index 100, 0)
	constexpr sint32 kConstants[8] = { 37, 90, 16, kSeed, 64, 5, 100, 0 };

	std::atomic<D3D12LocalArrayResult> s_activeResult{ D3D12LocalArrayResult::NotTested };

	const char* kVertexHLSL = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

	const char* kPixelHLSL = R"(
cbuffer SelfTestConstants : register(b0, space30)
{
	int4 c0;
	int4 c1;
};

uint4 main(float4 pos : SV_Position) : SV_Target0
{
	int4 R[128];
	for (int i = 0; i < 128; i++)
		R[i] = int4(0, 0, 0, 0);
	R[c0.x] = int4(c0.w, c0.w + 1, c0.w + 2, c0.w + 3);
	int4 a = R[37];
	R[90] = int4(c0.w * 3, 7, 11, 13);
	int4 b = R[c0.y];
	for (int k = 0; k < c0.z; k++)
		R[c1.z].x += 1 + k;
	int counter = R[100].x;
	for (int t = 0; t < 16; t++)
		R[c1.x + t] = int4(t * 5 + c0.w, t, 0, 0);
	int sel = 0;
	for (int s = 0; s < 16; s++)
		sel = (s == c1.y) ? R[c1.x + s].x : sel;
	return uint4(a.x, b.x, counter, sel);
}
)";

	// the same test shaped like the GLSL Cemu's decompiler emits (Ri array, ARi index register), through the real
	// GLSL -> SPIR-V -> SPIRV-Cross path. Push constants land in b0 space30, gl_VertexIndex reads b0 space31
	const char* kVertexGLSL = R"(#version 430
void main()
{
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

	const char* kPixelGLSL = R"(#version 430
layout(push_constant) uniform SelfTestBlock
{
	ivec4 c0;
	ivec4 c1;
} st;
layout(location = 0) out uvec4 passPixelColor0;
void main()
{
ivec4 Ri[128];
for (int i = 0; i < 128; i++)
	Ri[i] = ivec4(0);
ivec4 ARi = ivec4(0);
ARi.x = st.c0.x;
Ri[0+ARi.x] = ivec4(st.c0.w, st.c0.w + 1, st.c0.w + 2, st.c0.w + 3);
ivec4 a = Ri[37];
Ri[90] = ivec4(st.c0.w * 3, 7, 11, 13);
ARi.y = st.c0.y;
ivec4 b = Ri[0+ARi.y];
for (int k = 0; k < st.c0.z; k++)
	Ri[st.c1.z].x += 1 + k;
int counter = Ri[100].x;
for (int t = 0; t < 16; t++)
	Ri[st.c1.x + t] = ivec4(t * 5 + st.c0.w, t, 0, 0);
int sel = 0;
for (int s = 0; s < 16; s++)
	sel = (s == st.c1.y) ? Ri[st.c1.x + s].x : sel;
passPixelColor0 = uvec4(a.x, b.x, counter, sel);
}
)";

	class LocalArraySelfTest
	{
	public:
		LocalArraySelfTest(ID3D12Device* device, ID3D12CommandQueue* queue) : m_device(device), m_queue(queue) {}

		~LocalArraySelfTest()
		{
			if (m_fenceEvent)
				CloseHandle(m_fenceEvent);
		}

		bool Init(std::string& errorOut)
		{
			D3D12_ROOT_PARAMETER params[2]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 0;
			params[0].Constants.RegisterSpace = D3D12Const::kSpacePushConstants;
			params[0].Constants.Num32BitValues = 8;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].Constants.ShaderRegister = 0;
			params[1].Constants.RegisterSpace = D3D12Const::kSpaceRuntimeData;
			params[1].Constants.Num32BitValues = D3D12Const::kRuntimeDataDwords;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
			D3D12_ROOT_SIGNATURE_DESC desc{};
			desc.NumParameters = 2;
			desc.pParameters = params;
			ComPtr<ID3DBlob> blob, error;
			HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error.GetAddressOf());
			if (SUCCEEDED(hr))
				hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
			if (FAILED(hr))
				return _Fail(errorOut, "root signature", hr);

			D3D12_HEAP_PROPERTIES defaultHeap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			D3D12_RESOURCE_DESC texDesc{};
			texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			texDesc.Width = 1;
			texDesc.Height = 1;
			texDesc.DepthOrArraySize = 1;
			texDesc.MipLevels = 1;
			texDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
			texDesc.SampleDesc.Count = 1;
			texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			hr = m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&m_target));
			if (FAILED(hr))
				return _Fail(errorOut, "render target", hr);
			D3D12_SetDebugName(m_target.Get(), "LocalArraySelfTestTarget");

			D3D12_HEAP_PROPERTIES readbackHeap = D3D12_HeapProps(D3D12_HEAP_TYPE_READBACK);
			D3D12_RESOURCE_DESC bufferDesc = D3D12_BufferDesc(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
			hr = m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback));
			if (FAILED(hr))
				return _Fail(errorOut, "readback buffer", hr);

			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			heapDesc.NumDescriptors = 1;
			hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap));
			if (FAILED(hr))
				return _Fail(errorOut, "RTV heap", hr);
			m_rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
			m_device->CreateRenderTargetView(m_target.Get(), nullptr, m_rtv);

			hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator));
			if (SUCCEEDED(hr))
				hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList));
			if (SUCCEEDED(hr))
				hr = m_cmdList->Close();
			if (SUCCEEDED(hr))
				hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
			if (FAILED(hr))
				return _Fail(errorOut, "command list", hr);
			m_fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			if (!m_fenceEvent)
				return _Fail(errorOut, "fence event", E_FAIL);
			return true;
		}

		// renders the pixel and reads it back. False with errorOut set if the test couldn't run
		bool Run(const std::vector<uint8>& vs, const std::vector<uint8>& ps, uint32 valuesOut[4], std::string& errorOut)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = m_rootSignature.Get();
			desc.VS = { vs.data(), vs.size() };
			desc.PS = { ps.data(), ps.size() };
			desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			desc.SampleMask = UINT_MAX;
			desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
			desc.SampleDesc.Count = 1;
			ComPtr<ID3D12PipelineState> pso;
			HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
				return _Fail(errorOut, "CreateGraphicsPipelineState", hr);

			hr = m_allocator->Reset();
			if (SUCCEEDED(hr))
				hr = m_cmdList->Reset(m_allocator.Get(), pso.Get());
			if (FAILED(hr))
				return _Fail(errorOut, "command list reset", hr);
			const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			m_cmdList->ClearRenderTargetView(m_rtv, clear, 0, nullptr);
			m_cmdList->OMSetRenderTargets(1, &m_rtv, FALSE, nullptr);
			const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
			const D3D12_RECT scissor{ 0, 0, 1, 1 };
			m_cmdList->RSSetViewports(1, &viewport);
			m_cmdList->RSSetScissorRects(1, &scissor);
			m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
			m_cmdList->SetGraphicsRoot32BitConstants(0, 8, kConstants, 0);
			const uint32 runtimeData[D3D12Const::kRuntimeDataDwords]{};
			m_cmdList->SetGraphicsRoot32BitConstants(1, D3D12Const::kRuntimeDataDwords, runtimeData, 0);
			m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_cmdList->DrawInstanced(3, 1, 0, 0);
			D3D12_RESOURCE_BARRIER barrier = D3D12_TransitionBarrier(m_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			m_cmdList->ResourceBarrier(1, &barrier);
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = m_target.Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = m_readback.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_UINT;
			dst.PlacedFootprint.Footprint.Width = 1;
			dst.PlacedFootprint.Footprint.Height = 1;
			dst.PlacedFootprint.Footprint.Depth = 1;
			dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
			m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			barrier = D3D12_TransitionBarrier(m_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			m_cmdList->ResourceBarrier(1, &barrier);
			hr = m_cmdList->Close();
			if (FAILED(hr))
				return _Fail(errorOut, "command list close", hr);
			ID3D12CommandList* lists[] = { m_cmdList.Get() };
			m_queue->ExecuteCommandLists(1, lists);
			m_fenceValue++;
			hr = m_queue->Signal(m_fence.Get(), m_fenceValue);
			if (FAILED(hr))
				return _Fail(errorOut, "Signal", hr);
			if (m_fence->GetCompletedValue() < m_fenceValue)
			{
				m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
				if (WaitForSingleObject(m_fenceEvent, 5000) != WAIT_OBJECT_0)
				{
					const HRESULT removed = m_device->GetDeviceRemovedReason();
					errorOut = fmt::format("the GPU didn't finish within 5 s{}", FAILED(removed) ? ", device removed: " + D3D12_HResultToString(removed) : std::string());
					return false;
				}
			}
			void* mapped = nullptr;
			const D3D12_RANGE readRange{ 0, 16 };
			hr = m_readback->Map(0, &readRange, &mapped);
			if (FAILED(hr))
				return _Fail(errorOut, "Map", hr);
			memcpy(valuesOut, mapped, 16);
			const D3D12_RANGE noWrite{ 0, 0 };
			m_readback->Unmap(0, &noWrite);
			return true;
		}

	private:
		static bool _Fail(std::string& errorOut, const char* what, HRESULT hr)
		{
			errorOut = fmt::format("{} failed: {}", what, D3D12_HResultToString(hr));
			return false;
		}

		ID3D12Device* m_device;
		ID3D12CommandQueue* m_queue;
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12Resource> m_target;
		ComPtr<ID3D12Resource> m_readback;
		ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
		D3D12_CPU_DESCRIPTOR_HANDLE m_rtv{};
		ComPtr<ID3D12CommandAllocator> m_allocator;
		ComPtr<ID3D12GraphicsCommandList> m_cmdList;
		ComPtr<ID3D12Fence> m_fence;
		uint64 m_fenceValue = 0;
		HANDLE m_fenceEvent = nullptr;
	};

	D3D12LocalArrayResult _RunAndLog(LocalArraySelfTest& test, const char* name, const std::vector<uint8>& vs, const std::vector<uint8>& ps)
	{
		uint32 values[4]{};
		std::string error;
		if (!test.Run(vs, ps, values, error))
		{
			cemuLog_log(LogType::Force, "D3D12: PS local-array self-test {}: not run ({})", name, error);
			return D3D12LocalArrayResult::NotTested;
		}
		const bool pass = memcmp(values, kExpected, sizeof(kExpected)) == 0;
		cemuLog_log(LogType::Force, "D3D12: PS local-array self-test {}: {} ({:x} {:x} {} {:x}, expected {:x} {:x} {} {:x})", name, pass ? "PASS" : "LOST",
			values[0], values[1], values[2], values[3], kExpected[0], kExpected[1], kExpected[2], kExpected[3]);
		return pass ? D3D12LocalArrayResult::Pass : D3D12LocalArrayResult::Lost;
	}

	D3D12LocalArrayResult _TestVariant(LocalArraySelfTest& test, const char* name, D3D12ShaderCompiler::Variant variant)
	{
		std::vector<uint8> vs, ps;
		std::string log;
		if (!D3D12ShaderCompiler::CompileHLSLVariant(variant, D3D12Const::Stage::Vertex, kVertexHLSL, vs, log) ||
			!D3D12ShaderCompiler::CompileHLSLVariant(variant, D3D12Const::Stage::Pixel, kPixelHLSL, ps, log))
		{
			cemuLog_log(LogType::Force, "D3D12: PS local-array self-test {}: not run (compiler: {})", name, log);
			return D3D12LocalArrayResult::NotTested;
		}
		return _RunAndLog(test, name, vs, ps);
	}
}

void D3D12_RunLocalArraySelfTest(ID3D12Device* device, ID3D12CommandQueue* queue)
{
	try
	{
		LocalArraySelfTest test(device, queue);
		std::string error;
		if (!test.Init(error))
		{
			cemuLog_log(LogType::Force, "D3D12: PS local-array self-test: not run ({})", error);
			return;
		}
		const D3D12LocalArrayResult dxc = _TestVariant(test, "dxc", D3D12ShaderCompiler::Variant::DXC);
		const D3D12LocalArrayResult dxcO3 = _TestVariant(test, "dxc_o3", D3D12ShaderCompiler::Variant::DXC_O3);
		const D3D12LocalArrayResult fxc = _TestVariant(test, "fxc", D3D12ShaderCompiler::Variant::FXC);

		// the real path of game shaders, with the compiler the renderer uses
		const std::string glslName = fmt::format("glsl+{}", D3D12ShaderCompiler::GetCacheName());
		D3D12LocalArrayResult glsl = D3D12LocalArrayResult::NotTested;
		std::vector<uint8> vs, ps;
		D3D12BindingRemap remap;
		std::string log;
		if (!D3D12ShaderCompiler::CompileGLSL(D3D12Const::Stage::Vertex, kVertexGLSL, vs, remap, log) ||
			!D3D12ShaderCompiler::CompileGLSL(D3D12Const::Stage::Pixel, kPixelGLSL, ps, remap, log))
			cemuLog_log(LogType::Force, "D3D12: PS local-array self-test {}: not run (compiler: {})", glslName, log);
		else
			glsl = _RunAndLog(test, glslName.c_str(), vs, ps);

		D3D12LocalArrayResult active = glsl;
		if (active == D3D12LocalArrayResult::NotTested)
		{
			if (D3D12ShaderCompiler::GetBackend() == D3D12ShaderCompiler::Backend::FXC)
				active = fxc;
			else if (D3D12ShaderCompiler::GetBackend() == D3D12ShaderCompiler::Backend::DXC)
				active = D3D12ShaderCompiler::UsesDxcO3() ? dxcO3 : dxc;
		}
		s_activeResult = active;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "D3D12: PS local-array self-test: aborted ({})", ex.what());
	}
}

D3D12LocalArrayResult D3D12_GetLocalArraySelfTestResult()
{
	return s_activeResult;
}
