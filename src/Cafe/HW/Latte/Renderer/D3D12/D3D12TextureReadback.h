#pragma once

#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

class D3D12Renderer;

class LatteTextureReadbackInfoD3D12 : public LatteTextureReadbackInfo
{
public:
	LatteTextureReadbackInfoD3D12(D3D12Renderer* renderer, LatteTextureView* textureView);
	~LatteTextureReadbackInfoD3D12() override = default;

	uint32 GetImageSize() const { return m_image_size; }

	void StartTransfer() override;
	bool IsFinished() override;
	void ForceFinish() override;
	uint8* GetData() override;

private:
	D3D12Renderer* m_renderer;
	uint64 m_bufferOffset = 0;
	uint64 m_submissionId = 0;
};
