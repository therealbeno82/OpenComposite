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
		// If this fails the event is never signalled and the wait below would hang the game's
		// render thread forever, so don't wait at all - a torn frame beats a lockup.
		HRESULT hr = fence->SetEventOnCompletion(completionValue, waitEvent);
		if (FAILED(hr)) {
			OOVR_LOGF("WARNING: SetEventOnCompletion failed (hr=0x%08X) - skipping GPU wait", hr);
			return;
		}

		// A finite timeout for the same reason: a GPU hang or TDR shouldn't take the game with it.
		// Five seconds is far beyond any legitimate frame.
		if (WaitForSingleObject(waitEvent, 5000) == WAIT_TIMEOUT)
			OOVR_LOG_ONCE("WARNING: timed out waiting for the GPU to finish a swapchain copy");
	}
}

/**
 * The OpenXR runtime hands us colour swapchain images in D3D12_RESOURCE_STATE_RENDER_TARGET (we
 * request XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) and expects them back in that state when
 * xrReleaseSwapchainImage is called. Copying into one without transitioning it is a debug-layer
 * error and formally undefined - D3D12's implicit state promotion only applies from COMMON.
 *
 * Note the *source* texture is deliberately left alone: OpenVR does not specify what state a game
 * hands its texture over in, and naming the wrong StateBefore would be worse than naming none.
 */
void TransitionImage(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (before == after)
		return;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	list->ResourceBarrier(1, &barrier);
}
} // namespace

DX12Compositor::DX12Compositor(D3D12TextureData_t* td)
{
	queue = td->m_pCommandQueue;
	// Unchecked, a failure here leaves device null and the next CreateFence call dereferences it.
	OOVR_FAILED_DX_ABORT(queue->GetDevice(IID_PPV_ARGS(&device)));

	// Hardcoded to 0 in CreateCommandAllocator/CreateCommandList below. Fine for a single GPU, but
	// silently wrong on a linked multi-adapter setup - say so rather than rendering to nowhere.
	if (td->m_nNodeMask != 0)
		OOVR_LOGF("WARNING: game submitted a D3D12 texture with node mask %u; multi-adapter is not supported", td->m_nNodeMask);
}

