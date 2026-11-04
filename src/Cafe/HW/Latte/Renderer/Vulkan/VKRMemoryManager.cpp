#include "Cafe/HW/Latte/Renderer/Vulkan/VKRMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include <imgui.h>
#include <atomic>
#include <limits>

namespace
{
	std::atomic<uint64> s_vkBufferCreateAttempts{ 0 };
	std::atomic<uint64> s_vkBufferCreateSuccesses{ 0 };
	std::atomic<uint64> s_vkBufferCreateFailures{ 0 };
	std::atomic<uint64> s_vkBufferBytesRequested{ 0 };
	std::atomic<uint64> s_vkBufferBytesAllocated{ 0 };
	std::atomic<uint64> s_vkTextureHeapReservedBytes{ 0 };
	std::atomic<uint64> s_vkTextureHeapChunks{ 0 };
	std::atomic<uint64> s_vkTextureBytesCurrentlyAllocated{ 0 };
	std::atomic<uint64> s_vkTextureAllocationsCurrentlyLive{ 0 };
	std::atomic<uint64> s_vkImageAllocationAttempts{ 0 };
	std::atomic<uint64> s_vkImageAllocationSuccesses{ 0 };
	std::atomic<uint64> s_vkImageAllocationFailures{ 0 };
	std::atomic<uint64> s_vkImageAllocationBytes{ 0 };
}

/* VKRSynchronizedMemoryBuffer */

VKRSynchronizedRingAllocator::~VKRSynchronizedRingAllocator()
{
	for (auto& buf : m_buffers)
	{
		if (buf.basePtr != nullptr && buf.vk_mem != VK_NULL_HANDLE)
			vkUnmapMemory(m_vkr->GetLogicalDevice(), buf.vk_mem);
		m_vkrMemMgr->DeleteBuffer(buf.vk_buffer, buf.vk_mem);
	}
}

void VKRSynchronizedRingAllocator::addUploadBufferSyncPoint(AllocatorBuffer_t& buffer, uint32 offset)
{
	auto cmdBufferId = m_vkr->GetCurrentCommandBufferId();
	if (cmdBufferId == buffer.lastSyncpointCmdBufferId)
		return;
	buffer.lastSyncpointCmdBufferId = cmdBufferId;
	buffer.queue_syncPoints.emplace(cmdBufferId, offset);
}

void VKRSynchronizedRingAllocator::allocateAdditionalUploadBuffer(uint32 sizeRequiredForAlloc)
{
	// calculate buffer size, should be a multiple of bufferAllocSize that is at least as large as sizeRequiredForAlloc
	uint32 bufferAllocSize = m_minimumBufferAllocSize;
	while (bufferAllocSize < sizeRequiredForAlloc)
		bufferAllocSize += m_minimumBufferAllocSize;

	AllocatorBuffer_t newBuffer{};
	newBuffer.writeIndex = 0;
	newBuffer.basePtr = nullptr;
	bool created = false;
	if (m_bufferType == VKR_BUFFER_TYPE::STAGING)
		created = m_vkrMemMgr->CreateBuffer(bufferAllocSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, newBuffer.vk_buffer, newBuffer.vk_mem);
	else if (m_bufferType == VKR_BUFFER_TYPE::INDEX)
		created = m_vkrMemMgr->CreateBuffer(bufferAllocSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, newBuffer.vk_buffer, newBuffer.vk_mem);
	else if (m_bufferType == VKR_BUFFER_TYPE::STRIDE)
		created = m_vkrMemMgr->CreateBuffer(bufferAllocSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, newBuffer.vk_buffer, newBuffer.vk_mem);
	else
		cemu_assert_debug(false);

	if (!created)
	{
		cemuLog_log(LogType::Force, "Vulkan: failed to create synchronized upload buffer (type {}, size {} bytes)",
			(sint32)m_bufferType, bufferAllocSize);
		throw std::runtime_error("Failed to create synchronized Vulkan upload buffer");
	}

	void* bufferPtr = nullptr;
	VkResult mapResult = vkMapMemory(m_vkr->GetLogicalDevice(), newBuffer.vk_mem, 0, VK_WHOLE_SIZE, 0, &bufferPtr);
	if (mapResult != VK_SUCCESS || bufferPtr == nullptr)
	{
		cemuLog_log(LogType::Force, "Vulkan: vkMapMemory failed for synchronized upload buffer (VkResult {}, type {}, size {} bytes)",
			(sint32)mapResult, (sint32)m_bufferType, bufferAllocSize);
		m_vkrMemMgr->DeleteBuffer(newBuffer.vk_buffer, newBuffer.vk_mem);
		throw std::runtime_error("Failed to map synchronized Vulkan upload buffer");
	}

	newBuffer.basePtr = (uint8*)bufferPtr;
	newBuffer.size = bufferAllocSize;
	newBuffer.index = (uint32)m_buffers.size();
	m_buffers.push_back(newBuffer);
}

