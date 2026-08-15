#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#include "dx12compositor.h"

#include <algorithm>
#include <string>

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <d3d12.h>

#include "../Misc/Config.h"
#include "../Misc/xr_ext.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

using namespace vr;
using namespace std;

namespace {
void WaitForFence(ID3D12Fence* fence, UINT64 completionValue, HANDLE waitEvent)
{
	if (fence->GetCompletedValue() < completionValue) {
		fence->SetEventOnCompletion(completionValue, waitEvent);
		WaitForSingleObject(waitEvent, INFINITE);
	}
}
} // namespace

DX12Compositor::DX12Compositor(D3D12TextureData_t* td)
{
	queue = td->m_pCommandQueue;
	queue->GetDevice(IID_PPV_ARGS(&device));
}

DX12Compositor::~DX12Compositor()
{
	for (auto event : frameFenceEvents) {
		CloseHandle(event);
	}

	// Note: device and queue are ComPtrs, so they release themselves - don't call Release here.
}

void DX12Compositor::CheckCreateSwapChain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, bool cube)
{
	XrSwapchainCreateInfo& desc = createInfo;

	auto* src = (D3D12TextureData_t*)texture->handle;

	D3D12_RESOURCE_DESC srcDesc;
	srcDesc = src->m_pResource->GetDesc();

	if (bounds) {
		if (std::fabs(bounds->uMax - bounds->uMin) > 0.1)
			srcDesc.Width = uint32_t(float(srcDesc.Width) * std::fabs(bounds->uMax - bounds->uMin));
		if (std::fabs(bounds->vMax - bounds->vMin) > 0.1)
			srcDesc.Height = uint32_t(float(srcDesc.Height) * std::fabs(bounds->vMax - bounds->vMin));
	}

	if (cube) {
		// LibOVR can only use square cubemaps, while SteamVR can use any shape
		// Note we use CopySubresourceRegion later on, so this won't cause problems with that
		auto min = std::min(static_cast<UINT64>(srcDesc.Height), srcDesc.Width);
		srcDesc.Width = min;
		srcDesc.Height = static_cast<UINT>(min);
	}

	bool usable = chain == NULL ? false : CheckChainCompatible(srcDesc, texture->eColorSpace);

	if (!usable) {
		OOVR_LOG("Generating new swap chain");

		if (bounds)
			OOVR_LOGF("Bounds: uMin %f uMax %f vMin %f vMax %f", bounds->uMin, bounds->uMax, bounds->vMin, bounds->vMax);
		OOVR_LOGF("Texture desc format: %d", srcDesc.Format);
		OOVR_LOGF("Texture desc flags:  %d", srcDesc.Flags);
		OOVR_LOGF("Texture desc width:  %d", srcDesc.Width);
		OOVR_LOGF("Texture desc height: %d", srcDesc.Height);

		// First, delete the old chain if necessary
		if (chain) {
			OOVR_FAILED_XR_ABORT(xrDestroySwapchain(chain));
			chain = XR_NULL_HANDLE;
		}

		// Figure out what format we need to use
		DxgiFormatInfo info = {};
		if (!GetFormatInfo(srcDesc.Format, info)) {
			OOVR_ABORTF("Unknown (by OC) DXGI texture format %d", srcDesc.Format);
		}
		bool useLinearFormat;
		switch (texture->eColorSpace) {
		case vr::ColorSpace_Gamma:
			useLinearFormat = false;
			break;
		case vr::ColorSpace_Linear:
			useLinearFormat = true;
			break;
		default:
			// As per the docs for the auto mode, at eight bits per channel or less it assumes gamma
			// (using such small channels for linear colour would result in significant banding)
			useLinearFormat = info.bpc > 8;
			break;
		}

		DXGI_FORMAT type = useLinearFormat ? info.linear : info.srgb;

		if (type == DXGI_FORMAT_UNKNOWN) {
			OOVR_ABORTF("Invalid DXGI target format found: useLinear=%d type=DXGI_FORMAT_UNKNOWN fmt=%d", useLinearFormat, srcDesc.Format);
		}

		// Set aside the old format for checking later
		createInfoFormat = srcDesc.Format;

		// Make eye render buffer
		desc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		// TODO desc.Type = cube ? ovrTexture_Cube : ovrTexture_2D;
		desc.faceCount = cube ? 6 : 1;
		desc.width = static_cast<uint32_t>(srcDesc.Width);
		desc.height = srcDesc.Height;
		desc.format = type;
		desc.mipCount = srcDesc.MipLevels;
		desc.arraySize = 1;
		desc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

		// Match the source's sample count if the runtime can handle it - CopyResource requires both
		// resources to have the same sample count, and a multisampled swapchain is resolved by the
		// runtime itself. If the runtime's maximum is lower, CopyToSwapchain skips the copy entirely
		// (there is no shader resolve pass here).
		desc.sampleCount = std::min(srcDesc.SampleDesc.Count, std::max(xr_main_view(XruEyeLeft).maxSwapchainSampleCount, 1u));

		XrResult result = xrCreateSwapchain(xr_session.get(), &desc, &chain);
		if (!XR_SUCCEEDED(result))
			OOVR_ABORTF("Cannot create DX texture swap chain: err %d", result);

		// Go through the images and retrieve them - this will be used later in Invoke, since OpenXR doesn't
		// have a convenient way to request one specific image.
		uint32_t imageCount;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr));

		imagesHandles = std::vector<XrSwapchainImageD3D12KHR>(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain,
		    imagesHandles.size(), &imageCount, (XrSwapchainImageBaseHeader*)imagesHandles.data()));

		OOVR_FALSE_ABORT(imageCount == imagesHandles.size());

		// Wait for any in-flight work to finish before tearing down the command lists and allocators
		// it's still executing from, otherwise we release them out from underneath the GPU.
		for (size_t i = 0; i < frameFences.size() && i < frameFenceEvents.size(); i++) {
			if (frameFences[i])
				WaitForFence(frameFences[i].Get(), fenceValues[i], frameFenceEvents[i]);
		}

		for (auto event : frameFenceEvents) {
			CloseHandle(event);
		}

		commandLists.resize(imageCount);
		commandAllocators.resize(imageCount);
		frameFenceEvents.resize(imageCount);
		frameFences.resize(imageCount);
		fenceValues.resize(imageCount);

		for (uint32_t i = 0; i < imageCount; i++) {
			frameFenceEvents[i] = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!frameFenceEvents[i])
				OOVR_ABORTF("Failed to create DX12 fence wait event");
			fenceValues[i] = 0;
			OOVR_FAILED_DX_ABORT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
			    IID_PPV_ARGS(&frameFences[i])));

			OOVR_FAILED_DX_ABORT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			    IID_PPV_ARGS(&commandAllocators[i])));
			OOVR_FAILED_DX_ABORT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			    commandAllocators[i].Get(), nullptr,
			    IID_PPV_ARGS(&commandLists[i])));
			OOVR_FAILED_DX_ABORT(commandLists[i]->Close());
		}
		// TODO do we need to release the images at some point, or does the swapchain do that for us?
	}
}

