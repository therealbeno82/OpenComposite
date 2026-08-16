#include "stdafx.h"

#include "../Misc/Config.h"
#include "compositor.h"

Compositor::~Compositor()
{
	if (chain) {
		OOVR_FAILED_XR_SOFT_ABORT(xrDestroySwapchain(chain));
		chain = XR_NULL_HANDLE;
	}
}

void Compositor::Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, XrSwapchainSubImage& subImage, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	// Only take the in-compositor path if this backend can actually flip during the copy. DX12
	// can't - it has no shader blit - and taking the bounds without flipping them produced a
	// silently upside-down image.
	bool invertInCompositor = oovr_global_configuration.InvertUsingShaders() && SupportsShaderInvert();
	if (bounds && bounds->vMin == 0.0f && bounds->vMax == 1.0f && bounds->uMin == 0.0f && bounds->uMax == 1.0f)
		bounds = nullptr;
	CopyToSwapchain(texture, invertInCompositor ? bounds : nullptr, eye, submitFlags);
	subImage.swapchain = GetSwapChain();
	subImage.imageArrayIndex = 0; // This is *not* the swapchain index
	XrExtent2Di src = GetSrcSize();

	// supportsInvert says "the copy that just happened already flipped the image", not "this
	// backend could in principle". Passing an unconditional true meant an inverted submission
	// (vMin > vMax) fell through to the offset/extent maths below and produced a *negative*
	// extent.height, which is not a legal XrRect2Di. Contrary to the comments this replaced,
	// OpenXR has no way to express a flip in imageRect, so the honest handling is to normalise
	// the rect and say so.
	bool submittedFlipped = CalculateViewport(invertInCompositor ? nullptr : bounds, src.width, src.height, invertInCompositor, subImage.imageRect);
	if (submittedFlipped) {
		OOVR_LOG_ONCE("WARNING: game submitted vertically-flipped bounds and this backend cannot flip during the copy - "
		              "the image will be upside down. Try invertUsingShaders=true if the backend supports it.");
	}
}

bool Compositor::CalculateViewport(const vr::VRTextureBounds_t* ptrBounds, int32_t width, int32_t height, bool supportsInvert, XrRect2Di& viewport)
{
	bool submitVerticallyFlipped = false;

	if (ptrBounds) {
		vr::VRTextureBounds_t newBounds = *ptrBounds;
		if (!supportsInvert && newBounds.vMin > newBounds.vMax) {
			float newMax = newBounds.vMin;
			newBounds.vMin = newBounds.vMax;
			newBounds.vMax = newMax;
			submitVerticallyFlipped = true;
		} else {
			submitVerticallyFlipped = false;
		}

		viewport.offset.x = (int)(newBounds.uMin * (float)width);
		viewport.offset.y = (int)(newBounds.vMin * (float)height);
		viewport.extent.width = (int)((newBounds.uMax - newBounds.uMin) * (float)width);
		viewport.extent.height = (int)((newBounds.vMax - newBounds.vMin) * (float)height);
	} else {
		viewport.offset.x = viewport.offset.y = 0;
		viewport.extent.width = width;
		viewport.extent.height = height;
		submitVerticallyFlipped = false;
	}
	return submitVerticallyFlipped;
}
