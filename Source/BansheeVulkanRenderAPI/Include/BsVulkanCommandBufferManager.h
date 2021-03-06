//********************************** Banshee Engine (www.banshee3d.com) **************************************************//
//**************** Copyright (c) 2016 Marko Pintera (marko.pintera@gmail.com). All rights reserved. **********************//
#pragma once

#include "BsVulkanPrerequisites.h"
#include "BsCommandBufferManager.h"
#include "BsVulkanCommandBuffer.h"

namespace bs
{
	/** @addtogroup Vulkan
	 *  @{
	 */

	/** Wrapper around a command buffer used specifically for transfer operations. */
	class VulkanTransferBuffer
	{
	public:
		VulkanTransferBuffer();
		VulkanTransferBuffer(VulkanDevice* device, GpuQueueType type, UINT32 queueIdx);
		~VulkanTransferBuffer();

		/** 
		 * OR's the provided sync mask with the internal sync mask. The sync mask determines on which queues should
		 * the buffer wait on before executing. Sync mask is reset after a flush. See CommandSyncMask on how to generate
		 * a sync mask.
		 */
		void appendMask(UINT32 syncMask) { mSyncMask |= syncMask; }

		/** Resets the sync mask. */
		void clearMask() { mSyncMask = 0; }

		/** 
		 * Issues a pipeline barrier on the provided buffer. See vkCmdPipelineBarrier in Vulkan spec. for usage
		 * information.
		 */
		void memoryBarrier(VkBuffer buffer, VkAccessFlags srcAccessFlags, VkAccessFlags dstAccessFlags,
						   VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

		/** 
		 * Issues a pipeline barrier on the provided image, changing its layout. See vkCmdPipelineBarrier in Vulkan spec. 
		 * for usage information.
		 */
		void setLayout(VkImage image, VkAccessFlags srcAccessFlags, VkAccessFlags dstAccessFlags, 
			VkImageLayout oldLayout, VkImageLayout newLayout, const VkImageSubresourceRange& range);

		/** 
		 * Submits the command buffer on the queue. 
		 * 
		 *	@param[in]	wait	If true, the caller thread will wait until all device operations on the command buffer's
		 *						queue complete.	
		 */
		void flush(bool wait);

		/** Returns the internal command buffer. */
		VulkanCmdBuffer* getCB() const { return mCB; }
	private:
		friend class VulkanCommandBufferManager;

		/** Allocates a new internal command buffer. */
		void allocate();

		VulkanDevice* mDevice;
		GpuQueueType mType;
		UINT32 mQueueIdx;
		VulkanQueue* mQueue;
		UINT32 mQueueMask;

		VulkanCmdBuffer* mCB;
		UINT32 mSyncMask;
	};

	/** 
	 * Handles creation of Vulkan command buffers. See CommandBuffer. 
	 *
	 * @note Core thread only.
	 */
	class VulkanCommandBufferManager : public CommandBufferManager
	{
	public:
		VulkanCommandBufferManager(const VulkanRenderAPI& rapi);
		~VulkanCommandBufferManager();

		/** @copydoc CommandBufferManager::createInternal() */
		SPtr<CommandBuffer> createInternal(GpuQueueType type, UINT32 deviceIdx = 0, UINT32 queueIdx = 0,
			bool secondary = false) override;

		/** Notifies the manager that this buffer was just submitted to the queue for execution. */
		void setActiveBuffer(GpuQueueType type, UINT32 deviceIdx, UINT32 queueIdx, VulkanCmdBuffer* buffer);

		/** 
		 * Returns a set of command buffer semaphores depending on the provided sync mask. 
		 *
		 * @param[in]	deviceIdx	Index of the device to get the semaphores for.
		 * @param[in]	syncMask	Mask that has a bit enabled for each command buffer to retrieve the semaphore for.
		 *							If the command buffer is not currently executing, semaphore won't be returned.
		 * @param[out]	semaphores	List containing all the required semaphores. Semaphores are tightly packed at the
		 *							beginning of the array. Must be able to hold at least BS_MAX_COMMAND_BUFFERS entries.
		 * @param[out]	count		Number of semaphores provided in the @p semaphores array.
		 */
		void getSyncSemaphores(UINT32 deviceIdx, UINT32 syncMask, VkSemaphore* semaphores, UINT32& count);

		/** 
		 * Checks if any of the active command buffers finished executing on the device and updates their states 
		 * accordingly. 
		 */
		void refreshStates(UINT32 deviceIdx);

		/** 
		 * Returns an command buffer that can be used for executing transfer operations on the specified queue. 
		 * Transfer buffers are automatically flushed (submitted) whenever a new (normal) command buffer is about to
		 * execute.
		 */
		VulkanTransferBuffer* getTransferBuffer(UINT32 deviceIdx, GpuQueueType type, UINT32 queueIdx);

		/** Submits all transfer command buffers, ensuring all queued transfer operations get executed. */
		void flushTransferBuffers(UINT32 deviceIdx);

	private:
		/** Contains command buffers specific to one device. */
		struct PerDeviceData
		{
			VulkanCmdBuffer* activeBuffers[BS_MAX_UNIQUE_QUEUES];
			VulkanTransferBuffer transferBuffers[GQT_COUNT][BS_MAX_QUEUES_PER_TYPE];
		};

		const VulkanRenderAPI& mRapi;

		PerDeviceData* mDeviceData;
		UINT32 mNumDevices;
	};

	/**	Provides easy access to the VulkanCommandBufferManager. */
	VulkanCommandBufferManager& gVulkanCBManager();

	/** @} */
}
