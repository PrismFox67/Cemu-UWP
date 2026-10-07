# Direct3D 12 backend

A D3D12 renderer for Cemu, written so it can run inside a UWP app (including Xbox in Dev Mode) as well as a normal
Win32 desktop build. It plugs into the same `Renderer` interface as the OpenGL, Vulkan and Metal backends.

**Status: first implementation, not yet compiled with MSVC or run on hardware.** Everything compiles cleanly
(`-fsyntax-only -Wall -Wextra`) against the MinGW-w64 Windows/D3D12 headers, all backend classes are verified to be
non-abstract, and the SPIR-V binding remapper has been run on real Cemu shaders with the output passing `spirv-val`.
Expect a debugging phase before games render correctly. See [Known risks](#known-risks) for what is most likely to need
work.

## How it works

### Shaders

The backend does not have its own shader emitter. It consumes the exact same Vulkan-flavored GLSL the Vulkan backend
uses, so graphic packs written for Vulkan also work on D3D12:

```
Latte shader ─► Cemu decompiler (GLSL, Vulkan path) ─► glslang ─► SPIR-V
            ─► binding remap (D3D12SpirvRemap.cpp) ─► Mesa spirv_to_dxil ─► DXIL ─► signed by dxil.dll
```

- `D3D12ShaderCompiler.cpp`: glslang with the same settings as `RendererShaderVk`, then `spirv_to_dxil`, then
  signing through `IDxcValidator` from `dxil.dll` (D3D12 rejects unsigned DXIL).
- `D3D12SpirvRemap.cpp`: rewrites every `(set, binding)` in the SPIR-V so each resource lands in a fixed register
  space for its stage and class (CBV/SRV/UAV). The binding → register table is stored with the shader, and the
  renderer looks registers up per draw. Resources removed by the SPIR-V optimizer simply have no register.
- `RendererShaderD3D12.cpp`: async compile thread pool (same pattern as Vulkan) and a per-title DXIL cache
  (`shaderCache/precompiled/<titleId>_dxil.bin`). The transferable shader cache is shared with Vulkan.

### Binding model

One root signature for everything (`D3D12Renderer::CreateRootSignature`):

| Root param | Content |
|---|---|
| 0, 2, 4 | VS / PS / GS table: 17 CBVs (uniform var block + 16 uniform blocks), 18 SRVs, 1 UAV (transform feedback) |
| 1, 3, 5 | VS / PS / GS sampler table (18 samplers) |
| 6 | 16 root constants: spirv_to_dxil vertex runtime data (`first_vertex`, `base_instance`) |
| 7 | 16 root constants: push constants for internal shaders |

CBV/SRV/UAV tables are written per draw into a shader-visible ring heap. Sampler tables are deduplicated and cached
(D3D12 caps shader-visible sampler heaps at 2048 entries); when the heap fills, the renderer submits, waits and resets
it before recording any state for the next draw.

### Mapping of Latte features

| Latte / Vulkan backend | D3D12 backend |
|---|---|
| Negative viewport height | Positive viewport; D3D clip space Y already matches |
| Triangle fans | Reordered into strips by `LatteIndices` (same path Metal uses) |
| Quads, quad strips, line loops | Converted by `LatteIndices` (shared) |
| RECT primitives | Generated geometry shader (same GLSL generator as Vulkan) |
| Transform feedback | SSBO path (`UseTFViaSSBO() == true`), bound as a raw UAV |
| Dynamic depth bias | Baked into the PSO (part of the pipeline hash) |
| Custom border colors | Native `D3D12_SAMPLER_DESC::BorderColor` |
| Texture swizzles | Native `Shader4ComponentMapping` |
| Instance step rate divisors | Native `InstanceDataStepRate` (Vulkan only supports 1) |
| Logic ops | Native on UINT targets if supported; `CLEAR` emulated by blending; others ignored |
| 3x8 / 3x16 bit vertex formats | Fetched as 4 components (extra component ignored by the decoder) |
| Separate back-face stencil masks | Not available without the Agility SDK; front values used |

## Building

1. **Build `spirv_to_dxil` from Mesa** (MIT licensed). On Windows with Meson and MSVC:
   ```
   meson setup build -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms=windows -Dspirv-to-dxil=true -Dbuildtype=release
   meson compile -C build
   meson install -C build --destdir <prefix>
   ```
   The code was written against the Mesa 24.0 API. If your Mesa replaced
   `dxil_spirv_runtime_conf::zero_based_vertex_instance_id` with `first_vertex_and_base_instance_mode`, define
   `CEMU_SPIRV_TO_DXIL_SYSVAL_MODE`.
2. **Configure Cemu** with `-DENABLE_D3D12=ON -DSPIRV_TO_DXIL_ROOT=<prefix>`.
3. **Ship `dxil.dll`** next to the executable (it's in the Windows SDK `bin` folder and in DirectXShaderCompiler
   releases). UWP apps must package it; it's loaded with `LoadPackagedLibrary` there.
4. Select "Direct3D 12" under Graphics in the settings. Set `CEMU_D3D12_DEBUG=1` to enable the debug layer.

## UWP / Xbox

This backend is the rendering half of a UWP port, not the whole port. It uses only UWP-allowed APIs (no Agility SDK,
no enhanced barriers, no Win32 windowing outside the HWND swap chain path). A UWP host app still needs to:

- implement `WindowSystem` (Cemu's UI abstraction) without wxWidgets,
- set `WindowInfo::canvas_main` to `Backend::UWPCoreWindow` with the `CoreWindow` as an `IUnknown*`, or to
  `Backend::UWPSwapChainPanel` and attach `D3D12Renderer::GetSwapChain()` to its panel via
  `ISwapChainPanelNative::SetSwapChain`,
- build Cemu's other dependencies for UWP, and handle file access (`broadFileSystemAccess`) and input.

Requirements: D3D12 feature level 11_0, shader model 6.0, resource binding tier 2 (tier 3 recommended; tier 2 limits a
stage to 14 uniform blocks). Xbox One/Series hardware meets these.

## Known risks

Ordered by how likely they are to block games from rendering:

1. **VS → PS signature linkage.** Stages are translated to DXIL independently. D3D12 requires the pixel shader's input
   signature to match the vertex/geometry shader's output signature. Cemu emits matching locations on both sides, so
   this should usually line up, but if `CreateGraphicsPipelineState` reports linkage errors, the fix is to move to
   Mesa's NIR-level API and link stages with `dxil_spirv_nir_link()` at pipeline creation time.
2. **`spirv_to_dxil` details I couldn't verify without building Mesa:** vertex input semantics (`TEXCOORD<location>`),
   sampler register placement for combined image samplers (same register as the texture), and push constant
   mapping. All three are isolated in `D3D12ShaderCompiler.cpp` / the PSO input layout.
3. **Feedback loops.** If a texture is sampled while bound as a render target, the render target state wins and the
   sample is undefined. Vulkan handles this with feedback-loop layouts or pass splits; D3D12 needs a copy.
4. **Depth reads while depth testing.** Sampling a depth buffer that is also bound (writes off) should use
   `DEPTH_READ | SHADER_RESOURCE` with a read-only DSV. Not implemented yet.
5. **Format reinterpretation.** D3D12 can only reinterpret views within a typeless family. Views outside the family fall
   back to the base format (logged). Texture copies between families go through a buffer.
6. **Vertex attribute alignment.** D3D12 may reject 8/16-bit attributes at unaligned offsets; the robust fix is shader
   side fetching like the Metal backend.
7. **2D views of array slices.** D3D12 `Texture2D` SRVs can't select a slice; slice views use the array SRV form, which
   all major drivers accept but is technically a dimension mismatch.

## Not implemented yet

- Async pipeline compilation and a pipeline (PSO) cache. Pipelines are created synchronously on first use.
- Point size (D3D12 has no point sprites; points render 1 pixel wide).
- Readback of alternate formats (same limitation as Vulkan).
- Separate back-face stencil reference/masks.
- Performance work: state is re-bound for every draw (no "continued draw" fast path like Vulkan's).

## Testing plan

1. Win32 desktop with `CEMU_D3D12_DEBUG=1` and GPU-based validation, using a title that renders correctly on Vulkan.
   Fix debug layer errors first, starting with PSO creation and root signature mismatches.
2. Compare frames against the Vulkan backend with RenderDoc/PIX.
3. Only then move to a UWP host, then Xbox Dev Mode.

## File overview

| File | Purpose |
|---|---|
| `D3D12Renderer.*` | Device, queue, submission, barriers, swap chains, textures, buffers, `Renderer` interface |
| `D3D12RendererCore.cpp` | Draw path: uniforms, descriptor tables, samplers, pipeline binding, draws |
| `D3D12PipelineCache.*` | Latte register state → PSO, rect emulation GS |
| `D3D12ShaderCompiler.*`, `D3D12SpirvRemap.cpp`, `D3D12BindingModel.h` | GLSL → SPIR-V → DXIL, binding model |
| `RendererShaderD3D12.*` | Shader objects, async compile, DXIL cache |
| `LatteTextureD3D12.*`, `LatteTextureViewD3D12.*` | Textures, views, per-subresource state tracking |
| `CachedFBOD3D12.*` | Render target sets |
| `D3D12Memory.*`, `D3D12DescriptorHeaps.*` | Upload/readback rings, heaps, sampler cache |
| `D3D12SurfaceCopy.cpp` | Output blit to the swap chain, depth ↔ color copies |
| `D3D12ImGui.*` | ImGui renderer on the shared root signature |
| `D3D12Query.cpp`, `D3D12TextureReadback.*` | Occlusion queries, texture readback |
| `D3D12SwapChain.*` | HWND / CoreWindow / composition swap chains |
