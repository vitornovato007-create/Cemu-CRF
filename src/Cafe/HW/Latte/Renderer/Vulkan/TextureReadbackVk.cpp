#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanTextureReadback.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include <limits>

LatteTextureReadbackInfoVk::LatteTextureReadbackInfoVk(VkDevice device, LatteTextureView* textureView)
	: LatteTextureReadbackInfo(textureView), m_device(device)
{
	m_image_size = GetImageSize(textureView);
}

LatteTextureReadbackInfoVk::~LatteTextureReadbackInfoVk()
{
}

uint8* LatteTextureReadbackInfoVk::GetData()
{
	if (m_buffer_ptr == nullptr || m_bufferMemory == VK_NULL_HANDLE || m_transferSize == 0)
		throw std::runtime_error("Vulkan texture readback has no valid mapped buffer range");
	if ((uint64)m_buffer_offset + m_transferSize > m_bufferSize)
		throw std::runtime_error("Vulkan texture readback range exceeds the mapped buffer");

	if (m_memoryIsCoherent)
		return m_buffer_ptr + m_buffer_offset;

	const auto renderer = VulkanRenderer::GetInstance();
	const VkDeviceSize atomSize = renderer->GetNonCoherentAtomSize() ? renderer->GetNonCoherentAtomSize() : 1;
	const VkDeviceSize alignedOffset = ((VkDeviceSize)m_buffer_offset / atomSize) * atomSize;
	const VkDeviceSize requestedEnd = (VkDeviceSize)m_buffer_offset + m_transferSize;
	const VkDeviceSize alignedEnd = ((requestedEnd + atomSize - 1) / atomSize) * atomSize;

	VkMappedMemoryRange range{};
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = m_bufferMemory;
	range.offset = alignedOffset;
	range.size = alignedEnd <= m_bufferSize ? alignedEnd - alignedOffset : VK_WHOLE_SIZE;
	const VkResult invalidateResult = vkInvalidateMappedMemoryRanges(m_device, 1, &range);
	if (invalidateResult != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan texture readback cache invalidation failed: VkResult={}, offset={}, transferBytes={}, atomSize={}", (sint32)invalidateResult, m_buffer_offset, m_transferSize, atomSize);
		throw std::runtime_error(fmt::format("Vulkan texture readback invalidation failed: VkResult={}", (sint32)invalidateResult));
	}
	return m_buffer_ptr + m_buffer_offset;
}

uint32 LatteTextureReadbackInfoVk::GetImageSize(LatteTextureView* textureView)
{
	const auto* baseTexture = (LatteTextureVk*)textureView->baseTexture;
	// handle format
	const auto textureFormat = baseTexture->GetFormat();
	auto checkedLinearSize = [baseTexture](uint32 bytesPerPixel) -> uint32 {
		const uint64 pixelCount = (uint64)baseTexture->width * (uint64)baseTexture->height;
		if (pixelCount == 0 || pixelCount > std::numeric_limits<uint32>::max() / bytesPerPixel)
		{
			cemuLog_log(LogType::Force, "Texture readback size is invalid or exceeds 32-bit limits: resolution={}x{}, bytesPerPixel={}", baseTexture->width, baseTexture->height, bytesPerPixel);
			return 0;
		}
		return (uint32)(pixelCount * bytesPerPixel);
	};
	if (textureView->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R8G8B8A8_UNORM);
		return checkedLinearSize(4);
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R8_UNORM )
	{
		cemu_assert(textureFormat == VK_FORMAT_R8_UNORM);
		return checkedLinearSize(1);
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB)
	{
		cemu_assert(textureFormat == VK_FORMAT_R8G8B8A8_SRGB);
		return checkedLinearSize(4);
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R32_G32_B32_A32_FLOAT)
	{
		cemu_assert(textureFormat == VK_FORMAT_R32G32B32A32_SFLOAT);
		return checkedLinearSize(16);
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R32_FLOAT)
	{
		cemu_assert(textureFormat == VK_FORMAT_R32_SFLOAT || textureFormat == VK_FORMAT_D32_SFLOAT);
		if (baseTexture->isDepth)
			return checkedLinearSize(4);
		else
			return checkedLinearSize(4);		
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R16_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R16_UNORM);
		if (baseTexture->isDepth)
		{
			cemu_assert_debug(false);
			return checkedLinearSize(2);
		}
		else
		{
			return checkedLinearSize(2);
		}
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT)
	{
		cemu_assert(textureFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
		return checkedLinearSize(8);
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R8_G8_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R8G8_UNORM);
		return checkedLinearSize(2);
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R16G16B16A16_UNORM);
		return checkedLinearSize(8);
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::D24_S8_UNORM)
	{
		if (textureFormat == VK_FORMAT_D24_UNORM_S8_UINT)
			return checkedLinearSize(4);
		if (textureFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
			return checkedLinearSize(8);
		cemuLog_log(LogType::Force, "Texture readback depth/stencil format mismatch: GX2=D24_S8_UNORM, VkFormat={}", (sint32)textureFormat);
		return 0;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R5_G6_B5_UNORM )
	{
		if(textureFormat == VK_FORMAT_R5G6B5_UNORM_PACK16){
			return checkedLinearSize(2);
		}	
		return 0;
	}
	else
	{
		cemuLog_log(LogType::Force, "Unsupported texture readback format {:04x}; skipping readback", (uint32)textureView->format);
		return 0;
	}
}


