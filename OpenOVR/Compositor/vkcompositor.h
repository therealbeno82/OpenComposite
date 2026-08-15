#pragma once

#include "compositor.h"

class VkCompositor : public Compositor {
public:
	VkCompositor(const vr::Texture_t* initialTexture);

	~VkCompositor() override;

	void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) override;

	void InvokeCubemap(const vr::Texture_t* textures) override;

private:
	bool CheckChainCompatible(const vr::VRVulkanTextureData_t& tex, const XrSwapchainCreateInfo& chainDesc, vr::EColorSpace colourSpace) const;

	// The sample count of the app texture the current swapchain was built for. When the runtime can't
	// provide a swapchain with that many samples we create a single-sampled one and resolve into it, so
	// this can't be recovered from createInfo.sampleCount.
	uint32_t createInfoSourceSampleCount = 0;

	// These resources live in the runtime's VkDevice
	std::vector<XrSwapchainImageVulkanKHR> swapchainImages;

	// These resources live in the app's VkDevice
	VkDevice appDevice = VK_NULL_HANDLE;
	VkQueue appQueue = VK_NULL_HANDLE;
	VkCommandPool appCommandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> appCommandBuffers{};
};