void DX12Compositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	// TODO: support array textures
	D3D12TextureData_t* input = (D3D12TextureData_t*)texture->handle;

	// OpenXR swap chain doesn't support weird formats like DXGI_FORMAT_BC1_TYPELESS
	D3D12_RESOURCE_DESC srcDesc;
	srcDesc = input->m_pResource->GetDesc();
	if (srcDesc.Format == DXGI_FORMAT_BC1_TYPELESS) {
		if (chain) {
			OOVR_FAILED_XR_ABORT(xrDestroySwapchain(chain));
			chain = XR_NULL_HANDLE;
		}
		return;
	}

	// If the runtime can't create a swapchain with this sample count, there's nothing we can copy into
	// (CopyResource requires matching sample counts and there is no resolve pass). Drop the layer this
	// frame rather than submit garbage.
	if (srcDesc.SampleDesc.Count > xr_main_view(XruEyeLeft).maxSwapchainSampleCount) {
		OOVR_LOG_ONCEF("WARNING: MSAA source with sample count x%u exceeds the runtime's maximum of x%u - skipping copy",
		    srcDesc.SampleDesc.Count, xr_main_view(XruEyeLeft).maxSwapchainSampleCount);
		return;
	}

	CheckCreateSwapChain(texture, bounds, false);

	// First reserve an image from the swapchain
	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t currentIndex = 0;
	OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(chain, &acquireInfo, &currentIndex));

	WaitForFence(frameFences[currentIndex].Get(),
	    fenceValues[currentIndex], frameFenceEvents[currentIndex]);

	auto commandList = commandLists[currentIndex].Get();
	OOVR_FAILED_DX_ABORT(commandAllocators[currentIndex]->Reset());
	OOVR_FAILED_DX_ABORT(commandList->Reset(commandAllocators[currentIndex].Get(), nullptr));

	// Wait until the swapchain is ready - this makes sure the compositor isn't writing to it
	// We don't have to pass in currentIndex since it uses the oldest acquired-but-not-waited-on
	// image, so we should be careful with concurrency here.
	// XR_TIMEOUT_EXPIRED is considered successful but swapchain still can't be used, so keep
	// trying until it can be - we're not allowed to fail since the image has been acquired.
	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000; // time out in nano seconds - 500ms
	XrResult res;
	do {
		OOVR_FAILED_XR_ABORT(res = xrWaitSwapchainImage(chain, &waitInfo));
	} while (res == XR_TIMEOUT_EXPIRED);

	if (bounds) {
		// CheckCreateSwapChain sized the swapchain to the bounded sub-rect, so we have to copy just that
		// region rather than the whole resource - CopyResource requires both resources to be identical in
		// size, so using it here would silently fail (or trip the debug layer).
		const UINT srcWidth = static_cast<UINT>(srcDesc.Width);
		const UINT srcHeight = srcDesc.Height;

		// Bounds may be given inverted (vMin > vMax) to request a flip; we can't flip with a copy, so just
		// take the region they cover. The vertical flip is handled by the runtime via the layer's imageRect.
		const float uMin = std::min(bounds->uMin, bounds->uMax);
		const float vMin = std::min(bounds->vMin, bounds->vMax);

		D3D12_BOX sourceRegion;
		sourceRegion.left = std::min(static_cast<UINT>(uMin * (float)srcWidth), srcWidth);
		sourceRegion.top = std::min(static_cast<UINT>(vMin * (float)srcHeight), srcHeight);
		sourceRegion.right = std::min(sourceRegion.left + createInfo.width, srcWidth);
		sourceRegion.bottom = std::min(sourceRegion.top + createInfo.height, srcHeight);
		sourceRegion.front = 0;
		sourceRegion.back = 1;

		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = imagesHandles[currentIndex].texture;
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = input->m_pResource;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLoc.SubresourceIndex = 0;

		commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &sourceRegion);
	} else {
		// No bounds - the swapchain matches the source exactly, so a whole-resource copy is both valid
		// and the fastest option.
		commandList->CopyResource(imagesHandles[currentIndex].texture, input->m_pResource);
	}

	OOVR_FAILED_DX_ABORT(commandList->Close());
	ID3D12CommandList* set[] = { commandList };
	queue->ExecuteCommandLists(1, set);

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(chain, &releaseInfo));

	const auto fenceValue = currentFenceValue;
	OOVR_FAILED_DX_ABORT(queue->Signal(frameFences[currentIndex].Get(), fenceValue));
	fenceValues[currentIndex] = fenceValue;
	++currentFenceValue;
}

