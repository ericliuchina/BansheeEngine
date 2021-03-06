//********************************** Banshee Engine (www.banshee3d.com) **************************************************//
//**************** Copyright (c) 2016 Marko Pintera (marko.pintera@gmail.com). All rights reserved. **********************//
#include "BsVulkanTexture.h"
#include "BsVulkanRenderAPI.h"
#include "BsVulkanDevice.h"
#include "BsVulkanUtility.h"
#include "BsVulkanCommandBufferManager.h"
#include "BsVulkanHardwareBuffer.h"
#include "BsCoreThread.h"
#include "BsRenderStats.h"
#include "BsMath.h"

namespace bs
{
	VulkanImage::VulkanImage(VulkanResourceManager* owner, VkImage image, VkDeviceMemory memory, VkImageLayout layout,
							 const TextureProperties& props)
		: VulkanResource(owner, false), mImage(image), mMemory(memory), mLayout(layout)
	{
		mImageViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		mImageViewCI.pNext = nullptr;
		mImageViewCI.flags = 0;
		mImageViewCI.image = image;
		mImageViewCI.format = VulkanUtility::getPixelFormat(props.getFormat());
		mImageViewCI.components = {
			VK_COMPONENT_SWIZZLE_R,
			VK_COMPONENT_SWIZZLE_G,
			VK_COMPONENT_SWIZZLE_B,
			VK_COMPONENT_SWIZZLE_A
		};

		switch (props.getTextureType())
		{
		case TEX_TYPE_1D:
			mImageViewCI.viewType = VK_IMAGE_VIEW_TYPE_1D;
			break;
		default:
		case TEX_TYPE_2D:
			mImageViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
			break;
		case TEX_TYPE_3D:
			mImageViewCI.viewType = VK_IMAGE_VIEW_TYPE_3D;
			break;
		case TEX_TYPE_CUBE_MAP:
			mImageViewCI.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
			break;
		}

		if ((props.getUsage() & TU_DEPTHSTENCIL) != 0)
			mImageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		else
			mImageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

		TextureSurface completeSurface(0, props.getNumMipmaps() + 1, 0, props.getNumArraySlices());
		mMainView = createView(completeSurface);
	}

	VulkanImage::~VulkanImage()
	{
		VulkanDevice& device = mOwner->getDevice();
		VkDevice vkDevice = device.getLogical();

		vkDestroyImageView(vkDevice, mMainView, gVulkanAllocator);

		for(auto& entry : mImageInfos)
			vkDestroyImageView(vkDevice, entry.view, gVulkanAllocator);

		vkDestroyImage(vkDevice, mImage, gVulkanAllocator);
		device.freeMemory(mMemory);
	}

	VkImageView VulkanImage::getView(const TextureSurface& surface) const
	{
		for(auto& entry : mImageInfos)
		{
			if (surface.mipLevel == entry.surface.mipLevel &&
				surface.numMipLevels == entry.surface.numMipLevels &&
				surface.arraySlice == entry.surface.arraySlice &&
				surface.numArraySlices == entry.surface.numArraySlices)
			{
				return entry.view;
			}
		}

		ImageViewInfo info;
		info.surface = surface;
		info.view = createView(surface);

		mImageInfos.push_back(info);

		return info.view;
	}

	VkImageView VulkanImage::createView(const TextureSurface& surface) const
	{
		VkImageViewType oldViewType = mImageViewCI.viewType;

		if(surface.numArraySlices > 1)
		{
			switch (oldViewType)
			{
			case VK_IMAGE_VIEW_TYPE_1D:
				mImageViewCI.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
				break;
			case VK_IMAGE_VIEW_TYPE_2D:
				mImageViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
				break;
			case VK_IMAGE_VIEW_TYPE_CUBE:
				mImageViewCI.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
				break;
			default:
				break;
			}
		}

		mImageViewCI.subresourceRange.baseMipLevel = surface.mipLevel;
		mImageViewCI.subresourceRange.levelCount = surface.numMipLevels;
		mImageViewCI.subresourceRange.baseArrayLayer = surface.arraySlice;
		mImageViewCI.subresourceRange.layerCount = surface.numArraySlices;

		VkImageView view;
		VkResult result = vkCreateImageView(mOwner->getDevice().getLogical(), &mImageViewCI, gVulkanAllocator, &view);
		assert(result == VK_SUCCESS);

		mImageViewCI.viewType = oldViewType;
		return view;
	}