void LatteTextureReadbackInfoVk::StartTransfer()
{
	cemu_assert(m_textureView);

	auto* baseTexture = (LatteTextureVk*)m_textureView->baseTexture;
	baseTexture->GetImageObj()->flagForCurrentCommandBuffer();

	cemu_assert_debug(m_textureView->firstSlice == 0);
	cemu_assert_debug(m_textureView->firstMip == 0);
	cemu_assert_debug(m_textureView->baseTexture->dim != Latte::E_DIM::DIM_3D);

	VkBufferImageCopy region{};
	region.bufferOffset = m_buffer_offset;
	region.bufferRowLength = baseTexture->width;
	region.bufferImageHeight = baseTexture->height;

	region.imageSubresource.aspectMask = baseTexture->GetImageAspect();
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = 0;

	region.imageOffset = {0,0,0};
	region.imageExtent = {(uint32)baseTexture->width,(uint32)baseTexture->height,1};

	const auto renderer = VulkanRenderer::GetInstance();
	renderer->draw_endRenderPass();

	renderer->barrier_image<VulkanRenderer::ANY_TRANSFER | VulkanRenderer::IMAGE_WRITE, VulkanRenderer::TRANSFER_READ>(baseTexture, region.imageSubresource, VK_IMAGE_LAYOUT_GENERAL);

	renderer->barrier_sequentializeTransfer();

	vkCmdCopyImageToBuffer(renderer->getCurrentCommandBuffer(), baseTexture->GetImageObj()->m_image, VK_IMAGE_LAYOUT_GENERAL, m_buffer, 1, &region);

	renderer->barrier_sequentializeTransfer();

	renderer->barrier_image<VulkanRenderer::TRANSFER_READ, VulkanRenderer::ANY_TRANSFER | VulkanRenderer::IMAGE_WRITE>(baseTexture, region.imageSubresource, VK_IMAGE_LAYOUT_GENERAL); // make sure transfer is finished before image is modified
	renderer->barrier_bufferRange<VulkanRenderer::TRANSFER_WRITE, VulkanRenderer::HOST_READ>(m_buffer, m_buffer_offset, m_image_size); // make sure transfer is finished before result is read

	m_associatedCommandBufferId = renderer->GetCurrentCommandBufferId();
	m_textureView = nullptr;

	// to decrease latency of readbacks make sure that the current command buffer is submitted soon
	renderer->RequestSubmitSoon();
	renderer->RequestSubmitOnIdle();
}

bool LatteTextureReadbackInfoVk::IsFinished()
{
	const auto renderer = VulkanRenderer::GetInstance();
	return renderer->HasCommandBufferFinished(m_associatedCommandBufferId);
}

void LatteTextureReadbackInfoVk::ForceFinish()
{
	const auto renderer = VulkanRenderer::GetInstance();
	renderer->WaitCommandBufferFinished(m_associatedCommandBufferId);
}

