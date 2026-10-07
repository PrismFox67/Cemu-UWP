#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

class TextureDecoder;

struct D3D12TextureFormatInfo
{
	DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN; // typeless where possible so views can reinterpret within the family
	DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN; // UNKNOWN if the format can't be rendered to
	DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN; // depth formats only
	DXGI_FORMAT stencilSrvFormat = DXGI_FORMAT_UNKNOWN; // depth+stencil formats only
	TextureDecoder* decoder = nullptr;
	bool isDepth = false;
	bool hasStencil = false;
	bool isCompressed = false;
	bool isInteger = false; // UINT/SINT, blending must be disabled
	bool isAlternateFormat = false; // host format differs from the guest format (data needs conversion on readback)
};

struct D3D12FormatSupport
{
	bool b5g6r5 = false;
	bool b5g5r5a1 = false;
	bool b4g4r4a4 = false;
	bool d24s8 = false;
	bool logicOp = false;
};

namespace D3D12Format
{
	void InitFormatSupport(ID3D12Device* device, D3D12FormatSupport& support);

	void GetTextureFormatInfo(const D3D12FormatSupport& support, Latte::E_GX2SURFFMT format, bool isDepth, D3D12TextureFormatInfo& infoOut);

	// converts a view format (which may differ from the base texture's format) into a DXGI format that is compatible
	// with resourceFormat. Returns DXGI_FORMAT_UNKNOWN if the reinterpretation is not possible without a copy
	DXGI_FORMAT GetCompatibleViewFormat(const D3D12FormatSupport& support, DXGI_FORMAT resourceFormat, Latte::E_GX2SURFFMT viewFormat, bool isDepth, bool forRenderTarget);

	DXGI_FORMAT GetTypelessFormat(DXGI_FORMAT format);
	uint32 GetBytesPerBlock(DXGI_FORMAT format); // bytes per texel, or per 4x4 block for BC formats
	bool IsBlockCompressed(DXGI_FORMAT format);

	// vertex attribute formats. Attributes are always fetched as raw integers and decoded in the shader
	// returns a 4 component format for 3 component Latte formats since DXGI has no 3x8 or 3x16 formats
	DXGI_FORMAT GetVertexFormat(Latte::E_HWFMT format);

	D3D12_BLEND_OP GetBlendOp(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC combineFunc);
	D3D12_BLEND GetBlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor, bool isAlphaChannel);
	bool IsBlendFactorConstantAlpha(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor);
	bool IsBlendFactorConstant(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor);

	D3D12_COMPARISON_FUNC GetCompareFunc(uint32 latteCompareFunc); // 0=NEVER .. 7=ALWAYS
	D3D12_STENCIL_OP GetStencilOp(uint32 latteStencilOp);
	D3D12_TEXTURE_ADDRESS_MODE GetAddressMode(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clamp);

	// GPU7 texture swizzle (DST_SEL_X..W from SQ_TEX_RESOURCE_WORD4) to D3D12 Shader4ComponentMapping
	UINT GetComponentMapping(uint32 word4);
}