void DX12Compositor::InvokeCubemap(const vr::Texture_t* textures)
{
	CheckCreateSwapChain(&textures[0], nullptr, true);

	OOVR_SOFT_ABORT("TODO cubemap");
}

bool DX12Compositor::CheckChainCompatible(D3D12_RESOURCE_DESC& inputDesc, vr::EColorSpace colourSpace)
{
	bool usable = true;
#define FAIL(name)                             \
	do {                                       \
		usable = false;                        \
		OOVR_LOG("Resource mismatch: " #name); \
	} while (0);
#define CHECK(name, chainName)                  \
	if (inputDesc.name != createInfo.chainName) \
		FAIL(name);

	CHECK(Width, width)
	CHECK(Height, height)
	CHECK(MipLevels, mipCount)

	if (inputDesc.Format != createInfoFormat) {
		FAIL("Format");
	}

	// The swapchain is created with the source's sample count when possible - a change in either
	// direction must rebuild it, since CopyResource requires matching sample counts.
	if (inputDesc.SampleDesc.Count != createInfo.sampleCount) {
		FAIL("SampleDesc.Count");
	}
#undef CHECK
#undef FAIL

	return usable;
}

bool DX12Compositor::GetFormatInfo(DXGI_FORMAT format, DX12Compositor::DxgiFormatInfo& out)
{
#define DEF_FMT_BASE(typeless, linear, srgb, bpp, bpc, channels)            \
	{                                                                       \
		out = DxgiFormatInfo{ srgb, linear, typeless, bpp, bpc, channels }; \
		return true;                                                        \
	}

#define DEF_FMT_NOSRGB(name, bpp, bpc, channels) \
	case name##_TYPELESS:                        \
	case name##_UNORM:                           \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

#define DEF_FMT(name, bpp, bpc, channels) \
	case name##_TYPELESS:                 \
	case name##_UNORM:                    \
	case name##_UNORM_SRGB:               \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, name##_UNORM_SRGB, bpp, bpc, channels)

#define DEF_FMT_UNORM(linear, bpp, bpc, channels) \
	case linear:                                  \
		DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, linear, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

	// Note that this *should* have pretty much all the types we'll ever see in games
	// Filtering out the non-typeless and non-unorm/srgb types, this is all we're left with
	// (note that types that are only typeless and don't have unorm/srgb variants are dropped too)
	switch (format) {
		// The relatively traditional 8bpp 32-bit types
		DEF_FMT(DXGI_FORMAT_R8G8B8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8X8, 32, 8, 3)

		// Some larger linear-only types
		DEF_FMT_NOSRGB(DXGI_FORMAT_R16G16B16A16, 64, 16, 4)
		DEF_FMT_NOSRGB(DXGI_FORMAT_R10G10B10A2, 32, 10, 4)

		// A jumble of other weird types
		DEF_FMT_UNORM(DXGI_FORMAT_B5G6R5_UNORM, 16, 5, 3)
		DEF_FMT_UNORM(DXGI_FORMAT_B5G5R5A1_UNORM, 16, 5, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, 32, 10, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_B4G4R4A4_UNORM, 16, 4, 4)
		DEF_FMT(DXGI_FORMAT_BC1, 64, 16, 4)

	default:
		// Unknown type
		return false;
	}

#undef DEF_FMT
#undef DEF_FMT_NOSRGB
#undef DEF_FMT_BASE
#undef DEF_FMT_UNORM
}

#endif
