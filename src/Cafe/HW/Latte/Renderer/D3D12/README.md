# Direct3D 12 backend

A D3D12 renderer for Cemu, written so it can run inside a UWP app (including Xbox in Dev Mode) as well as a normal
Win32 desktop build. It plugs into the same `Renderer` interface as the OpenGL, Vulkan and Metal backends.

**Status: first implementation, not yet run with commercial games or on a real Windows GPU.** What has been verified:

- The MSVC builds from CI (Win32 build of the UWP frontend) run Wii U homebrew end to end under Wine with vkd3d
  (D3D12 on Vulkan) and lavapipe: *MultiDRCSpaceDemo* (.rpx) and the *Homebrew App Store* (.wuhb) render correctly,
  including Cemu's own ImGui overlay and the frontend's in-game menu. This found and fixed an upside-down output pass.

- The whole shader path (decompiler-style GLSL -> HLSL -> Microsoft's `d3dcompiler_47`) with the backend's real
  translation code, root signature and binding remap tables, drawing through D3D12 (Wine's vkd3d on lavapipe) and
  checking the pixels: vertex + pixel shaders, the RECT emulation geometry shader and Latte geometry shaders, uniform
  block layout (std140 offsets), all texture types used by the decompiler, comparison samplers, base vertex handling,
  `gl_FragCoord`/`gl_FrontFacing`, render target sets with gaps. All internal shaders (surface copy, ImGui, output
  shaders) compile with FXC.
- Everything compiles against the MinGW-w64 Windows/D3D12 headers; MSVC builds run in the fork's CI.

See [Known risks](#known-risks) for what is most likely to need work.

## How it works

### Shaders

The backend does not have its own shader emitter. It consumes the Vulkan-flavored GLSL the Vulkan backend uses, so
graphic packs written for Vulkan also work on D3D12:

```
Latte shader -> Cemu decompiler (GLSL, Vulkan path) -> glslang -> SPIR-V
             -> binding remap (D3D12SpirvRemap.cpp) -> SPIRV-Cross -> HLSL 5.1 -> FXC (d3dcompiler_47) -> DXBC
```

- `D3D12ShaderTranslate.cpp`: glslang with the same settings as `RendererShaderVk` (without spirv-opt, see below),
  the binding remap and SPIRV-Cross. Platform independent, so it is tested on Linux.
- `D3D12ShaderCompiler.cpp`: compiles the HLSL with FXC (`d3dcompiler_47.dll`, part of Windows and available to UWP
  apps). Optional backends, selected with the environment variable `CEMU_D3D12_SHADER_COMPILER`: `dxc` (needs
  `dxcompiler.dll` and `dxil.dll` next to the executable) and `spirv_to_dxil` (Mesa, only when built with
  `ENABLE_D3D12_SPIRV_TO_DXIL`).
- `D3D12SpirvRemap.cpp`: rewrites every `(set, binding)` in the SPIR-V so each resource lands in a fixed register
  space for its stage and class (CBV/SRV/UAV). The binding -> register table is stored with the shader, and the
  renderer looks registers up per draw. Unused resources simply have no register.
- `RendererShaderD3D12.cpp`: async compile thread pool (same pattern as Vulkan) and a per-title bytecode cache
  (`shaderCache/precompiled/<titleId>_d3d12_<compiler>.bin`). The transferable shader cache is shared with Vulkan.

**Stage linkage.** D3D12 matches the output signature of one stage against the input signature of the next by
register, and FXC assigns registers in declaration order. Vulkan matches by location and lets the pixel shader read
inputs the previous stage never declared. Under D3D12 the decompiler therefore makes the stage before the pixel shader
declare exactly the pixel shader's inputs, in the same order (`LatteDecompilerOptions::declareAllPSInputs`, also for the
RECT emulation GS and Latte geometry shaders, whose hash includes the PS input table). spirv-opt is not run because its
dead code elimination removes unused inputs; FXC optimizes anyway.

**SPIRV-Cross limitations worked around:** arrays of input blocks in geometry shaders (the VS -> GS ring parameters are
plain variables under D3D12, `flattenV2GInterface`), `gl_in[i].gl_Position` naming, identifiers that collide with HLSL
intrinsics (renamed), and push constant arrays with an 8 byte stride (internal shaders use vec4 arrays).

### Binding model

One root signature for everything (`D3D12Renderer::CreateRootSignature`):

| Root param | Content |
|---|---|
| 0, 2, 4 | VS / PS / GS table: 17 CBVs (uniform var block + 16 uniform blocks), 18 SRVs, 1 UAV (transform feedback) |
| 1, 3, 5 | VS / PS / GS sampler table (18 samplers) |
| 6 | 16 root constants: vertex runtime data (base vertex, base instance for `gl_VertexIndex`/`gl_InstanceIndex`) |
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

Nothing beyond Cemu's normal Windows build is needed: SPIRV-Cross comes from vcpkg and `d3dcompiler_47.dll` is part of
Windows. `ENABLE_D3D12` is on by default on Windows. Select "Direct3D 12" under Graphics in the settings. Set
`CEMU_D3D12_DEBUG=1` to enable the D3D12 debug layer (needs the "Graphics Tools" optional Windows feature).

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

Ordered by how likely they are to cause problems in games:

1. **FXC compile times.** Decompiled shaders can be large and FXC is much slower than glslang. The bytecode cache helps
   from the second run on; async compilation (on by default in the UWP frontend) hides most stutter. If it is too slow,
   try `CEMU_D3D12_SHADER_COMPILER=dxc`.
2. **Feedback loops.** If a texture is sampled while bound as a render target, the render target state wins and the
   sample is undefined. Vulkan handles this with feedback-loop layouts or pass splits; D3D12 needs a copy.
3. **Depth reads while depth testing.** Sampling a depth buffer that is also bound (writes off) should use
   `DEPTH_READ | SHADER_RESOURCE` with a read-only DSV. Not implemented yet.
4. **Format reinterpretation.** D3D12 can only reinterpret views within a typeless family. Views outside the family fall
   back to the base format (logged). Texture copies between families go through a buffer.
5. **Vertex attribute alignment.** D3D12 may reject 8/16-bit attributes at unaligned offsets; the robust fix is shader
   side fetching like the Metal backend.
6. **2D views of array slices.** D3D12 `Texture2D` SRVs can't select a slice. Binding the array SRV form to a `Texture2D`
   declaration worked on Intel but the Xbox (AMD) read slice 0 (Mario Kart 8's button icons came out as fragments of
   the first font sheet). Color views of slice N>0 now sample a copy of that slice that is refreshed when the texture
   changes; depth views still use the array form.

## Not implemented yet

- Avoiding the wait for pipelines a title has never used. They are created when a draw first needs them; with "Async
  shader compile" enabled on background threads, and the draw waits at most 2 seconds before it is skipped (Intel's
  driver compiler took minutes for some Mario Kart 8 pipelines, which froze the GPU thread when they were created
  synchronously). Skipping right away is not an option: with it, the 3D scene of a Mario Kart 8 race stayed black for
  the whole race (probably a once-per-course draw such as its lighting was skipped). Pipelines that take the driver more
  than 2 seconds are logged.

  Pipeline caching, for comparison with Vulkan's pipeline cache:
  - `shaderCache/transferable/<titleId>_d3d12pipelines.bin` lists the descriptions of all pipelines the title used
    (shader hashes plus fixed function state). It doesn't depend on the GPU, so it can be copied to another device with
    the transferable shader cache. With "PrecompilePipelines" (default on) these pipelines are created while the shader
    cache loading screen is shown.
  - `shaderCache/driver/d3d12/<titleId>.bin` (ID3D12PipelineLibrary) keeps the pipelines compiled by the driver, so
    later launches load them in milliseconds. It is discarded when the driver changes.
  - On Xbox there is no driver cache: the Xbox UWP driver (SraKmd) removes the device (DXGI_ERROR_DRIVER_INTERNAL_ERROR)
    when a pipeline library is created. Precompiled pipelines are kept in memory for the session instead, so every
    launch recreates the recorded pipelines on the loading screen. Copy the transferable files from a PC to avoid
    compiling them in game.
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
| `D3D12ShaderTranslate*`, `D3D12SpirvRemap.cpp`, `D3D12BindingModel.h` | GLSL → SPIR-V → HLSL, binding model |
| `D3D12ShaderCompiler.*` | HLSL → DXBC (FXC) or DXIL (DXC, optional), optional spirv_to_dxil path |
| `D3D12RootSignature.h` | The shared root signature (header-only, also used by tests) |
| `RendererShaderD3D12.*` | Shader objects, async compile, DXIL cache |
| `LatteTextureD3D12.*`, `LatteTextureViewD3D12.*` | Textures, views, per-subresource state tracking |
| `CachedFBOD3D12.*` | Render target sets |
| `D3D12Memory.*`, `D3D12DescriptorHeaps.*` | Upload/readback rings, heaps, sampler cache |
| `D3D12SurfaceCopy.cpp` | Output blit to the swap chain, depth ↔ color copies |
| `D3D12ImGui.*` | ImGui renderer on the shared root signature |
| `D3D12Query.cpp`, `D3D12TextureReadback.*` | Occlusion queries, texture readback |
| `D3D12SwapChain.*` | HWND / CoreWindow / composition swap chains |
