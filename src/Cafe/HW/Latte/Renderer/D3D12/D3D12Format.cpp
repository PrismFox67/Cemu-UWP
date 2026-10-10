#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Format.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"

static bool _CheckFormat(ID3D12Device* device, DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 required)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
	fs.Format = format;
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
		return false;
	return (fs.Support1 & required) == required;
}

void D3D12Format::InitFormatSupport(ID3D12Device* device, D3D12FormatSupport& support)
{
	const auto sampleAndRender = (D3D12_FORMAT_SUPPORT1)(D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
	support.b5g6r5 = _CheckFormat(device, DXGI_FORMAT_B5G6R5_UNORM, sampleAndRender);
	support.b5g5r5a1 = _CheckFormat(device, DXGI_FORMAT_B5G5R5A1_UNORM, sampleAndRender);
	support.b4g4r4a4 = _CheckFormat(device, DXGI_FORMAT_B4G4R4A4_UNORM, sampleAndRender);
	support.d24s8 = _CheckFormat(device, DXGI_FORMAT_D24_UNORM_S8_UINT, (D3D12_FORMAT_SUPPORT1)(D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL));
	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
		support.logicOp = options.OutputMergerLogicOp != FALSE;
	cemuLog_log(LogType::Force, "D3D12: Format support: B5G6R5 {}, B5G5R5A1 {}, B4G4R4A4 {}, D24S8 {}, logic ops {}", support.b5g6r5, support.b5g5r5a1, support.b4g4r4a4, support.d24s8, support.logicOp);
}

static void _SetColor(D3D12TextureFormatInfo& info, DXGI_FORMAT typeless, DXGI_FORMAT view, TextureDecoder* decoder, bool renderable = true)
{
	info.resourceFormat = typeless;
	info.srvFormat = view;
	info.rtvFormat = renderable ? view : DXGI_FORMAT_UNKNOWN;
	info.decoder = decoder;
}

void D3D12Format::GetTextureFormatInfo(const D3D12FormatSupport& support, Latte::E_GX2SURFFMT format, bool isDepth, D3D12TextureFormatInfo& info)
{
	info = {};
	info.isDepth = isDepth;
	if (isDepth)
	{
		switch (format)
		{
		case Latte::E_GX2SURFFMT::D24_S8_UNORM:
			info.hasStencil = true;
			if (support.d24s8)
			{
				info.resourceFormat = DXGI_FORMAT_R24G8_TYPELESS;
				info.dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
				info.srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				info.stencilSrvFormat = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
				info.decoder = TextureDecoder_D24_S8::getInstance();
			}
			else
			{
				info.resourceFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
				info.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
				info.srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
				info.stencilSrvFormat = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
				info.decoder = TextureDecoder_NullData64::getInstance();
				info.isAlternateFormat = true;
			}
			break;
		case Latte::E_GX2SURFFMT::D24_S8_FLOAT:
			info.hasStencil = true;
			info.resourceFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
			info.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			info.srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			info.stencilSrvFormat = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
			info.decoder = TextureDecoder_NullData64::getInstance();
			info.isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::D32_FLOAT:
			info.resourceFormat = DXGI_FORMAT_R32_TYPELESS;
			info.dsvFormat = DXGI_FORMAT_D32_FLOAT;
			info.srvFormat = DXGI_FORMAT_R32_FLOAT;
			info.decoder = TextureDecoder_R32_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::D16_UNORM:
			info.resourceFormat = DXGI_FORMAT_R16_TYPELESS;
			info.dsvFormat = DXGI_FORMAT_D16_UNORM;
			info.srvFormat = DXGI_FORMAT_R16_UNORM;
			info.decoder = TextureDecoder_R16_UNORM::getInstance();
			break;
		case Latte::E_GX2SURFFMT::D32_S8_FLOAT:
			info.hasStencil = true;
			info.resourceFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
			info.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			info.srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			info.stencilSrvFormat = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
			info.decoder = TextureDecoder_D32_S8_UINT_X24::getInstance();
			break;
		default:
			cemuLog_log(LogType::Force, "D3D12: Unsupported depth texture format {:04x}", (uint32)format);
			info.resourceFormat = DXGI_FORMAT_R16_TYPELESS;
			info.dsvFormat = DXGI_FORMAT_D16_UNORM;
			info.srvFormat = DXGI_FORMAT_R16_UNORM;
			info.decoder = nullptr;
			info.isAlternateFormat = true;
			break;
		}
		return;
	}

	if (format == (Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT | Latte::E_GX2SURFFMT::FMT_BIT_SRGB))
		format = Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT; // same workaround as the Vulkan backend

	switch (format)
	{
	// RGBA
	case Latte::E_GX2SURFFMT::R32_G32_B32_A32_FLOAT:
		_SetColor(info, DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_R32_G32_B32_A32_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_G32_B32_A32_UINT:
		_SetColor(info, DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT, TextureDecoder_R32_G32_B32_A32_UINT::getInstance());
		info.isInteger = true;
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT:
		_SetColor(info, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, TextureDecoder_R16_G16_B16_A16_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_UINT:
		_SetColor(info, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT, TextureDecoder_R16_G16_B16_A16_UINT::getInstance());
		info.isInteger = true;
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM:
		_SetColor(info, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, TextureDecoder_R16_G16_B16_A16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_SNORM:
		_SetColor(info, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SNORM, TextureDecoder_R16_G16_B16_A16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM:
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R8_G8_B8_A8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SNORM:
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SNORM, TextureDecoder_R8_G8_B8_A8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB:
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TextureDecoder_R8_G8_B8_A8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_UINT:
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT, TextureDecoder_R8_G8_B8_A8::getInstance());
		info.isInteger = true;
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SINT:
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SINT, TextureDecoder_R8_G8_B8_A8::getInstance());
		info.isInteger = true;
		break;
	// RG
	case Latte::E_GX2SURFFMT::R32_G32_FLOAT:
		_SetColor(info, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, TextureDecoder_R32_G32_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_G32_UINT:
		_SetColor(info, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT, TextureDecoder_R32_G32_UINT::getInstance());
		info.isInteger = true;
		break;
	case Latte::E_GX2SURFFMT::R16_G16_UNORM:
		_SetColor(info, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, TextureDecoder_R16_G16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_FLOAT:
		_SetColor(info, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, TextureDecoder_R16_G16_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_UNORM:
		_SetColor(info, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, TextureDecoder_R8_G8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_SNORM:
		_SetColor(info, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SNORM, TextureDecoder_R8_G8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R4_G4_UNORM:
		// DXGI has no 4+4 bit format. Same fallback the Vulkan backend uses when R4G4/R4G4B4A4 are unsupported
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4_UNORM_To_RGBA8::getInstance());
		info.isAlternateFormat = true;
		break;
	// R
	case Latte::E_GX2SURFFMT::R32_FLOAT:
		_SetColor(info, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, TextureDecoder_R32_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_UINT:
		_SetColor(info, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT, TextureDecoder_R32_UINT::getInstance());
		info.isInteger = true;
		break;
	case Latte::E_GX2SURFFMT::R16_FLOAT:
		_SetColor(info, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT, TextureDecoder_R16_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_UNORM:
		_SetColor(info, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, TextureDecoder_R16_UNORM::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_SNORM:
		_SetColor(info, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SNORM, TextureDecoder_R16_SNORM::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_UINT:
		_SetColor(info, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, TextureDecoder_R16_UINT::getInstance());
		info.isInteger = true;
		break;
	case Latte::E_GX2SURFFMT::R8_UNORM:
		_SetColor(info, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, TextureDecoder_R8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_SNORM:
		_SetColor(info, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SNORM, TextureDecoder_R8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_UINT:
		_SetColor(info, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, TextureDecoder_R8_UINT::getInstance());
		info.isInteger = true;
		break;
	// packed formats. DXGI's B5G6R5 and B5G5R5A1 have the same bit layout as Vulkan's R5G6B5_PACK16 and A1R5G5B5_PACK16
	// so the decoders written for the Vulkan backend can be reused as-is
	case Latte::E_GX2SURFFMT::R5_G6_B5_UNORM:
		if (support.b5g6r5)
			_SetColor(info, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, TextureDecoder_R5_G6_B5_swappedRB::getInstance());
		else
			_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5G6B5_UNORM_To_RGBA8::getInstance());
		info.isAlternateFormat = true;
		break;
	case Latte::E_GX2SURFFMT::R5_G5_B5_A1_UNORM:
		if (support.b5g5r5a1)
			_SetColor(info, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB::getInstance());
		else
			_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8::getInstance());
		info.isAlternateFormat = true;
		break;
	case Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM:
		if (support.b5g5r5a1)
			_SetColor(info, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, TextureDecoder_A1_B5_G5_R5_UNORM_vulkan::getInstance());
		else
			_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8::getInstance());
		info.isAlternateFormat = true;
		break;
	case Latte::E_GX2SURFFMT::R4_G4_B4_A4_UNORM:
		// DXGI_FORMAT_B4G4R4A4_UNORM has a different channel order than Vulkan's R4G4B4A4_PACK16, so always expand to RGBA8
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4B4A4_UNORM_To_RGBA8::getInstance());
		info.isAlternateFormat = true;
		break;
	case Latte::E_GX2SURFFMT::R11_G11_B10_FLOAT:
		_SetColor(info, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, TextureDecoder_R11_G11_B10_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R10_G10_B10_A2_UNORM:
		_SetColor(info, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, TextureDecoder_R10_G10_B10_A2_UNORM::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R10_G10_B10_A2_SNORM:
		_SetColor(info, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SNORM, TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16::getInstance());
		info.isAlternateFormat = true;
		break;
	case Latte::E_GX2SURFFMT::R10_G10_B10_A2_SRGB:
		// DXGI has no 10 bit sRGB format
		_SetColor(info, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, TextureDecoder_R10_G10_B10_A2_UNORM::getInstance());
		info.isAlternateFormat = true;
		break;
	// block compressed
	case Latte::E_GX2SURFFMT::BC1_UNORM:
		_SetColor(info, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, TextureDecoder_BC1::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC1_SRGB:
		_SetColor(info, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB, TextureDecoder_BC1::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC2_UNORM:
		_SetColor(info, DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, TextureDecoder_BC2::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC2_SRGB:
		_SetColor(info, DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB, TextureDecoder_BC2::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC3_UNORM:
		_SetColor(info, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, TextureDecoder_BC3::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC3_SRGB:
		_SetColor(info, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB, TextureDecoder_BC3::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC4_UNORM:
		_SetColor(info, DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, TextureDecoder_BC4::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC4_SNORM:
		_SetColor(info, DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM, TextureDecoder_BC4::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC5_UNORM:
		_SetColor(info, DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, TextureDecoder_BC5::getInstance(), false);
		info.isCompressed = true;
		break;
	case Latte::E_GX2SURFFMT::BC5_SNORM:
		_SetColor(info, DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM, TextureDecoder_BC5::getInstance(), false);
		info.isCompressed = true;
		break;
	// depth formats sampled as color
	case Latte::E_GX2SURFFMT::R24_X8_UNORM:
		_SetColor(info, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, TextureDecoder_R24_X8::getInstance());
		info.isAlternateFormat = true;
		break;
	case Latte::E_GX2SURFFMT::X24_G8_UINT:
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT, TextureDecoder_X24_G8_UINT::getInstance());
		info.isInteger = true;
		info.isAlternateFormat = true;
		break;
	case Latte::E_GX2SURFFMT::R32_X8_FLOAT:
		_SetColor(info, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, TextureDecoder_NullData64::getInstance());
		info.isAlternateFormat = true;
		break;
	default:
		cemuLog_log(LogType::Force, "D3D12: Unsupported color texture format {:04x}", (uint32)format);
		_SetColor(info, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr);
		info.isAlternateFormat = true;
		break;
	}
}

DXGI_FORMAT D3D12Format::GetTypelessFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
		return DXGI_FORMAT_R32G32B32A32_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
		return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
		return DXGI_FORMAT_R32G32_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
		return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
		return DXGI_FORMAT_R16G16_TYPELESS;
	case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
		return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
		return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		return DXGI_FORMAT_R32G8X24_TYPELESS;
	case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT:
		return DXGI_FORMAT_R8G8_TYPELESS;
	case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
		return DXGI_FORMAT_R16_TYPELESS;
	case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT:
		return DXGI_FORMAT_R8_TYPELESS;
	case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
		return DXGI_FORMAT_BC1_TYPELESS;
	case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
		return DXGI_FORMAT_BC2_TYPELESS;
	case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
		return DXGI_FORMAT_BC3_TYPELESS;
	case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
		return DXGI_FORMAT_BC4_TYPELESS;
	case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
		return DXGI_FORMAT_BC5_TYPELESS;
	default:
		return format; // formats without a typeless family (R11G11B10, B5G6R5, ...)
	}
}

uint32 D3D12Format::GetBytesPerBlock(DXGI_FORMAT format)
{
	switch (GetTypelessFormat(format))
	{
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 8;
	case DXGI_FORMAT_R32G32_TYPELESS: return 8;
	case DXGI_FORMAT_R32G8X24_TYPELESS: return 8;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 4;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return 4;
	case DXGI_FORMAT_R16G16_TYPELESS: return 4;
	case DXGI_FORMAT_R32_TYPELESS: return 4;
	case DXGI_FORMAT_R24G8_TYPELESS: return 4;
	case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
	case DXGI_FORMAT_R8G8_TYPELESS: return 2;
	case DXGI_FORMAT_R16_TYPELESS: return 2;
	case DXGI_FORMAT_B5G6R5_UNORM: return 2;
	case DXGI_FORMAT_B5G5R5A1_UNORM: return 2;
	case DXGI_FORMAT_B4G4R4A4_UNORM: return 2;
	case DXGI_FORMAT_R8_TYPELESS: return 1;
	case DXGI_FORMAT_BC1_TYPELESS: return 8;
	case DXGI_FORMAT_BC4_TYPELESS: return 8;
	case DXGI_FORMAT_BC2_TYPELESS: return 16;
	case DXGI_FORMAT_BC3_TYPELESS: return 16;
	case DXGI_FORMAT_BC5_TYPELESS: return 16;
	default:
		cemu_assert_debug(false);
		return 4;
	}
}

bool D3D12Format::IsBlockCompressed(DXGI_FORMAT format)
{
	switch (GetTypelessFormat(format))
	{
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC5_TYPELESS:
		return true;
	default:
		return false;
	}
}

DXGI_FORMAT D3D12Format::GetCompatibleViewFormat(const D3D12FormatSupport& support, DXGI_FORMAT resourceFormat, Latte::E_GX2SURFFMT viewFormat, bool isDepth, bool forRenderTarget)
{
	D3D12TextureFormatInfo viewInfo;
	GetTextureFormatInfo(support, viewFormat, isDepth, viewInfo);
	DXGI_FORMAT candidate = forRenderTarget ? (isDepth ? viewInfo.dsvFormat : viewInfo.rtvFormat) : viewInfo.srvFormat;
	if (candidate == DXGI_FORMAT_UNKNOWN)
		return DXGI_FORMAT_UNKNOWN;
	if (GetTypelessFormat(candidate) == GetTypelessFormat(resourceFormat))
		return candidate;
	return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT D3D12Format::GetVertexFormat(Latte::E_HWFMT format)
{
	// attributes are fetched as raw unsigned integers and converted by the attribute decoder in the shader,
	// identical to how the Vulkan backend handles them (see PipelineCompiler::GetVertexFormat)
	switch (format)
	{
	case Latte::E_HWFMT::HWFMT_32_32_32_32_FLOAT:
	case Latte::E_HWFMT::HWFMT_32_32_32_32:
		return DXGI_FORMAT_R32G32B32A32_UINT;
	case Latte::E_HWFMT::HWFMT_32_32_32_FLOAT:
	case Latte::E_HWFMT::HWFMT_32_32_32:
		return DXGI_FORMAT_R32G32B32_UINT;
	case Latte::E_HWFMT::HWFMT_32_32_FLOAT:
	case Latte::E_HWFMT::HWFMT_32_32:
		return DXGI_FORMAT_R32G32_UINT;
	case Latte::E_HWFMT::HWFMT_32_FLOAT:
	case Latte::E_HWFMT::HWFMT_32:
	case Latte::E_HWFMT::HWFMT_2_10_10_10:
		return DXGI_FORMAT_R32_UINT;
	case Latte::E_HWFMT::HWFMT_8_8_8_8:
		return DXGI_FORMAT_R8G8B8A8_UINT;
	case Latte::E_HWFMT::HWFMT_8_8_8:
		// no 3x8 format in DXGI. Fetch 4 bytes, the 4th component is ignored by the attribute decoder
		// note: this reads one byte past the attribute. Out of range vertex fetches return 0 in D3D12
		return DXGI_FORMAT_R8G8B8A8_UINT;
	case Latte::E_HWFMT::HWFMT_8_8:
		return DXGI_FORMAT_R8G8_UINT;
	case Latte::E_HWFMT::HWFMT_8:
		return DXGI_FORMAT_R8_UINT;
	case Latte::E_HWFMT::HWFMT_16_16_16_16:
	case Latte::E_HWFMT::HWFMT_16_16_16_16_FLOAT:
		return DXGI_FORMAT_R16G16B16A16_UINT;
	case Latte::E_HWFMT::HWFMT_16_16_16:
	case Latte::E_HWFMT::HWFMT_16_16_16_FLOAT:
		// no 3x16 format in DXGI, see HWFMT_8_8_8
		return DXGI_FORMAT_R16G16B16A16_UINT;
	case Latte::E_HWFMT::HWFMT_16_16:
	case Latte::E_HWFMT::HWFMT_16_16_FLOAT:
		return DXGI_FORMAT_R16G16_UINT;
	case Latte::E_HWFMT::HWFMT_16:
	case Latte::E_HWFMT::HWFMT_16_FLOAT:
		return DXGI_FORMAT_R16_UINT;
	default:
		cemuLog_log(LogType::Force, "D3D12: Unsupported vertex format: {:02x}", (uint32)format);
		cemu_assert_debug(false);
		return DXGI_FORMAT_R32_UINT;
	}
}

D3D12_BLEND_OP D3D12Format::GetBlendOp(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC combineFunc)
{
	switch (combineFunc)
	{
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::DST_PLUS_SRC:
		return D3D12_BLEND_OP_ADD;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::SRC_MINUS_DST:
		return D3D12_BLEND_OP_SUBTRACT;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::MIN_DST_SRC:
		return D3D12_BLEND_OP_MIN;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::MAX_DST_SRC:
		return D3D12_BLEND_OP_MAX;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::DST_MINUS_SRC:
		return D3D12_BLEND_OP_REV_SUBTRACT;
	default:
		cemu_assert_suspicious();
		return D3D12_BLEND_OP_ADD;
	}
}

D3D12_BLEND D3D12Format::GetBlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor, bool isAlphaChannel)
{
	// D3D12 does not allow *_COLOR factors for the alpha channel, use the matching *_ALPHA factor instead (equivalent for alpha)
	// D3D12 (without the Agility SDK) has no separate constant-alpha factor. CONSTANT_ALPHA is mapped to BLEND_FACTOR and
	// the renderer replicates the alpha constant into RGB when only constant-alpha factors are used (see D3D12PipelineCache)
	static const D3D12_BLEND colorFactors[] =
	{
		/* 0x00 */ D3D12_BLEND_ZERO,
		/* 0x01 */ D3D12_BLEND_ONE,
		/* 0x02 */ D3D12_BLEND_SRC_COLOR,
		/* 0x03 */ D3D12_BLEND_INV_SRC_COLOR,
		/* 0x04 */ D3D12_BLEND_SRC_ALPHA,
		/* 0x05 */ D3D12_BLEND_INV_SRC_ALPHA,
		/* 0x06 */ D3D12_BLEND_DEST_ALPHA,
		/* 0x07 */ D3D12_BLEND_INV_DEST_ALPHA,
		/* 0x08 */ D3D12_BLEND_DEST_COLOR,
		/* 0x09 */ D3D12_BLEND_INV_DEST_COLOR,
		/* 0x0A */ D3D12_BLEND_SRC_ALPHA_SAT,
		/* 0x0B */ D3D12_BLEND_ONE, // todo (unknown on Vulkan too)
		/* 0x0C */ D3D12_BLEND_ONE, // todo
		/* 0x0D */ D3D12_BLEND_BLEND_FACTOR,
		/* 0x0E */ D3D12_BLEND_INV_BLEND_FACTOR,
		/* 0x0F */ D3D12_BLEND_SRC1_COLOR,
		/* 0x10 */ D3D12_BLEND_INV_SRC1_COLOR,
		/* 0x11 */ D3D12_BLEND_SRC1_ALPHA,
		/* 0x12 */ D3D12_BLEND_INV_SRC1_ALPHA,
		/* 0x13 */ D3D12_BLEND_BLEND_FACTOR,
		/* 0x14 */ D3D12_BLEND_INV_BLEND_FACTOR,
	};
	uint32 idx = (uint32)factor;
	if (idx >= std::size(colorFactors))
	{
		cemu_assert_suspicious();
		return D3D12_BLEND_ONE;
	}
	D3D12_BLEND b = colorFactors[idx];
	if (isAlphaChannel)
	{
		switch (b)
		{
		case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
		case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
		case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
		case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
		case D3D12_BLEND_SRC1_COLOR: return D3D12_BLEND_SRC1_ALPHA;
		case D3D12_BLEND_INV_SRC1_COLOR: return D3D12_BLEND_INV_SRC1_ALPHA;
		default: break;
		}
	}
	return b;
}

bool D3D12Format::IsBlendFactorConstantAlpha(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor)
{
	return (uint32)factor == 0x13 || (uint32)factor == 0x14;
}

bool D3D12Format::IsBlendFactorConstant(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor)
{
	uint32 f = (uint32)factor;
	return f == 0x0D || f == 0x0E || f == 0x13 || f == 0x14;
}

D3D12_COMPARISON_FUNC D3D12Format::GetCompareFunc(uint32 latteCompareFunc)
{
	static const D3D12_COMPARISON_FUNC table[8] =
	{
		D3D12_COMPARISON_FUNC_NEVER,
		D3D12_COMPARISON_FUNC_LESS,
		D3D12_COMPARISON_FUNC_EQUAL,
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_COMPARISON_FUNC_GREATER,
		D3D12_COMPARISON_FUNC_NOT_EQUAL,
		D3D12_COMPARISON_FUNC_GREATER_EQUAL,
		D3D12_COMPARISON_FUNC_ALWAYS,
	};
	return table[latteCompareFunc & 7];
}

D3D12_STENCIL_OP D3D12Format::GetStencilOp(uint32 latteStencilOp)
{
	static const D3D12_STENCIL_OP table[8] =
	{
		D3D12_STENCIL_OP_KEEP,
		D3D12_STENCIL_OP_ZERO,
		D3D12_STENCIL_OP_REPLACE,
		D3D12_STENCIL_OP_INCR_SAT,
		D3D12_STENCIL_OP_DECR_SAT,
		D3D12_STENCIL_OP_INVERT,
		D3D12_STENCIL_OP_INCR,
		D3D12_STENCIL_OP_DECR,
	};
	return table[latteStencilOp & 7];
}

D3D12_TEXTURE_ADDRESS_MODE D3D12Format::GetAddressMode(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clamp)
{
	static const D3D12_TEXTURE_ADDRESS_MODE table[8] =
	{
		D3D12_TEXTURE_ADDRESS_MODE_WRAP, // WRAP
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR, // MIRROR
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // CLAMP_LAST_TEXEL
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, // MIRROR_ONCE_LAST_TEXEL
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // CLAMP_HALF_BORDER (unsupported)
		D3D12_TEXTURE_ADDRESS_MODE_BORDER, // MIRROR_ONCE_HALF_BORDER (unsupported)
		D3D12_TEXTURE_ADDRESS_MODE_BORDER, // CLAMP_BORDER
		D3D12_TEXTURE_ADDRESS_MODE_BORDER, // MIRROR_ONCE_BORDER
	};
	return table[(uint32)clamp & 7];
}

UINT D3D12Format::GetComponentMapping(uint32 word4)
{
	// SQ_TEX_RESOURCE_WORD4 DST_SEL: 0-3 = RGBA, 4 = 0, 5 = 1, 6/7 = 0
	static const D3D12_SHADER_COMPONENT_MAPPING table[8] =
	{
		D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
		D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
		D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
		D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3,
		D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
		D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
		D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
		D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
	};
	uint32 r = (word4 >> 16) & 7;
	uint32 g = (word4 >> 19) & 7;
	uint32 b = (word4 >> 22) & 7;
	uint32 a = (word4 >> 25) & 7;
	return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(table[r], table[g], table[b], table[a]);
}