	void VulkanImage::copy(VulkanTransferBuffer* cb, VulkanBuffer* destination, const VkExtent3D& extent,
							const VkImageSubresourceLayers& range, VkImageLayout layout)
	{
		VkBufferImageCopy region;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.bufferOffset = 0;
		region.imageOffset.x = 0;
		region.imageOffset.y = 0;
		region.imageOffset.z = 0;
		region.imageExtent = extent;
		region.imageSubresource = range;

		vkCmdCopyImageToBuffer(cb->getCB()->getHandle(), mImage, layout, destination->getHandle(), 1, &region);
	}

	VulkanTextureCore::VulkanTextureCore(const TEXTURE_DESC& desc, const SPtr<PixelData>& initialData,
		GpuDeviceFlags deviceMask)
		: TextureCore(desc, initialData, deviceMask), mImages(), mDeviceMask(deviceMask), mAccessFlags(0)
		, mStagingBuffer(nullptr), mMappedDeviceIdx(-1), mMappedGlobalQueueIdx(-1), mMappedMip(0), mMappedFace(0)
		, mMappedLockOptions(GBL_WRITE_ONLY), mDirectlyMappable(false), mSupportsGPUWrites(false), mIsMapped(false)
	{
		
	}

	VulkanTextureCore::~VulkanTextureCore()
	{ 
		for (UINT32 i = 0; i < BS_MAX_DEVICES; i++)
		{
			if (mImages[i] == nullptr)
				return;

			mImages[i]->destroy();
		}

		assert(mStagingBuffer == nullptr);

		BS_INC_RENDER_STAT_CAT(ResDestroyed, RenderStatObject_Texture);
	}