DX12Compositor::~DX12Compositor()
{
	// The base destructor destroys the swapchain after us, and the runtime may free the D3D12
	// resources backing its images as soon as that happens - wait for any copies still in flight
	// before we close the fence events, or the queue could execute them after the images are gone.
	for (size_t i = 0; i < frameFences.size() && i < frameFenceEvents.size(); i++) {
		if (frameFences[i])
			WaitForFence(frameFences[i].Get(), fenceValues[i], frameFenceEvents[i]);
	}

	for (auto event : frameFenceEvents) {
		CloseHandle(event);
	}
	frameFenceEvents.clear();

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

		const uint32_t runtimeMaxSampleCount = std::max(xr_main_view(XruEyeLeft).maxSwapchainSampleCount, 1u);
		// If the runtime can't create a multisampled swapchain, the copy path resolves the source into
		// a single-sample chain instead - which requires the chain's format to match the source exactly.
		const bool resolvingMSAA = srcDesc.SampleDesc.Count > 1 && srcDesc.SampleDesc.Count > runtimeMaxSampleCount;

		// Wait for any in-flight work to finish before destroying the old chain: the runtime may free
		// the D3D12 resources backing its images on xrDestroySwapchain, while our command lists could
		// still be executing copies against them. This must happen *before* the destroy.
		for (size_t i = 0; i < frameFences.size() && i < frameFenceEvents.size(); i++) {
			if (frameFences[i])
				WaitForFence(frameFences[i].Get(), fenceValues[i], frameFenceEvents[i]);
		}

		for (auto event : frameFenceEvents) {
			CloseHandle(event);
		}
		// Don't leave closed handles in the vector - the loop below happens to overwrite every
		// live index, but a double-close is one refactor away otherwise.
		frameFenceEvents.clear();

		// Then delete the old chain
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

		DXGI_FORMAT type;
		if (resolvingMSAA) {
			// ResolveSubresource requires identical source and destination formats, so the swapchain
			// for a resolved MSAA source must keep the raw source format rather than the
			// sRGB/linear-converted variant.
			type = srcDesc.Format;
			if (!useLinearFormat && info.srgb != DXGI_FORMAT_UNKNOWN && srcDesc.Format != info.srgb) {
				OOVR_LOG_ONCEF("WARNING: resolving MSAA into a chain with the raw source format %d, so the sRGB "
				               "conversion to %d is skipped - expect a washed-out image",
				    srcDesc.Format, info.srgb);
			}
		} else {
			type = useLinearFormat ? info.linear : info.srgb;
		}

		if (type == DXGI_FORMAT_UNKNOWN) {
			OOVR_ABORTF("Invalid DXGI target format found: useLinear=%d type=DXGI_FORMAT_UNKNOWN fmt=%d", useLinearFormat, srcDesc.Format);
		}

		// Set aside the old format and colour space for checking later
		createInfoFormat = srcDesc.Format;
		createInfoColourSpace = texture->eColorSpace;

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
		// runtime itself. If the runtime's maximum is lower, create a single-sample chain and resolve
		// the source into it with ResolveSubresource on each frame instead.
		desc.sampleCount = resolvingMSAA ? 1u : std::min(srcDesc.SampleDesc.Count, runtimeMaxSampleCount);

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

		// How deep the runtime's chain is decides how far ahead of the GPU we can run: the fence
		// wait in CopyToSwapchain blocks until the copy from imageCount frames ago has finished.
		// A count of 2 against a game that pipelines 2-3 frames deep means a real CPU stall.
		OOVR_LOGF("Swapchain image count: %u (sample count %u)", imageCount, desc.sampleCount);

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

	const uint32_t runtimeMaxSampleCount = std::max(xr_main_view(XruEyeLeft).maxSwapchainSampleCount, 1u);

	// If the runtime can't create a multisampled swapchain, CheckCreateSwapChain builds a
	// single-sample chain and we resolve the source into it with ResolveSubresource. That's a
	// whole-resource operation, so a bounded (inverted) submission can't be resolved - drop the
	// layer rather than submit garbage.
	const bool resolvingMSAA = srcDesc.SampleDesc.Count > 1 && srcDesc.SampleDesc.Count > runtimeMaxSampleCount;
	if (resolvingMSAA && bounds) {
		OOVR_LOG_ONCEF("WARNING: MSAA source with sample count x%u exceeds the runtime's maximum of x%u and has bounds - skipping copy",
		    srcDesc.SampleDesc.Count, runtimeMaxSampleCount);
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

	ID3D12Resource* const dest = imagesHandles[currentIndex].texture;
	const D3D12_RESOURCE_STATES destWriteState = resolvingMSAA
	    ? D3D12_RESOURCE_STATE_RESOLVE_DEST
	    : D3D12_RESOURCE_STATE_COPY_DEST;

	TransitionImage(commandList, dest, D3D12_RESOURCE_STATE_RENDER_TARGET, destWriteState);

	// SteamVR lets an app submit one array texture and select the slice by eye rather than handing
	// over a separate texture per eye - see the equivalent handling in DX11's CopyToSwapchain.
	// Without this both eyes get slice 0, i.e. the left eye's image twice.
	const UINT arraySlice = (srcDesc.DepthOrArraySize > 1 && eye.has_value()) ? static_cast<UINT>(*eye) : 0u;
	// D3D12 subresource indices are MipSlice + ArraySlice * MipLevels; we only ever touch mip 0.
	const UINT srcSubresource = arraySlice * std::max<UINT>(srcDesc.MipLevels, 1u);

	D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
	dstLoc.pResource = dest;
	dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dstLoc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
	srcLoc.pResource = input->m_pResource;
	srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcLoc.SubresourceIndex = srcSubresource;

	if (resolvingMSAA) {
		// The chain was created single-sample with the source's exact format, so a straight resolve
		// is valid (the runtime resolves for us only when it supports multisampled swapchains).
		commandList->ResolveSubresource(dest, 0, input->m_pResource, srcSubresource, srcDesc.Format);
	} else if (bounds) {
		// CheckCreateSwapChain sized the swapchain to the bounded sub-rect, so we have to copy just that
		// region rather than the whole resource - CopyResource requires both resources to be identical in
		// size, so using it here would silently fail (or trip the debug layer).
		const UINT srcWidth = static_cast<UINT>(srcDesc.Width);
		const UINT srcHeight = srcDesc.Height;

		// Bounds may be given inverted (vMin > vMax) to request a flip; we can't flip with a copy, so
		// just take the region they cover. Note this path only runs with invertUsingShaders=true,
		// and DX12 reports SupportsShaderInvert() == false, so in practice Invoke never sends us
		// bounds - the runtime cannot flip via imageRect either, whatever this comment used to say.
		const float uMin = std::min(bounds->uMin, bounds->uMax);
		const float vMin = std::min(bounds->vMin, bounds->vMax);

		D3D12_BOX sourceRegion;
		sourceRegion.left = std::min(static_cast<UINT>(uMin * (float)srcWidth), srcWidth);
		sourceRegion.top = std::min(static_cast<UINT>(vMin * (float)srcHeight), srcHeight);
		sourceRegion.right = std::min(sourceRegion.left + createInfo.width, srcWidth);
		sourceRegion.bottom = std::min(sourceRegion.top + createInfo.height, srcHeight);
		sourceRegion.front = 0;
		sourceRegion.back = 1;

		// Bounds near the far edge clamp left and right (or top and bottom) onto each other. An
		// empty box is invalid for CopyTextureRegion - the debug layer errors and some drivers
		// remove the device - so skip the copy and leave the image as it is.
		if (sourceRegion.right <= sourceRegion.left || sourceRegion.bottom <= sourceRegion.top) {
			OOVR_LOG_ONCEF("WARNING: bounds produced an empty copy region (u %f..%f, v %f..%f) - skipping copy",
			    bounds->uMin, bounds->uMax, bounds->vMin, bounds->vMax);
		} else {
			commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &sourceRegion);
		}
	} else if (srcDesc.DepthOrArraySize > 1) {
		// CopyResource requires the two resources to be identical, and an array source against our
		// single-slice swapchain isn't - copy just the slice this eye wants. Note this tests the
		// source being an array rather than the slice being non-zero: the left eye takes slice 0,
		// which is still an array-to-single-slice copy.
		//
		// Only mip 0 is copied. The swapchain is created with the source's mip count, so a mipped
		// array source leaves the smaller mips unwritten - no worse than the bounds path above, and
		// nothing samples them.
		commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
	} else {
		// No bounds - the swapchain matches the source exactly, so a whole-resource copy is both valid
		// and the fastest option.
		commandList->CopyResource(dest, input->m_pResource);
	}

	// Hand the image back in the state the runtime expects to find it in.
	TransitionImage(commandList, dest, destWriteState, D3D12_RESOURCE_STATE_RENDER_TARGET);

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

	// The chain's format is picked from the colour space as well as the source format, so this has
	// to force a rebuild too - it used to be an unused parameter.
	if (colourSpace != createInfoColourSpace) {
		FAIL("ColorSpace");
	}

	// The swapchain is created with the source's sample count when possible - a change in either
	// direction must rebuild it, since CopyResource requires matching sample counts. A multisampled
	// source against a single-sample chain is fine though: that's the resolve path.
	if (inputDesc.SampleDesc.Count != createInfo.sampleCount && !(createInfo.sampleCount == 1 && inputDesc.SampleDesc.Count > 1)) {
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

#define DEF_FMT_FLOAT(name, bpp, bpc, channels) \
	case name##_TYPELESS:                       \
	case name##_FLOAT:                          \
		DEF_FMT_BASE(name##_TYPELESS, name##_FLOAT, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

#define DEF_FMT_FLOAT_ONLY(name, bpp, bpc, channels) \
	case name##_FLOAT:                              \
		DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, name##_FLOAT, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

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

		// HDR float types (linear only) - DX12 games doing HDR pipelines submit these directly.
		// R16G16B16A16's typeless variant is already covered by DEF_FMT_NOSRGB above.
		DEF_FMT_FLOAT(DXGI_FORMAT_R32G32B32A32, 128, 32, 4)
		DEF_FMT_FLOAT(DXGI_FORMAT_R32G32B32, 96, 32, 3)
		DEF_FMT_FLOAT_ONLY(DXGI_FORMAT_R16G16B16A16, 64, 16, 4)
		// R11G11B10 has no typeless variant
	case DXGI_FORMAT_R11G11B10_FLOAT:
		DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_UNKNOWN, 32, 11, 3)

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
#undef DEF_FMT_FLOAT
#undef DEF_FMT_FLOAT_ONLY
#undef DEF_FMT_BASE
#undef DEF_FMT_UNORM
}

#endif