VKRSynchronizedRingAllocator::AllocatorReservation_t VKRSynchronizedRingAllocator::AllocateBufferMemory(uint32 size, uint32 alignment)
{
	const uint32 atomSize = std::max<uint32>(m_vkr->GetNonCoherentAtomSize(), 128);
	if (alignment < atomSize)
		alignment = atomSize;
	const uint64 alignedSize = ((uint64)size + atomSize - 1) / atomSize * atomSize;
	if (alignedSize > std::numeric_limits<uint32>::max())
		throw std::runtime_error("Vulkan upload reservation size exceeds the allocator limit");
	size = (uint32)alignedSize;

	for (auto& itr : m_buffers)
	{
		// align pointer
		uint32 alignmentPadding = (alignment - (itr.writeIndex % alignment)) % alignment;
		uint32 distanceToSyncPoint;
		if (!itr.queue_syncPoints.empty())
		{
			if (itr.queue_syncPoints.front().offset < itr.writeIndex)
				distanceToSyncPoint = 0xFFFFFFFF;
			else
				distanceToSyncPoint = itr.queue_syncPoints.front().offset - itr.writeIndex;
		}
		else
			distanceToSyncPoint = 0xFFFFFFFF;
		uint32 spaceNeeded = alignmentPadding + size;
		if (spaceNeeded > distanceToSyncPoint)
			continue; // not enough space in current buffer
		if ((itr.writeIndex + spaceNeeded) > itr.size)
		{
			// wrap-around
			spaceNeeded = size;
			alignmentPadding = 0;
			// check if there is enough space in current buffer after wrap-around
			if (!itr.queue_syncPoints.empty())
			{
				distanceToSyncPoint = itr.queue_syncPoints.front().offset - 0;
				if (spaceNeeded > distanceToSyncPoint)
					continue;
			}
			else if (spaceNeeded > itr.size)
				continue;
			itr.writeIndex = 0;
		}
		addUploadBufferSyncPoint(itr, itr.writeIndex);
		itr.writeIndex += alignmentPadding;
		uint32 offset = itr.writeIndex;
		itr.writeIndex += size;
		itr.cleanupCounter = 0;
		VKRSynchronizedRingAllocator::AllocatorReservation_t res;
		res.vkBuffer = itr.vk_buffer;
		res.vkMem = itr.vk_mem;
		res.memPtr = itr.basePtr + offset;
		res.bufferOffset = offset;
		res.size = size;
		res.bufferIndex = itr.index;
		return res;
	}
	// allocate new buffer
	allocateAdditionalUploadBuffer(size);
	return AllocateBufferMemory(size, alignment);
}

void VKRSynchronizedRingAllocator::FlushReservation(AllocatorReservation_t& uploadReservation)
{
	cemu_assert_debug(m_bufferType == VKR_BUFFER_TYPE::STAGING); // only the staging buffer isn't coherent
	// Reservations are padded and aligned to nonCoherentAtomSize before reaching this flush.
	VkMappedMemoryRange flushedRange{};
	flushedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	flushedRange.memory = uploadReservation.vkMem;
	flushedRange.offset = uploadReservation.bufferOffset;
	flushedRange.size = uploadReservation.size;
	const VkResult flushResult = vkFlushMappedMemoryRanges(m_vkr->GetLogicalDevice(), 1, &flushedRange);
	if (flushResult != VK_SUCCESS)
		cemuLog_log(LogType::Force, "Vulkan staging upload flush failed: VkResult={}, offset={}, bytes={}", (sint32)flushResult, uploadReservation.bufferOffset, uploadReservation.size);
}

void VKRSynchronizedRingAllocator::CleanupBuffer(uint64 latestFinishedCommandBufferId)
{
	if (latestFinishedCommandBufferId > 1)
		latestFinishedCommandBufferId -= 1;

	for (auto& itr : m_buffers)
	{
		while (!itr.queue_syncPoints.empty() && latestFinishedCommandBufferId > itr.queue_syncPoints.front().commandBufferId)
		{
			itr.queue_syncPoints.pop();
		}
		if (itr.queue_syncPoints.empty())
			itr.cleanupCounter++;
	}

	// check if last buffer is available for deletion
	if (m_buffers.size() >= 2)
	{
		auto& lastBuffer = m_buffers.back();
		if (lastBuffer.cleanupCounter >= 1000)
		{
			// release buffer
			vkUnmapMemory(m_vkr->GetLogicalDevice(), lastBuffer.vk_mem);
			m_vkrMemMgr->DeleteBuffer(lastBuffer.vk_buffer, lastBuffer.vk_mem);
			m_buffers.pop_back();
		}
	}
}

VkBuffer VKRSynchronizedRingAllocator::GetBufferByIndex(uint32 index) const
{
	return m_buffers[index].vk_buffer;
}

void VKRSynchronizedRingAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	numBuffers = (uint32)m_buffers.size();
	totalBufferSize = 0;
	freeBufferSize = 0;
	for (auto& itr : m_buffers)
	{
		totalBufferSize += itr.size;
		// calculate free space in buffer
		uint32 distanceToSyncPoint;
		if (!itr.queue_syncPoints.empty())
		{
			if (itr.queue_syncPoints.front().offset < itr.writeIndex)
				distanceToSyncPoint = (itr.size - itr.writeIndex) + itr.queue_syncPoints.front().offset; // size with wrap-around
			else
				distanceToSyncPoint = itr.queue_syncPoints.front().offset - itr.writeIndex;
		}
		else
			distanceToSyncPoint = itr.size;
		freeBufferSize += distanceToSyncPoint;
	}
}

/* VKRSynchronizedHeapAllocator */

VKRSynchronizedHeapAllocator::VKRSynchronizedHeapAllocator(class VKRMemoryManager* vkMemoryManager, VKR_BUFFER_TYPE bufferType, size_t minimumBufferAllocSize)
	: m_vkrMemMgr(vkMemoryManager), m_chunkedHeap(bufferType, minimumBufferAllocSize) {};

VKRSynchronizedHeapAllocator::AllocatorReservation* VKRSynchronizedHeapAllocator::AllocateBufferMemory(uint32 size, uint32 alignment)
{
	const uint32 atomSize = m_vkrMemMgr->GetNonCoherentAtomSize();
	if (alignment < atomSize)
		alignment = atomSize;
	const uint64 alignedSize = ((uint64)size + atomSize - 1) / atomSize * atomSize;
	if (alignedSize > std::numeric_limits<uint32>::max())
		throw std::runtime_error("Vulkan heap reservation size exceeds the allocator limit");
	size = (uint32)alignedSize;
	CHAddr addr = m_chunkedHeap.alloc(size, alignment);
	m_activeAllocations.emplace_back(addr);
	AllocatorReservation* res = m_poolAllocatorReservation.allocObj();
	res->bufferIndex = addr.chunkIndex;
	res->bufferOffset = addr.offset;
	res->size = size;
	res->memPtr = m_chunkedHeap.GetChunkPtr(addr.chunkIndex) + addr.offset;
	m_chunkedHeap.GetChunkVkMemInfo(addr.chunkIndex, res->vkBuffer, res->vkMem);
	return res;
}

void VKRSynchronizedHeapAllocator::FreeReservation(AllocatorReservation* uploadReservation)
{
	// put the allocation on a delayed release queue for the current command buffer
	uint64 currentCommandBufferId = VulkanRenderer::GetInstance()->GetCurrentCommandBufferId();
	auto it = std::find_if(m_activeAllocations.begin(), m_activeAllocations.end(), [&uploadReservation](const TrackedAllocation& allocation) { return allocation.allocation.chunkIndex == uploadReservation->bufferIndex && allocation.allocation.offset == uploadReservation->bufferOffset; });
	cemu_assert_debug(it != m_activeAllocations.end());
	m_releaseQueue[currentCommandBufferId].emplace_back(it->allocation);
	m_activeAllocations.erase(it);
	m_poolAllocatorReservation.freeObj(uploadReservation);
}

void VKRSynchronizedHeapAllocator::FlushReservation(AllocatorReservation* uploadReservation)
{
	if (m_chunkedHeap.RequiresFlush(uploadReservation->bufferIndex))
	{
		VkMappedMemoryRange flushedRange{};
		flushedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		flushedRange.memory = uploadReservation->vkMem;
		flushedRange.offset = uploadReservation->bufferOffset;
		flushedRange.size = uploadReservation->size;
		const VkResult flushResult = vkFlushMappedMemoryRanges(VulkanRenderer::GetInstance()->GetLogicalDevice(), 1, &flushedRange);
		if (flushResult != VK_SUCCESS)
			cemuLog_log(LogType::Force, "Vulkan index/stride upload flush failed: VkResult={}, bufferIndex={}, offset={}, bytes={}", (sint32)flushResult, uploadReservation->bufferIndex, uploadReservation->bufferOffset, uploadReservation->size);
	}
}

void VKRSynchronizedHeapAllocator::CleanupBuffer(uint64 latestFinishedCommandBufferId)
{
	auto it = m_releaseQueue.begin();
	while (it != m_releaseQueue.end())
	{
		if (it->first <= latestFinishedCommandBufferId)
		{
			// release allocations
			for(auto& addr : it->second)
				m_chunkedHeap.free(addr);
			it = m_releaseQueue.erase(it);
			continue;
		}
		it++;
	}
}

void VKRSynchronizedHeapAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	m_chunkedHeap.GetStats(numBuffers, totalBufferSize, freeBufferSize);
}

/* VkTextureChunkedHeap */

