#pragma once
#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"

class LatteTextureReadbackInfoVk : public LatteTextureReadbackInfo
{
public:
	LatteTextureReadbackInfoVk(VkDevice device, LatteTextureView* textureView);
	~LatteTextureReadbackInfoVk();

	static uint32 GetImageSize(LatteTextureView* textureView);

	void StartTransfer() override;

	bool IsFinished() override;
	void ForceFinish() override;

	uint8* GetData() override;

	uint32 GetImageSize() const
	{
		return m_image_size;
	}

	void SetBuffer(VkBuffer buffer, VkDeviceMemory memory, uint8* buffer_ptr, const uint32 buffer_offset, const uint32 transferSize, const uint32 bufferSize)
	{
		m_buffer = buffer;
		m_bufferMemory = memory;
		m_buffer_ptr = buffer_ptr;
		m_buffer_offset = buffer_offset;
		m_transferSize = transferSize;
		m_bufferSize = bufferSize;
	}

private:
	VkDevice m_device = nullptr;

	VkBuffer m_buffer = nullptr;
	VkDeviceMemory m_bufferMemory = VK_NULL_HANDLE;
	uint8* m_buffer_ptr = nullptr;
	uint32 m_buffer_offset = 0;
	uint32 m_transferSize = 0;
	uint32 m_bufferSize = 0;
	uint64 m_associatedCommandBufferId = 0;
};
