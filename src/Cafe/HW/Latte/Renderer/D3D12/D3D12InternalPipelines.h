#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"

// shaders used by the renderer itself (surface copies). See D3D12SurfaceCopy.cpp
struct D3D12Renderer::InternalPipelines
{
	std::unique_ptr<RendererShaderD3D12> copyVS;
	std::unique_ptr<RendererShaderD3D12> copyPSDepthToColor;
	std::unique_ptr<RendererShaderD3D12> copyPSColorToDepth;
};