VkTextureChunkedHeap::~VkTextureChunkedHeap()
{
	VkDevice device = VulkanRenderer::GetInstance()->GetLogicalDevice();
	s_vkTextureHeapReservedBytes.fetch_sub(m_numHeapBytes, std::memory_order_relaxed);
	for (auto& i : m_list_chunkInfo)
	{
		if (i.mem != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, i.mem, nullptr);
			s_vkTextureHeapChunks.fetch_sub(1, std::memory_order_relaxed);
		}
	}
}

uint32 VkTextureChunkedHeap::allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize)
{
	cemu_assert_debug(m_list_chunkInfo.size() == chunkIndex);
	m_list_chunkInfo.resize(m_list_chunkInfo.size() + 1);

	// pad minimumAllocationSize to 32KB alignment
	minimumAllocationSize = (minimumAllocationSize + (32 * 1024 - 1)) & ~(32 * 1024 - 1);

	uint32 allocationSize = 1024 * 1024 * 128;
	if (chunkIndex == 0)
	{
		// make the first allocation smaller, this decreases wasted memory when there are textures that require specific flags (and thus separate heaps)
		allocationSize = 1024 * 1024 * 16;
	}
	if (allocationSize < minimumAllocationSize)
		allocationSize = minimumAllocationSize;
	// get available memory types/heaps
	std::vector<uint32> deviceLocalMemoryTypeIndices = m_vkrMemoryManager->FindMemoryTypes(m_typeFilter, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	std::vector<uint32> hostLocalMemoryTypeIndices = m_vkrMemoryManager->FindMemoryTypes(m_typeFilter, 0);
	// remove device local memory types from host local vector
	auto pred = [&deviceLocalMemoryTypeIndices](const uint32& v) -> bool {
		return std::find(deviceLocalMemoryTypeIndices.begin(), deviceLocalMemoryTypeIndices.end(), v) != deviceLocalMemoryTypeIndices.end();
	};
	hostLocalMemoryTypeIndices.erase(std::remove_if(hostLocalMemoryTypeIndices.begin(), hostLocalMemoryTypeIndices.end(), pred), hostLocalMemoryTypeIndices.end());
	// allocate chunk memory
	VkResult lastAllocationResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	for (sint32 t = 0; t < 3; t++)
	{
		// attempt to allocate from device local memory first
		for (auto memType : deviceLocalMemoryTypeIndices)
		{
			VkMemoryAllocateInfo allocInfo{};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocInfo.allocationSize = allocationSize;
			allocInfo.memoryTypeIndex = memType;

			VkDeviceMemory imageMemory;
			VkResult r = vkAllocateMemory(VulkanRenderer::GetInstance()->GetLogicalDevice(), &allocInfo, nullptr, &imageMemory);
			lastAllocationResult = r;
			if (r != VK_SUCCESS)
				continue;
			m_list_chunkInfo[chunkIndex].mem = imageMemory;
			s_vkTextureHeapReservedBytes.fetch_add(allocationSize, std::memory_order_relaxed);
			s_vkTextureHeapChunks.fetch_add(1, std::memory_order_relaxed);
			return allocationSize;
		}
		// attempt to allocate from host-local memory
		for (auto memType : hostLocalMemoryTypeIndices)
		{
			VkMemoryAllocateInfo allocInfo{};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocInfo.allocationSize = allocationSize;
			allocInfo.memoryTypeIndex = memType;

			VkDeviceMemory imageMemory;
			VkResult r = vkAllocateMemory(VulkanRenderer::GetInstance()->GetLogicalDevice(), &allocInfo, nullptr, &imageMemory);
			lastAllocationResult = r;
			if (r != VK_SUCCESS)
				continue;
			m_list_chunkInfo[chunkIndex].mem = imageMemory;
			s_vkTextureHeapReservedBytes.fetch_add(allocationSize, std::memory_order_relaxed);
			s_vkTextureHeapChunks.fetch_add(1, std::memory_order_relaxed);
			return allocationSize;
		}
		// retry with smaller size if possible
		allocationSize /= 2;
		if (allocationSize < minimumAllocationSize)
			break;
		cemuLog_log(LogType::Force, "Failed to allocate texture memory chunk with size {}MB. Trying again with smaller allocation size", allocationSize / 1024 / 1024);
	}
	s_vkImageAllocationFailures.fetch_add(1, std::memory_order_relaxed);
	cemuLog_log(LogType::Force, "Unable to allocate Vulkan texture memory chunk: VkResult={}, requested={} bytes, device-local memory types={}, host-local memory types={}", (sint32)lastAllocationResult, allocationSize, deviceLocalMemoryTypeIndices.size(), hostLocalMemoryTypeIndices.size());
	throw std::runtime_error(fmt::format("failed to allocate image memory: VkResult={}", (sint32)lastAllocationResult));
	return 0;
}

/* VkBufferChunkedHeap */

VKRBuffer* VKRBuffer::Create(VKR_BUFFER_TYPE bufferType, size_t bufferSize, VkMemoryPropertyFlags properties)
{
	auto* memMgr = VulkanRenderer::GetInstance()->GetMemoryManager();
	VkBuffer buffer;
	VkDeviceMemory bufferMemory;
	bool allocSuccess;
	if (bufferType == VKR_BUFFER_TYPE::STAGING)
		allocSuccess = memMgr->CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, properties, buffer, bufferMemory);
	else if (bufferType == VKR_BUFFER_TYPE::INDEX)
		allocSuccess = memMgr->CreateBuffer(bufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, properties, buffer, bufferMemory);
	else if (bufferType == VKR_BUFFER_TYPE::STRIDE)
		allocSuccess = memMgr->CreateBuffer(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, properties, buffer, bufferMemory);
	else
		cemu_assert_debug(false);
	if (!allocSuccess)
		return nullptr;

	VKRBuffer* bufferObj = new VKRBuffer(buffer, bufferMemory);
	bufferObj->m_mappedMemory = nullptr;
	// If host visible, map the allocation and reject the buffer if mapping fails.
	if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	{
		void* data = nullptr;
		const VkResult mapResult = vkMapMemory(VulkanRenderer::GetInstance()->GetLogicalDevice(), bufferMemory, 0, bufferSize, 0, &data);
		if (mapResult != VK_SUCCESS || data == nullptr)
		{
			cemuLog_log(LogType::Force, "Vulkan VKRBuffer::Create vkMapMemory failed: VkResult={}, type={}, bytes={}, properties={:#x}", (sint32)mapResult, (sint32)bufferType, bufferSize, properties);
			delete bufferObj;
			return nullptr;
		}
		bufferObj->m_mappedMemory = (uint8*)data;
		bufferObj->m_requiresFlush = !HAS_FLAG(properties, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}
	return bufferObj;
}

VKRBuffer::~VKRBuffer()
{
	VkDevice logicalDevice = VulkanRenderer::GetInstance()->GetLogicalDevice();
	if (m_mappedMemory && m_bufferMemory != VK_NULL_HANDLE)
		vkUnmapMemory(logicalDevice, m_bufferMemory);
	// Destroy the bound buffer before releasing its VkDeviceMemory allocation.
	if (m_buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(logicalDevice, m_buffer, nullptr);
	if (m_bufferMemory != VK_NULL_HANDLE)
		vkFreeMemory(logicalDevice, m_bufferMemory, nullptr);
}

VkBufferChunkedHeap::~VkBufferChunkedHeap()
{
	for (auto& chunk : m_chunkBuffers)
		delete chunk;
}

uint32 VkBufferChunkedHeap::allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize)
{
	size_t allocationSize = std::max<size_t>(m_minimumBufferAllocationSize, minimumAllocationSize);
	VKRBuffer* buffer = VKRBuffer::Create(m_bufferType, allocationSize, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if(!buffer)
		buffer = VKRBuffer::Create(m_bufferType, allocationSize, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	if(!buffer)
		VulkanRenderer::GetInstance()->UnrecoverableError("Failed to allocate buffer memory for VkBufferChunkedHeap");
	cemu_assert_debug(buffer);
	cemu_assert_debug(m_chunkBuffers.size() == chunkIndex);
	m_chunkBuffers.emplace_back(buffer);
	// todo - VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT might be worth it?
	return allocationSize;
}

bool VKRMemoryManager::FindMemoryType(uint32 typeFilter, VkMemoryPropertyFlags properties, uint32& memoryIndex) const
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(m_vkr->GetPhysicalDevice(), &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if (typeFilter & (1 << i) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			memoryIndex = i;
			return true;
		}
	}
	return false;
}

std::vector<uint32> VKRMemoryManager::FindMemoryTypes(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
	std::vector<uint32> memoryTypes;
	memoryTypes.clear();
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(m_vkr->GetPhysicalDevice(), &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if (typeFilter & (1 << i) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
			memoryTypes.emplace_back(i);
	}

	if (memoryTypes.empty())
		m_vkr->UnrecoverableError(fmt::format("Failed to find suitable memory type ({0:#08x} {1:#08x})", typeFilter, properties).c_str());

	return memoryTypes;
}

size_t VKRMemoryManager::GetTotalMemoryForBufferType(VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, size_t minimumBufferSize)
{
	VkDevice logicalDevice = m_vkr->GetLogicalDevice();
	// create temporary buffer object to get memory type
	VkBuffer temporaryBuffer;
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.usage = usage;
	bufferInfo.size = minimumBufferSize; // the buffer size can theoretically influence the memory type, is there a better way to handle this?
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(logicalDevice, &bufferInfo, nullptr, &temporaryBuffer) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan: GetTotalMemoryForBufferType() failed to create temporary buffer");
		return 0;
	}

	// get memory requirements for buffer
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(logicalDevice, temporaryBuffer, &memRequirements);
	uint32 typeFilter = memRequirements.memoryTypeBits;
	// destroy temporary buffer
	vkDestroyBuffer(logicalDevice, temporaryBuffer, nullptr);
	// get list of all suitable heaps
	std::unordered_set<uint32> list_heapIndices;
	VkPhysicalDeviceMemoryProperties memProperties{};
	vkGetPhysicalDeviceMemoryProperties(m_vkr->GetPhysicalDevice(), &memProperties);
	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if (typeFilter & (1 << i) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
			list_heapIndices.emplace(memProperties.memoryTypes[i].heapIndex);
	}
	// sum up size of heaps
	size_t total = 0;
	for (auto heapIndex : list_heapIndices)
	{
		if (heapIndex > memProperties.memoryHeapCount)
			continue;
		total += memProperties.memoryHeaps[heapIndex].size;
	}

	return total;
}

bool VKRMemoryManager::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) const
{
	s_vkBufferBytesRequested.fetch_add(size, std::memory_order_relaxed);
	s_vkBufferCreateAttempts.fetch_add(1, std::memory_order_relaxed);
	auto recordFailure = []() { s_vkBufferCreateFailures.fetch_add(1, std::memory_order_relaxed); return false; };
	buffer = VK_NULL_HANDLE;
	bufferMemory = VK_NULL_HANDLE;
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.usage = usage;
	bufferInfo.size = size;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	const VkResult createResult = vkCreateBuffer(m_vkr->GetLogicalDevice(), &bufferInfo, nullptr, &buffer);
	if (createResult != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan vkCreateBuffer failed: VkResult={}, size={}, usage={:#x}, properties={:#x}", (sint32)createResult, size, usage, properties);
		return recordFailure();
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(m_vkr->GetLogicalDevice(), buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	if (!FindMemoryType(memRequirements.memoryTypeBits, properties, allocInfo.memoryTypeIndex))
	{
		cemuLog_log(LogType::Force, "Vulkan buffer has no compatible memory type: size={}, memoryTypeBits={:#x}, requestedProperties={:#x}", memRequirements.size, memRequirements.memoryTypeBits, properties);
		vkDestroyBuffer(m_vkr->GetLogicalDevice(), buffer, nullptr);
		buffer = VK_NULL_HANDLE;
		return recordFailure();
	}
	const VkResult allocationResult = vkAllocateMemory(m_vkr->GetLogicalDevice(), &allocInfo, nullptr, &bufferMemory);
	if (allocationResult != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan buffer memory allocation failed: VkResult={}, bytes={}, memoryType={}", (sint32)allocationResult, allocInfo.allocationSize, allocInfo.memoryTypeIndex);
		vkDestroyBuffer(m_vkr->GetLogicalDevice(), buffer, nullptr);
		buffer = VK_NULL_HANDLE;
		return recordFailure();
	}
	VkResult bindResult = vkBindBufferMemory(m_vkr->GetLogicalDevice(), buffer, bufferMemory, 0);
	if (bindResult != VK_SUCCESS)
	{
		vkDestroyBuffer(m_vkr->GetLogicalDevice(), buffer, nullptr);
		vkFreeMemory(m_vkr->GetLogicalDevice(), bufferMemory, nullptr);
		buffer = VK_NULL_HANDLE;
		bufferMemory = VK_NULL_HANDLE;
		cemuLog_log(LogType::Force, "Failed to bind buffer (CreateBuffer): VkResult {}", (sint32)bindResult);
		return recordFailure();
	}
	s_vkBufferCreateSuccesses.fetch_add(1, std::memory_order_relaxed);
	s_vkBufferBytesAllocated.fetch_add(memRequirements.size, std::memory_order_relaxed);
	return true;
}

bool VKRMemoryManager::CreateBufferFromHostMemory(void* hostPointer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) const
{
	s_vkBufferBytesRequested.fetch_add(size, std::memory_order_relaxed);
	s_vkBufferCreateAttempts.fetch_add(1, std::memory_order_relaxed);
	auto recordFailure = []() { s_vkBufferCreateFailures.fetch_add(1, std::memory_order_relaxed); return false; };
	buffer = VK_NULL_HANDLE;
	bufferMemory = VK_NULL_HANDLE;
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.usage = usage;
	bufferInfo.size = size;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkExternalMemoryBufferCreateInfo emb{};
	emb.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	emb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

	bufferInfo.pNext = &emb;

	const VkResult createResult = vkCreateBuffer(m_vkr->GetLogicalDevice(), &bufferInfo, nullptr, &buffer);
	if (createResult != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan host-pointer vkCreateBuffer failed: VkResult={}, size={}, usage={:#x}", (sint32)createResult, size, usage);
		return recordFailure();
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(m_vkr->GetLogicalDevice(), buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;

	VkImportMemoryHostPointerInfoEXT importHostMem{};
	importHostMem.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
	importHostMem.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	importHostMem.pHostPointer = hostPointer;
	// VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT or
	// VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT
	// whats the difference ?

	allocInfo.pNext = &importHostMem;

	if (!FindMemoryType(memRequirements.memoryTypeBits, properties, allocInfo.memoryTypeIndex))
	{
		cemuLog_log(LogType::Force, "Vulkan host-pointer buffer has no compatible memory type: size={}, memoryTypeBits={:#x}, requestedProperties={:#x}", memRequirements.size, memRequirements.memoryTypeBits, properties);
		vkDestroyBuffer(m_vkr->GetLogicalDevice(), buffer, nullptr);
		buffer = VK_NULL_HANDLE;
		return recordFailure();
	}
	const VkResult allocationResult = vkAllocateMemory(m_vkr->GetLogicalDevice(), &allocInfo, nullptr, &bufferMemory);
	if (allocationResult != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan host-pointer buffer memory allocation failed: VkResult={}, bytes={}, memoryType={}", (sint32)allocationResult, allocInfo.allocationSize, allocInfo.memoryTypeIndex);
		vkDestroyBuffer(m_vkr->GetLogicalDevice(), buffer, nullptr);
		buffer = VK_NULL_HANDLE;
		return recordFailure();
	}
	VkResult bindResult = vkBindBufferMemory(m_vkr->GetLogicalDevice(), buffer, bufferMemory, 0);
	if (bindResult != VK_SUCCESS)
	{
		vkDestroyBuffer(m_vkr->GetLogicalDevice(), buffer, nullptr);
		vkFreeMemory(m_vkr->GetLogicalDevice(), bufferMemory, nullptr);
		buffer = VK_NULL_HANDLE;
		bufferMemory = VK_NULL_HANDLE;
		cemuLog_log(LogType::Force, "Failed to bind buffer (CreateBufferFromHostMemory): VkResult {}", (sint32)bindResult);
		return recordFailure();
	}
	s_vkBufferCreateSuccesses.fetch_add(1, std::memory_order_relaxed);
	s_vkBufferBytesAllocated.fetch_add(memRequirements.size, std::memory_order_relaxed);
	return true;
}

void VKRMemoryManager::DeleteBuffer(VkBuffer& buffer, VkDeviceMemory& deviceMem) const
{
	if (buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(m_vkr->GetLogicalDevice(), buffer, nullptr);
	if (deviceMem != VK_NULL_HANDLE)
		vkFreeMemory(m_vkr->GetLogicalDevice(), deviceMem, nullptr);
	buffer = VK_NULL_HANDLE;
	deviceMem = VK_NULL_HANDLE;
}

VkImageMemAllocation* VKRMemoryManager::imageMemoryAllocate(VkImage image)
{
	s_vkImageAllocationAttempts.fetch_add(1, std::memory_order_relaxed);
	VkMemoryRequirements memRequirements{};
	vkGetImageMemoryRequirements(m_vkr->GetLogicalDevice(), image, &memRequirements);
	if (memRequirements.size == 0 || memRequirements.size > 0x7FFF0000ull ||
		memRequirements.alignment == 0 || memRequirements.alignment > std::numeric_limits<uint32>::max() ||
		(memRequirements.alignment & (memRequirements.alignment - 1)) != 0)
	{
		s_vkImageAllocationFailures.fetch_add(1, std::memory_order_relaxed);
		cemuLog_log(LogType::Force, "Vulkan image memory requirements exceed the texture heap limits: image={}, size={}, alignment={}, memoryTypeBits={:#x}", (void*)image, memRequirements.size, memRequirements.alignment, memRequirements.memoryTypeBits);
		m_vkr->UnrecoverableError("Invalid or unsupported Vulkan image memory requirements");
		return nullptr;
	}
	uint32 typeFilter = memRequirements.memoryTypeBits;

	// get or create heap for this type filter
	VkTextureChunkedHeap* texHeap;
	auto it = map_textureHeap.find(typeFilter);
	if (it == map_textureHeap.end())
	{
		texHeap = new VkTextureChunkedHeap(this, typeFilter);
		map_textureHeap.emplace(typeFilter, texHeap);
	}
	else
		texHeap = it->second.get();

	// alloc mem from heap
	uint32 allocationSize = (uint32)memRequirements.size;

	CHAddr mem = texHeap->allocMem(allocationSize, (uint32)memRequirements.alignment);
	if (!mem.isValid())
	{
		// allocation failed, try to make space by deleting textures
		// todo - improve this algorithm
		std::vector<LatteTexture*> deleteableTextures = LatteTC_GetDeleteableTextures();
		// delete up to 20 textures from the deletable textures list, then retry allocation
		while (!deleteableTextures.empty())
		{
			size_t numDelete = deleteableTextures.size();
			if (numDelete > 20)
				numDelete = 20;
			for (size_t i = 0; i < numDelete; i++)
				LatteTexture_Delete(deleteableTextures[i]);
			deleteableTextures.erase(deleteableTextures.begin(), deleteableTextures.begin() + numDelete);
			mem = texHeap->allocMem(allocationSize, (uint32)memRequirements.alignment);
			if (mem.isValid())
				break;
		}
		if (!mem.isValid())
		{
			s_vkImageAllocationFailures.fetch_add(1, std::memory_order_relaxed);
			cemuLog_log(LogType::Force, "Vulkan image allocation failed: requested={} bytes, alignment={}, memoryTypeBits={:#x}", allocationSize, memRequirements.alignment, typeFilter);
			m_vkr->UnrecoverableError("Ran out of VRAM for textures");
		}
	}

	VkResult bindResult = vkBindImageMemory(m_vkr->GetLogicalDevice(), image, texHeap->getChunkMem(mem.chunkIndex), mem.offset);
	if (bindResult != VK_SUCCESS)
	{
		s_vkImageAllocationFailures.fetch_add(1, std::memory_order_relaxed);
		texHeap->freeMem(mem);
		cemuLog_log(LogType::Force,
			"Vulkan: vkBindImageMemory failed (VkResult {}, image {}, allocation {} bytes, memory type bits {:#x})",
			(sint32)bindResult, (void*)image, allocationSize, typeFilter);
		m_vkr->UnrecoverableError("Failed to bind Vulkan image memory");
		return nullptr;
	}

	s_vkImageAllocationSuccesses.fetch_add(1, std::memory_order_relaxed);
	s_vkImageAllocationBytes.fetch_add(allocationSize, std::memory_order_relaxed);
	s_vkTextureBytesCurrentlyAllocated.fetch_add(allocationSize, std::memory_order_relaxed);
	s_vkTextureAllocationsCurrentlyLive.fetch_add(1, std::memory_order_relaxed);
	return new VkImageMemAllocation(typeFilter, mem, allocationSize);
}

void VKRMemoryManager::imageMemoryFree(VkImageMemAllocation* imageMemAllocation)
{
	auto heapItr = map_textureHeap.find(imageMemAllocation->typeFilter);
	if (heapItr == map_textureHeap.end())
	{
		cemuLog_log(LogType::Force, "Internal texture heap error");
		return;
	}
	s_vkTextureBytesCurrentlyAllocated.fetch_sub(imageMemAllocation->allocationSize, std::memory_order_relaxed);
	s_vkTextureAllocationsCurrentlyLive.fetch_sub(1, std::memory_order_relaxed);
	heapItr->second->freeMem(imageMemAllocation->mem);
	delete imageMemAllocation;
}

uint32 VKRMemoryManager::GetNonCoherentAtomSize() const
{
	return m_vkr->GetNonCoherentAtomSize();
}

VulkanMemoryDiagnostics VKRMemoryManager::GetDiagnostics() const
{
	VulkanMemoryDiagnostics result;
	result.textureHeapReservedBytes = s_vkTextureHeapReservedBytes.load(std::memory_order_relaxed);
	result.textureHeapChunks = s_vkTextureHeapChunks.load(std::memory_order_relaxed);
	result.textureBytesCurrentlyAllocated = s_vkTextureBytesCurrentlyAllocated.load(std::memory_order_relaxed);
	result.textureAllocationsCurrentlyLive = s_vkTextureAllocationsCurrentlyLive.load(std::memory_order_relaxed);
	result.bufferBytesRequested = s_vkBufferBytesRequested.load(std::memory_order_relaxed);
	result.bufferBytesAllocatedCumulative = s_vkBufferBytesAllocated.load(std::memory_order_relaxed);
	result.bufferCreateAttempts = s_vkBufferCreateAttempts.load(std::memory_order_relaxed);
	result.bufferCreateSuccesses = s_vkBufferCreateSuccesses.load(std::memory_order_relaxed);
	result.bufferCreateFailures = s_vkBufferCreateFailures.load(std::memory_order_relaxed);
	result.imageAllocationAttempts = s_vkImageAllocationAttempts.load(std::memory_order_relaxed);
	result.imageAllocationSuccesses = s_vkImageAllocationSuccesses.load(std::memory_order_relaxed);
	result.imageAllocationFailures = s_vkImageAllocationFailures.load(std::memory_order_relaxed);
	result.imageAllocationBytesCumulative = s_vkImageAllocationBytes.load(std::memory_order_relaxed);
	return result;
}

void VKRMemoryManager::appendOverlayHeapDebugInfo()
{
	for (auto& itr : map_textureHeap)
	{
		uint32 heapSize;
		uint32 allocatedBytes;
		itr.second->getStatistics(heapSize, allocatedBytes);

		uint32 heapSizeMB = (heapSize / 1024 / 1024);
		uint32 allocatedBytesMB = (allocatedBytes / 1024 / 1024);

		ImGui::Text("%s", fmt::format("{0:#08x} Size: {1}MB/{2}MB", itr.first, allocatedBytesMB, heapSizeMB).c_str());
	}
}