	void VulkanTextureCore::initialize()
	{
		THROW_IF_NOT_CORE_THREAD;

		const TextureProperties& props = mProperties;

		mImageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		mImageCI.pNext = nullptr;
		mImageCI.flags = 0;

		// Note: If usage is dynamic I might consider creating a VK_IMAGE_TILING_LINEAR (if supported by the device)

		TextureType texType = props.getTextureType();
		switch(texType)
		{
		case TEX_TYPE_1D:
			mImageCI.imageType = VK_IMAGE_TYPE_1D;
			break;
		case TEX_TYPE_2D:
			mImageCI.imageType = VK_IMAGE_TYPE_2D;
			break;
		case TEX_TYPE_3D:
			mImageCI.imageType = VK_IMAGE_TYPE_3D;
			break;
		case TEX_TYPE_CUBE_MAP:
			mImageCI.imageType = VK_IMAGE_TYPE_2D;
			mImageCI.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			break;
		}

		int usage = props.getUsage();
		if ((usage & TU_RENDERTARGET) != 0)
		{
			mImageCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			mSupportsGPUWrites = true;
			mAccessFlags = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		}
		else if ((usage & TU_DEPTHSTENCIL) != 0)
		{
			mImageCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			mSupportsGPUWrites = true;
			mAccessFlags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		}
		else if ((usage & TU_LOADSTORE) != 0)
		{
			mImageCI.usage = VK_IMAGE_USAGE_STORAGE_BIT;
			mSupportsGPUWrites = true;
			mAccessFlags = VK_ACCESS_SHADER_WRITE_BIT;
		}
		else
		{
			mImageCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			mAccessFlags = VK_ACCESS_SHADER_READ_BIT;;
		}

		if ((usage & TU_CPUREADABLE) != 0)
			mImageCI.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		if ((usage & TU_DYNAMIC) != 0) // Attempt to use linear tiling for dynamic textures, so we can directly map and modify them
		{
			// Only support 2D textures, with one sample and one mip level, only used for shader reads
			// (Optionally check vkGetPhysicalDeviceFormatProperties & vkGetPhysicalDeviceImageFormatProperties for
			// additional supported configs, but right now there doesn't seem to be any additional support)
			if(texType == TEX_TYPE_2D && props.getNumSamples() <= 1 && props.getNumMipmaps() == 0 && 
				props.getNumFaces() == 1 && (mImageCI.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0)
			{
				mDirectlyMappable = true;
				tiling = VK_IMAGE_TILING_LINEAR;
				layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
			}
		}

		mImageCI.format = VulkanUtility::getPixelFormat(props.getFormat());
		mImageCI.extent = { props.getWidth(), props.getHeight(), props.getDepth() };
		mImageCI.mipLevels = props.getNumMipmaps() + 1;
		mImageCI.arrayLayers = props.getNumFaces();
		mImageCI.samples = VulkanUtility::getSampleFlags(props.getNumSamples());
		mImageCI.tiling = tiling;
		mImageCI.initialLayout = layout;
		mImageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		mImageCI.queueFamilyIndexCount = 0;
		mImageCI.pQueueFamilyIndices = nullptr;

		VulkanRenderAPI& rapi = static_cast<VulkanRenderAPI&>(RenderAPICore::instance());
		VulkanDevice* devices[BS_MAX_DEVICES];
		VulkanUtility::getDevices(rapi, mDeviceMask, devices);

		// Allocate buffers per-device
		for (UINT32 i = 0; i < BS_MAX_DEVICES; i++)
		{
			if (devices[i] == nullptr)
				continue;

			mImages[i] = createImage(*devices[i]);
		}

		BS_INC_RENDER_STAT_CAT(ResCreated, RenderStatObject_Texture);
		TextureCore::initialize();
	}

	VulkanImage* VulkanTextureCore::createImage(VulkanDevice& device)
	{
		bool directlyMappable = mImageCI.tiling == VK_IMAGE_TILING_LINEAR;
		VkMemoryPropertyFlags flags = directlyMappable ?
			(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : // Note: Try using cached memory
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		VkDevice vkDevice = device.getLogical();

		VkImage image;
		VkResult result = vkCreateImage(vkDevice, &mImageCI, gVulkanAllocator, &image);
		assert(result == VK_SUCCESS);

		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(vkDevice, image, &memReqs);

		VkDeviceMemory memory = device.allocateMemory(memReqs, flags);
		result = vkBindImageMemory(vkDevice, image, memory, 0);
		assert(result == VK_SUCCESS);

		return device.getResourceManager().create<VulkanImage>(image, memory, mImageCI.initialLayout, getProperties());
	}

	VulkanBuffer* VulkanTextureCore::createStaging(VulkanDevice& device, UINT32 mipLevel, bool readable)
	{
		const TextureProperties& props = getProperties();

		UINT32 mipWidth, mipHeight, mipDepth;
		PixelUtil::getSizeForMipLevel(props.getWidth(), props.getHeight(), props.getDepth(), mipLevel, mipWidth, mipHeight, 
			mipDepth);

		UINT32 mipLevelSize = PixelUtil::getMemorySize(mipWidth, mipHeight, mipDepth, props.getFormat());

		VkBufferCreateInfo bufferCI;
		bufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCI.pNext = nullptr;
		bufferCI.flags = 0;
		bufferCI.size = mipLevelSize;
		bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		bufferCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferCI.queueFamilyIndexCount = 0;
		bufferCI.pQueueFamilyIndices = nullptr;

		if (readable)
			bufferCI.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		VkDevice vkDevice = device.getLogical();

		VkBuffer buffer;
		VkResult result = vkCreateBuffer(vkDevice, &bufferCI, gVulkanAllocator, &buffer);
		assert(result == VK_SUCCESS);

		VkMemoryRequirements memReqs;
		vkGetBufferMemoryRequirements(vkDevice, buffer, &memReqs);

		VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

		VkDeviceMemory memory = device.allocateMemory(memReqs, flags);
		result = vkBindBufferMemory(vkDevice, buffer, memory, 0);
		assert(result == VK_SUCCESS);

		return device.getResourceManager().create<VulkanBuffer>(buffer, VK_NULL_HANDLE, memory);
	}

	VkImageView VulkanTextureCore::getView(UINT32 deviceIdx) const
	{
		if (mImages[deviceIdx] == nullptr)
			return VK_NULL_HANDLE;

		return mImages[deviceIdx]->getView();
	}

	VkImageView VulkanTextureCore::getView(UINT32 deviceIdx, const TextureSurface& surface) const
	{
		if (mImages[deviceIdx] == nullptr)
			return VK_NULL_HANDLE;

		return mImages[deviceIdx]->getView(surface);
	}

	void VulkanTextureCore::copyImpl(UINT32 srcFace, UINT32 srcMipLevel, UINT32 destFace, UINT32 destMipLevel,
									 const SPtr<TextureCore>& target, UINT32 queueIdx)
	{
		// TODO - Handle resolve here as well
	}

	PixelData VulkanTextureCore::lockImpl(GpuLockOptions options, UINT32 mipLevel, UINT32 face, UINT32 deviceIdx,
										  UINT32 queueIdx)
	{
		if (mProperties.getNumSamples() > 1)
		{
			LOGERR("Multisampled textures cannot be accessed from the CPU directly.");
			return PixelData();
		}

#if BS_PROFILING_ENABLED
		if (options == GBL_READ_ONLY || options == GBL_READ_WRITE)
		{
			BS_INC_RENDER_STAT_CAT(ResRead, RenderStatObject_Texture);
		}

		if (options == GBL_READ_WRITE || options == GBL_WRITE_ONLY || options == GBL_WRITE_ONLY_DISCARD || options == GBL_WRITE_ONLY_NO_OVERWRITE)
		{
			BS_INC_RENDER_STAT_CAT(ResWrite, RenderStatObject_Texture);
		}
#endif

		UINT32 mipWidth = std::max(1u, mProperties.getWidth() >> mipLevel);
		UINT32 mipHeight = std::max(1u, mProperties.getHeight() >> mipLevel);
		UINT32 mipDepth = std::max(1u, mProperties.getDepth() >> mipLevel);

		PixelData lockedArea(mipWidth, mipHeight, mipDepth, mProperties.getFormat());

		VulkanImage* image = mImages[deviceIdx];

		if (image == nullptr)
			return PixelData();

		mIsMapped = true;
		mMappedDeviceIdx = deviceIdx;
		mMappedGlobalQueueIdx = queueIdx;
		mMappedFace = face;
		mMappedMip = mipLevel;
		mMappedLockOptions = options;

		VulkanRenderAPI& rapi = static_cast<VulkanRenderAPI&>(RenderAPICore::instance());
		VulkanDevice& device = *rapi._getDevice(deviceIdx);

		VulkanCommandBufferManager& cbManager = gVulkanCBManager();
		GpuQueueType queueType;
		UINT32 localQueueIdx = CommandSyncMask::getQueueIdxAndType(queueIdx, queueType);

		VulkanImage* subresource = image->getSubresource(face, mipLevel);

		// If memory is host visible try mapping it directly
		if (mDirectlyMappable)
		{
			// Initially the texture will be in preinitialized layout, and it will transition to general layout on first
			// use in shader. No further transitions are allowed for directly mappable textures.
			assert(image->getLayout() == VK_IMAGE_LAYOUT_PREINITIALIZED || image->getLayout() == VK_IMAGE_LAYOUT_GENERAL);

			// GPU should never be allowed to write to a directly mappable texture, since only linear tiling is supported
			// for direct mapping, and we don't support using it with either storage textures or render targets.
			assert(!mSupportsGPUWrites);

			// Check is the GPU currently reading from the image
			UINT32 useMask = subresource->getUseInfo(VulkanUseFlag::Read);
			bool isUsedOnGPU = useMask != 0;

			// We're safe to map directly since GPU isn't using the subresource
			if (!isUsedOnGPU)
				return image->map(face, mipLevel);

			// Caller guarantees he won't touch the same data as the GPU, so just map even though the GPU is using the
			// subresource
			if (options == GBL_WRITE_ONLY_NO_OVERWRITE)
				return image->map(face, mipLevel);

			// No GPU writes are are supported and we're only reading, so no need to wait on anything
			if (options == GBL_READ_ONLY)
				return image->map(face, mipLevel);

			// Caller doesn't care about buffer contents, so just discard the existing buffer and create a new one
			if (options == GBL_WRITE_ONLY_DISCARD)
			{
				// TODO - Since I'm only writing to a single subresource, how will discard work? Discard every subresource?

				//buffer->destroy();

				//buffer = createBuffer(device, false, mReadable);
				//mBuffers[deviceIdx] = buffer;

				//return buffer->map(offset, length);
			}

			// We need to both read and write, meaning we need to wait until existing reads complete before we return
			if (options == GBL_READ_WRITE)
			{
				VulkanTransferBuffer* transferCB = cbManager.getTransferBuffer(deviceIdx, queueType, localQueueIdx);

				// Ensure flush() will wait for all queues currently reading from the texture to finish
				transferCB->appendMask(useMask);

				// Submit the command buffer and wait until it finishes
				transferCB->flush(true);

				return image->map(face, mipLevel);
			}

			// Otherwise, we're doing write only, in which case it's best to use the staging buffer to avoid waiting
			// and blocking, so fall through
		}

		bool needRead = options == GBL_READ_WRITE || options == GBL_READ_ONLY;

		// Allocate a staging buffer
		mStagingBuffer = createStaging(device, mipLevel, needRead);

		if (needRead) // If reading, we need to copy the current contents of the image to the staging buffer
		{
			VulkanTransferBuffer* transferCB = cbManager.getTransferBuffer(deviceIdx, queueType, localQueueIdx);

			// Similar to above, if image supports GPU writes, we need to wait on any potential writes to complete
			if (mSupportsGPUWrites)
			{
				// Ensure flush() will wait for all queues currently writing to the image (if any) to finish
				UINT32 writeUseMask = subresource->getUseInfo(VulkanUseFlag::Write);
				transferCB->appendMask(writeUseMask);
			}

			const TextureProperties& props = getProperties();

			VkImageSubresourceRange range;
			if ((props.getUsage() & TU_DEPTHSTENCIL) != 0)
				range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			else
				range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

			range.baseArrayLayer = face;
			range.layerCount = 1;
			range.baseMipLevel = mipLevel;
			range.levelCount = 1;

			VkImageSubresourceLayers rangeLayers;
			if ((props.getUsage() & TU_DEPTHSTENCIL) != 0)
				rangeLayers.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			else
				rangeLayers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

			rangeLayers.baseArrayLayer = range.baseArrayLayer;
			rangeLayers.layerCount = range.layerCount;
			rangeLayers.mipLevel = range.baseMipLevel;

			VkExtent3D extent;
			PixelUtil::getSizeForMipLevel(props.getWidth(), props.getHeight(), props.getDepth(), mMappedMip,
										  extent.width, extent.height, extent.depth);

			// Transfer texture to a valid layout
			transferCB->setLayout(image->getHandle(), mAccessFlags, VK_ACCESS_TRANSFER_READ_BIT, image->getLayout(),
								  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, range);

			// Queue copy command
			image->copy(transferCB, mStagingBuffer, extent, rangeLayers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

			// Transfer back to original layout
			transferCB->setLayout(image->getHandle(), VK_ACCESS_TRANSFER_READ_BIT, mAccessFlags,
								  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->getLayout(), range);

			// Ensure data written to the staging buffer is visible
			VkAccessFlags stagingAccessFlags;
			if (options == GBL_READ_ONLY)
				stagingAccessFlags = VK_ACCESS_HOST_READ_BIT;
			else // Must be read/write
				stagingAccessFlags = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;

			transferCB->memoryBarrier(mStagingBuffer->getHandle(),
									  VK_ACCESS_TRANSFER_WRITE_BIT,
									  stagingAccessFlags,
									  VK_PIPELINE_STAGE_TRANSFER_BIT,
									  VK_PIPELINE_STAGE_HOST_BIT);

			// Submit the command buffer and wait until it finishes
			transferCB->flush(true);
		}

		return mStagingBuffer->map(offset, length);
	}

	void VulkanTextureCore::unlockImpl()
	{
		// Possibly map() failed with some error
		if (!mIsMapped)
			return;

		// Note: If we did any writes they need to be made visible to the GPU. However there is no need to execute 
		// a pipeline barrier because (as per spec) host writes are implicitly visible to the device.

		if (mStagingBuffer == nullptr)
			mImages[mMappedDeviceIdx]->unmap();
		else
		{
			bool isWrite = mMappedLockOptions != GBL_READ_ONLY;

			// We the caller wrote anything to the staging buffer, we need to upload it back to the main buffer
			if (isWrite)
			{
				VulkanRenderAPI& rapi = static_cast<VulkanRenderAPI&>(RenderAPICore::instance());
				VulkanDevice& device = *rapi._getDevice(mMappedDeviceIdx);

				VulkanCommandBufferManager& cbManager = gVulkanCBManager();
				GpuQueueType queueType;
				UINT32 localQueueIdx = CommandSyncMask::getQueueIdxAndType(mMappedGlobalQueueIdx, queueType);

				VulkanImage* image = mImages[mMappedDeviceIdx];
				VulkanTransferBuffer* transferCB = cbManager.getTransferBuffer(mMappedDeviceIdx, queueType, localQueueIdx);

				VulkanImage* subresource = image->getSubresource(mMappedFace, mMappedMip);

				// If the subresource is used in any way on the GPU, we need to wait for that use to finish before
				// we issue our copy
				UINT32 useMask = subresource->getUseInfo(VulkanUseFlag::Read | VulkanUseFlag::Write);
				if (useMask != 0) // Subresource is currently used on the GPU
				{
					// Try to avoid the wait

					// Caller guarantees he won't touch the same data as the GPU, so just copy
					if (mMappedLockOptions == GBL_WRITE_ONLY_NO_OVERWRITE) 
					{
						// Fall through to copy()
					}
					// Caller doesn't care about buffer contents, so just discard the  existing buffer and create a new one
					else if (mMappedLockOptions == GBL_WRITE_ONLY_DISCARD) 
					{
						// TODO - Handle discard

						//buffer->destroy();

						//buffer = createBuffer(device, false, mReadable);
						//mBuffers[mMappedDeviceIdx] = buffer;
					}
					else // Otherwise we have no choice but to issue a dependency between the queues
						transferCB->appendMask(useMask);
				}

				const TextureProperties& props = getProperties();

				VkImageSubresourceRange range;
				if ((props.getUsage() & TU_DEPTHSTENCIL) != 0)
					range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
				else
					range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

				range.baseArrayLayer = mMappedFace;
				range.layerCount = 1;
				range.baseMipLevel = mMappedMip;
				range.levelCount = 1;

				VkImageSubresourceLayers rangeLayers;
				rangeLayers.aspectMask = range.aspectMask;
				rangeLayers.baseArrayLayer = range.baseArrayLayer;
				rangeLayers.layerCount = range.layerCount;
				rangeLayers.mipLevel = range.baseMipLevel;

				VkExtent3D extent;
				PixelUtil::getSizeForMipLevel(props.getWidth(), props.getHeight(), props.getDepth(), mMappedMip,
											  extent.width, extent.height, extent.depth);

				VkImageLayout transferLayout;
				if (mDirectlyMappable)
					transferLayout = VK_IMAGE_LAYOUT_GENERAL;
				else
					transferLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

				// Transfer texture to a valid layout
				transferCB->setLayout(image->getHandle(), mAccessFlags, VK_ACCESS_TRANSFER_WRITE_BIT, image->getLayout(),
									  transferLayout, range);

				// Queue copy command
				mStagingBuffer->copy(transferCB, image, extent, rangeLayers, transferLayout);

				// Transfer back to original layout
				transferCB->setLayout(image->getHandle(), VK_ACCESS_TRANSFER_WRITE_BIT, mAccessFlags,
									  transferLayout, image->getLayout(), range);

				// Notify the command buffer that these resources are being used on it
				transferCB->getCB()->registerResource(mStagingBuffer, VK_ACCESS_TRANSFER_READ_BIT, VulkanUseFlag::Read);
				transferCB->getCB()->registerResource(image, mAccessFlags, image->getLayout(), range, VulkanUseFlag::Write);

				// We don't actually flush the transfer buffer here since it's an expensive operation, but it's instead
				// done automatically before next "normal" command buffer submission.
			}

			mStagingBuffer->unmap();

			mStagingBuffer->destroy();
			mStagingBuffer = nullptr;
		}

		mIsMapped = false;
	}

	void VulkanTextureCore::readDataImpl(PixelData& dest, UINT32 mipLevel, UINT32 face, UINT32 deviceIdx, UINT32 queueIdx)
	{
		if (mProperties.getNumSamples() > 1)
		{
			LOGERR("Multisampled textures cannot be accessed from the CPU directly.");
			return;
		}

		PixelData myData = lock(GBL_READ_ONLY, mipLevel, face, deviceIdx, queueIdx);

#if BS_DEBUG_MODE
		if (dest.getConsecutiveSize() != myData.getConsecutiveSize())
		{
			unlock();
			BS_EXCEPT(InternalErrorException, "Buffer sizes don't match");
		}
#endif

		PixelUtil::bulkPixelConversion(myData, dest);

		unlock();

		BS_INC_RENDER_STAT_CAT(ResRead, RenderStatObject_Texture);
	}

	void VulkanTextureCore::writeDataImpl(const PixelData& src, UINT32 mipLevel, UINT32 face, bool discardWholeBuffer,
									  UINT32 queueIdx)
	{
		if (mProperties.getNumSamples() > 1)
		{
			LOGERR("Multisampled textures cannot be accessed from the CPU directly.");
			return;
		}

		PixelFormat format = mProperties.getFormat();

		mipLevel = Math::clamp(mipLevel, (UINT32)mipLevel, mProperties.getNumMipmaps());
		face = Math::clamp(face, (UINT32)0, mProperties.getNumFaces() - 1);

		if (face > 0 && mProperties.getTextureType() == TEX_TYPE_3D)
		{
			LOGERR("3D texture arrays are not supported.");
			return;
		}

		// Write to every device
		for (UINT32 i = 0; i < BS_MAX_DEVICES; i++)
		{
			if (mImages[i] == nullptr)
				continue;

			PixelData myData = lock(discardWholeBuffer ? GBL_WRITE_ONLY_DISCARD : GBL_WRITE_ONLY, mipLevel, face, i,
									queueIdx);
			PixelUtil::bulkPixelConversion(src, myData);
			unlock();
		}

		BS_INC_RENDER_STAT_CAT(ResWrite, RenderStatObject_Texture);
	}
}
