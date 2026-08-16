//
// Created by ZNix on 25/10/2020.
//

#include "XrBackend.h"
#include "XrGenericTracker.h"
#include "generated/interfaces/vrtypes.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#if defined(SUPPORT_GL) && !defined(_WIN32)
#include <GL/glx.h>
#endif

#include <openxr/openxr_platform.h>

#include <algorithm>

// On Android, the app has to pass the OpenGLES setup data through
#ifdef ANDROID
#include "../OpenOVR/Misc/android_api.h"
#endif

#include "../OpenOVR/Misc/Config.h"

// FIXME find a better way to send the OnPostFrame call?
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "../OpenOVR/Reimpl/BaseOverlay.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/convert.h"
#include "generated/static_bases.gen.h"

#include "generated/interfaces/IVRCompositor_018.h"

#include "tmp_gfx/TemporaryGraphics.h"

#if defined(SUPPORT_VK)
#include "tmp_gfx/TemporaryVk.h"
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include "tmp_gfx/TemporaryD3D11.h"
#endif

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <mutex>

using namespace vr;

std::mutex inputRestartMutex;
std::unique_ptr<TemporaryGraphics> XrBackend::temporaryGraphics = nullptr;
XrBackend::XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx)
{
	memset(projectionViews, 0, sizeof(projectionViews));

	// setup temporaryGraphics

#if defined(SUPPORT_VK)
	if (useVulkanTmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryVk>();
	}
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (!temporaryGraphics && useD3D11TmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryD3D11>();
	}
#endif

	OOVR_FALSE_ABORT(temporaryGraphics);

	// setup the device indexes
	for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
		std::shared_ptr<ITrackedDevice> dev = GetDevice(i);

		if (dev)
			dev->InitialiseDevice(i);
	}
}

XrBackend::~XrBackend()
{
	// First clear out the compositors, since they might try and access the OpenXR instance
	// in their destructor.
	PrepareForSessionShutdown();

	DrvOpenXR::FullShutdown();

	graphicsBinding = nullptr;

	// This must happen after session destruction (which occurs in FullShutdown), as runtimes (namely Monado)
	// may try to access these resources while destroying the session.
	temporaryGraphics.reset();
}

XrSessionState XrBackend::GetSessionState()
{
	return sessionState;
}

std::shared_ptr<IHMD> XrBackend::GetPrimaryHMD()
{
	return hmd;
}

std::shared_ptr<ITrackedDevice> XrBackend::GetDevice(
    vr::TrackedDeviceIndex_t index)
{
	switch (index) {
	case vr::k_unTrackedDeviceIndex_Hmd:
		return GetPrimaryHMD();
	case 1:
		return hand_left;
	case 2:
		return hand_right;
	default: {
		vr::TrackedDeviceIndex_t corrected_index = index - RESERVED_DEVICE_INDICES;

		// Fast path: WaitGetPoses asks for all k_unMaxTrackedDeviceCount indices every frame and nearly
		// all of them are past the end of the tracker list. Reject those without taking the lock or
		// touching a shared_ptr refcount.
		if (corrected_index >= generic_trackers_count.load(std::memory_order_acquire))
			return nullptr;

		std::shared_lock lock(generic_trackers_mutex);
		if (corrected_index >= generic_trackers.size())
			return nullptr;

		return generic_trackers.at(corrected_index);
	}
	}
}

std::shared_ptr<ITrackedDevice> XrBackend::GetDeviceByHand(
    ITrackedDevice::TrackedDeviceType hand)
{
	switch (hand) {
	case ITrackedDevice::HAND_LEFT:
		return hand_left;
	case ITrackedDevice::HAND_RIGHT:
		return hand_right;
	default:
		OOVR_SOFT_ABORTF("Cannot get hand by type '%d'", (int)hand);
		return nullptr;
	}
}

void XrBackend::GetDeviceToAbsoluteTrackingPose(
    vr::ETrackingUniverseOrigin toOrigin,
    float predictedSecondsToPhotonsFromNow,
    vr::TrackedDevicePose_t* poseArray,
    uint32_t poseArrayCount)
{
	// OpenVR asks for poses at "now + predictedSecondsToPhotonsFromNow", where now is the frame's
	// predicted display time. OpenXR has no wall-clock mapping for XrTime, so we express the request
	// as an offset from GetBestTime(): assume the display time sits ~11ms (the compositor latency)
	// ahead of the present the game's prediction is measured from, and shift by the difference.
	//
	// This is a heuristic, not a measurement - runtimes don't expose their true latency through the
	// API. If motion feels wrong, revisit kAssumeDisplayLatencySecs.
	constexpr float kAssumeDisplayLatencySecs = 0.011f; // 11ms
	const float offsetSecs = predictedSecondsToPhotonsFromNow - kAssumeDisplayLatencySecs;

	for (uint32_t i = 0; i < poseArrayCount; ++i) {
		std::shared_ptr<ITrackedDevice> dev = GetDevice(i);
		if (dev) {
			dev->GetPose(toOrigin, &poseArray[i], ETrackingStateType::TrackingStateType_Rendering, offsetSecs);
		} else {
			poseArray[i] = BackendManager::InvalidPose();
		}
	}
}

static void find_queue_family_and_queue_idx(VkDevice dev, VkPhysicalDevice pdev, VkQueue desired_queue, uint32_t& out_queueFamilyIndex, uint32_t& out_queueIndex)
{
	uint32_t queue_family_count;
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, NULL);

	std::vector<VkQueueFamilyProperties> hi(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, hi.data());
	OOVR_LOGF("number of queue families is %d", queue_family_count);

	for (uint32_t i = 0; i < queue_family_count; i++) {
		OOVR_LOGF("queue family %d has %d queues", i, hi[i].queueCount);
		for (uint32_t j = 0; j < hi[i].queueCount; j++) {
			VkQueue tmp;
			vkGetDeviceQueue(dev, i, j, &tmp);
			if (tmp == desired_queue) {
				OOVR_LOGF("Got desired queue: %d %d", i, j);
				out_queueFamilyIndex = i;
				out_queueIndex = j;
				return;
			}
		}
	}
	OOVR_ABORT("Couldn't find the queue family index/queue index of the queue that the OpenVR app gave us!"
	           "This is really odd and really shouldn't ever happen");
}

/* Submitting Frames */
void XrBackend::CheckOrInitCompositors(const vr::Texture_t* tex)
{
	// Check we're using the session with the application's device
	if (!usingApplicationGraphicsAPI) {
		usingApplicationGraphicsAPI = true;

		OOVR_LOG("Recreating OpenXR session for application graphics API");

		// Shutdown old session - apparently Varjo doesn't like the session being destroyed
		// after querying for graphics requirements.
		DrvOpenXR::ShutdownSession();

		switch (tex->eType) {
		case vr::TextureType_DirectX: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			auto* d3dTex = (ID3D11Texture2D*)tex->handle;
			ID3D11Device* dev = nullptr;
			d3dTex->GetDevice(&dev);

			XrGraphicsBindingD3D11KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
			d3dInfo.device = dev;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D11KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

			dev->Release();
#else

 #ifdef __linux__
            OOVR_ABORT("Application is trying to submit a D3D11 texture, this is likely a Steam Proton bug, please report it");
 #else
            OOVR_ABORT("Application is trying to submit a D3D11 texture, which OpenComposite supports but is disabled in this build");
 #endif

#endif
			break;
		}
		case vr::TextureType_DirectX12: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D12KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D12GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			D3D12TextureData_t* d3dTexData = (D3D12TextureData_t*)tex->handle;
			ComPtr<ID3D12Device> device;
			// A null device here would go straight into the graphics binding and fail inside the
			// runtime with a much less useful message.
			OOVR_FAILED_DX_ABORT(d3dTexData->m_pResource->GetDevice(IID_PPV_ARGS(&device)));

			XrGraphicsBindingD3D12KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
			d3dInfo.device = device.Get();
			d3dInfo.queue = d3dTexData->m_pCommandQueue;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D12KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

			// Note: there used to be a D3D12GetDebugInterface/EnableDebugLayer call here under
			// _DEBUG. It was removed for two reasons: D3D12GetDebugInterface fails with
			// DXGI_ERROR_SDK_COMPONENT_MISSING on any machine without the Windows "Graphics Tools"
			// optional feature, and the unchecked result was then null-dereferenced; and it ran
			// after the game had already created its device, so EnableDebugLayer was a no-op even
			// when it succeeded. The debug layer has to be enabled before device creation, which
			// OpenComposite is never in a position to do - use the DirectX Control Panel or
			// d3dconfig to force it on for the game's executable instead.

			// Note: device is a ComPtr, it releases itself - don't call Release here.
#else

 #ifdef __linux__
           OOVR_ABORT("Application is trying to submit a D3D12 texture, this is likely a Steam Proton bug, please report it");
 #else
           OOVR_ABORT("Application is trying to submit a D3D12 texture, which OpenComposite supports but is disabled in this build");
 #endif

#endif
			break;
		}
		case vr::TextureType_Vulkan: {
			const vr::VRVulkanTextureData_t* vktex = (vr::VRVulkanTextureData_t*)tex->handle;

			VkPhysicalDevice xr_desire;
			// Regardless of error checking, we have to call this or we get crazy validation errors.
			xr_ext->xrGetVulkanGraphicsDeviceKHR(xr_instance, xr_system, vktex->m_pInstance, &xr_desire);

			if (xr_desire != vktex->m_pPhysicalDevice) {
				OOVR_ABORTF("The VkPhysicalDevice that the OpenVR app (%p) used is different from the one that the OpenXR runtime used (%p)!\n"
				            "This should never happen, except for on multi-gpu, in which case DRI_PRIME=1 should fix things on Linux.",
				    (void*)vktex->m_pPhysicalDevice, (void*)xr_desire);
			}

			XrGraphicsBindingVulkanKHR binding;
			binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
			binding.next = nullptr;
			binding.instance = vktex->m_pInstance;
			binding.physicalDevice = vktex->m_pPhysicalDevice;
			binding.device = vktex->m_pDevice;

			find_queue_family_and_queue_idx( //
			    binding.device, //
			    binding.physicalDevice, //
			    vktex->m_pQueue, //
			    binding.queueFamilyIndex, //
			    binding.queueIndex //
			);

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingVulkanKHR>>(binding);
			DrvOpenXR::SetupSession();
			break;
		}
		case vr::TextureType_OpenGL: {
#ifdef SUPPORT_GL
			// The spec requires that we call this before starting a session using OpenGL. Unfortunately we
			// can't actually do anything with this information, since the game has already created the context.
			XrGraphicsRequirementsOpenGLKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			// Platform-specific OpenGL context stuff:
#ifdef _WIN32
			XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
			binding.hGLRC = wglGetCurrentContext();
			binding.hDC = wglGetCurrentDC();

			if (!binding.hGLRC || !binding.hDC) {
				OOVR_ABORTF("Null OpenGL GLRC or DC: %p,%p", (void*)binding.hGLRC, (void*)binding.hDC);
			}

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLWin32KHR>>(binding);
			DrvOpenXR::SetupSession();
#else
			// Only support xlib for now (same as Monado)
			// TODO wayland
			// TODO xcb

			// HACK: Create a separate GLX context since `MaybeRestartForInputs()` may attempt to bind it on a different thread while the app's context is still in use
			const PFNGLXCREATECONTEXTATTRIBSARBPROC pfn_glXCreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");
			if (pfn_glXCreateContextAttribsARB == nullptr)
				OOVR_ABORT("glXCreateContextAttribsARB not available");

			XrGraphicsBindingOpenGLXlibKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
			binding.xDisplay = glXGetCurrentDisplay();
			int configCount = 0;
			// clang-format off
			const int configAttribs[] = {
				GLX_CONFIG_CAVEAT, GLX_NONE,
				None,
			};
			// clang-format on
			GLXFBConfig* const configs = glXChooseFBConfig(binding.xDisplay, DefaultScreen(binding.xDisplay), configAttribs, &configCount);
			if (configs == nullptr || configCount < 1)
				OOVR_ABORT("No usable GLXFBConfig found");
			binding.glxFBConfig = configs[0];
			XFree(configs);
			binding.visualid = glXGetVisualFromFBConfig(binding.xDisplay, binding.glxFBConfig)->visualid;
			binding.glxDrawable = glXGetCurrentDrawable();
			// clang-format off
			const int contextAttribs[] = {
				GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
				GLX_CONTEXT_MINOR_VERSION_ARB, 2,
				None,
			};
			// clang-format on
			binding.glxContext = pfn_glXCreateContextAttribsARB(binding.xDisplay, binding.glxFBConfig, glXGetCurrentContext(), true, contextAttribs);

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLXlibKHR>>(binding);
			DrvOpenXR::SetupSession();
#endif
			// End of platform-specific code

#elif defined(SUPPORT_GLES)
			// The spec requires that we call this before starting a session using OpenGL. We could actually handle this properly
			// on android since the app has to be modified to work with us, but for now don't bother.
			XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLESGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			if (!OpenComposite_Android_GLES_Binding_Info)
				OOVR_ABORT("App is trying to use GLES, but OpenComposite_Android_GLES_Binding_Info global is not set.\n"
				           "Please ensure this is set by the application.");

			XrGraphicsBindingOpenGLESAndroidKHR binding = *OpenComposite_Android_GLES_Binding_Info;
			OOVR_FALSE_ABORT(binding.type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR);
			binding.next = nullptr;

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLESAndroidKHR>>(binding);
			DrvOpenXR::SetupSession();

#else
			OOVR_ABORT("Application is trying to submit an OpenGL texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		default:
			OOVR_ABORTF("Invalid/unknown texture type %d", tex->eType);
		}

		// Real graphics binding should be setup now - get rid of temporary graphics
		temporaryGraphics.reset();
	}

	for (std::unique_ptr<Compositor>& compositor : compositors) {
		// Skip a compositor if it's already set up
		if (compositor)
			continue;

		compositor = BaseCompositor::CreateCompositorAPI(tex);
	}
}

void XrBackend::ReportFrameTimings()
{
	// frameTimings/frameStartTime are written by the render thread, but GetFrameTiming can read
	// them from any thread a game chooses - keep this safe under the same session lock the rest of
	// the frame lifecycle uses.
	auto lock = xr_session.lock_shared();

	const float threshold = oovr_global_configuration.HitchWarningMs();
	const uint32_t summaryInterval = oovr_global_configuration.FrameTimingSummaryFrames();

	// frameStartTime is default-constructed until the first frame begins - nothing to report yet.
	if (frameStartTime == std::chrono::steady_clock::time_point{} || (threshold <= 0.0f && summaryInterval == 0)) {
		frameTimings = {};
		return;
	}

	const double totalMs = std::chrono::duration<double, std::milli>(
	    std::chrono::steady_clock::now() - frameStartTime)
	                           .count();

	if (summaryInterval > 0) {
		if (framesSummarised == 0) {
			summaryStartTime = frameStartTime;
			frameTotalMinMs = totalMs;
			frameTotalMaxMs = totalMs;
		} else {
			frameTotalMinMs = std::min(frameTotalMinMs, totalMs);
			frameTotalMaxMs = std::max(frameTotalMaxMs, totalMs);
		}

		frameTimingSum.waitFrameMs += frameTimings.waitFrameMs;
		frameTimingSum.beginFrameMs += frameTimings.beginFrameMs;
		frameTimingSum.locateViewsMs += frameTimings.locateViewsMs;
		frameTimingSum.compositorMs += frameTimings.compositorMs;
		frameTimingSum.endFrameMs += frameTimings.endFrameMs;
		frameTotalSumMs += totalMs;
		framesSummarised++;

		if (framesSummarised >= summaryInterval)
			LogFrameTimingSummary();
	}

	if (threshold > 0.0f && totalMs >= threshold) {
		const FrameTimings& t = frameTimings;

		// A run where *every* frame busts the threshold would otherwise write a line per frame for
		// the whole session. Log every hitch while there are few enough to read individually, then
		// thin out - the pattern is established long before the hundredth line.
		hitchCount++;
		constexpr uint32_t logEveryHitchUntil = 200;
		constexpr uint32_t thinnedInterval = 100;
		if (hitchCount > logEveryHitchUntil && (hitchCount % thinnedInterval) != 0) {
			frameTimings = {};
			return;
		}

		// Whatever isn't in one of the phases below was spent in the game's own rendering, between
		// its WaitGetPoses and its Submit calls. Reporting it explicitly is what makes this log
		// line able to answer "is it us or is it them".
		const double accounted = t.waitFrameMs + t.beginFrameMs + t.locateViewsMs + t.compositorMs + t.endFrameMs;
		const double gameMs = totalMs - accounted;

		OOVR_LOGF("HITCH frame %u: %.1fms total | xrWaitFrame %.1f | xrBeginFrame %.1f | locateViews %.1f | compositor %.1f | xrEndFrame %.1f | game %.1f (hitch #%u)",
		    nFrameIndex, totalMs, t.waitFrameMs, t.beginFrameMs, t.locateViewsMs, t.compositorMs, t.endFrameMs, gameMs,
		    hitchCount);
	}

	frameTimings = {};
}

void XrBackend::LogFrameTimingSummary()
{
	if (framesSummarised == 0)
		return;

	const double n = (double)framesSummarised;
	const double avgTotal = frameTotalSumMs / n;
	const double avgWait = frameTimingSum.waitFrameMs / n;
	const double avgBegin = frameTimingSum.beginFrameMs / n;
	const double avgLocate = frameTimingSum.locateViewsMs / n;
	const double avgComp = frameTimingSum.compositorMs / n;
	const double avgEnd = frameTimingSum.endFrameMs / n;

	// Whatever isn't in one of our phases was spent in the game's own rendering. Note this is wall
	// time between our SubmitFrames returning and the next WaitGetPoses, so it includes the game's
	// GPU work - a starved GPU lands here too.
	const double avgGame = avgTotal - (avgWait + avgBegin + avgLocate + avgComp + avgEnd);

	// Measured over wall time rather than 1000/avgTotal, so periods where the game stopped calling
	// WaitGetPoses entirely (track loads) are reflected honestly.
	const double windowMs = std::chrono::duration<double, std::milli>(
	    std::chrono::steady_clock::now() - summaryStartTime)
	                            .count();
	const double fps = windowMs > 0.0 ? (n * 1000.0) / windowMs : 0.0;

	// The ratio of our frame rate to the display's is the single most useful number here. A value
	// near 0.5 means the app is running at exactly half refresh - which is what a runtime-side
	// half-rate lock (Virtual Desktop's Synchronous SpaceWarp, SteamVR's motion smoothing) looks
	// like, and is quite different from simply being slow, which gives a ragged ratio.
	const float displayHz = xr_gbl ? xr_gbl->GetDisplayFrequency(0.0f) : 0.0f;
	const double refreshRatio = displayHz > 0.0f ? fps / displayHz : 0.0;

	// Frames that were measured but never reached xrEndFrame (the session was inactive, or the
	// runtime refused the frame) don't advance nFrameIndex, so the window can cover more frames
	// than the counter moved - don't let the subtraction wrap.
	const uint32_t windowStart = nFrameIndex >= framesSummarised ? nFrameIndex - framesSummarised : 0;

	OOVR_LOGF("FRAMES %u-%u: %.1f fps of %.1fHz (%.2fx) | avg %.2fms (min %.2f max %.2f) | xrWaitFrame %.2f | xrBeginFrame %.2f | locateViews %.2f | compositor %.2f | xrEndFrame %.2f | game %.2f",
	    windowStart, nFrameIndex, fps, displayHz, refreshRatio,
	    avgTotal, frameTotalMinMs, frameTotalMaxMs,
	    avgWait, avgBegin, avgLocate, avgComp, avgEnd, avgGame);

	frameTimingSum = {};
	frameTotalSumMs = 0.0;
	frameTotalMinMs = 0.0;
	frameTotalMaxMs = 0.0;
	framesSummarised = 0;
}

void XrBackend::WaitForTrackingData()
{
	// Events may make the session active/inactive or trigger a restart (e.g. a graphics-API switch).
	// Handle them before the state checks, or a session that was just recreated would only start
	// rendering on the next WaitGetPoses call instead of this one.
	PumpEvents();

	// If the game calls WaitGetPoses twice in a row, the frame from the first call is still open and
	// we can't xrWaitFrame again. Re-report the poses already located for the open frame.
	if (renderingFrame)
		return;

	// Make sure the OpenXR session is active before doing anything else, and if not then skip
	if (!sessionActive) {
		renderingFrame = false;

		// Drop any half-measured frame instead of leaving the clock running across the gap. A
		// session can sit inactive for many seconds (headset taken off, session being rebuilt for
		// the game's graphics API), and measuring that as one enormous frame would report a hitch
		// that never happened.
		frameTimings = {};
		frameStartTime = {};

		// Same reasoning for the rolling average - its fps figure is frames over wall time, so an
		// inactive gap inside the window would read as a huge framerate drop that didn't happen.
		frameTimingSum = {};
		frameTotalSumMs = 0.0;
		framesSummarised = 0;
		return;
	}

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state{ XR_TYPE_FRAME_STATE };

	// This is the top of a new frame, so anything still in the accumulator belongs to the previous
	// one. Report it now rather than at submit time, so frames the game abandons midway (it stops
	// calling Submit when it's between sessions) still get accounted for.
	ReportFrameTimings();
	frameStartTime = std::chrono::steady_clock::now();

	{
		auto lock = xr_session.lock_shared();

		auto waitStart = std::chrono::steady_clock::now();
		XrResult waitResult = xrWaitFrame(xr_session.get(), &waitInfo, &state);
		// A session that's mid-teardown (headset off, restart in progress) reports these instead of
		// crashing - treat them as "no frame this time" and let the next WaitGetPoses try again.
		if (waitResult == XR_ERROR_SESSION_NOT_RUNNING || waitResult == XR_ERROR_CALL_ORDER_INVALID) {
			OOVR_LOGF("WaitGetPoses: xrWaitFrame failed (XrResult %d) - skipping frame", waitResult);
			frameTimings = {};
			frameStartTime = {};
			renderingFrame = false;
			return;
		}
		OOVR_FAILED_XR_ABORT(waitResult);
		auto waitEnd = std::chrono::steady_clock::now();
		frameTimings.waitFrameMs = std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();

		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;
		xr_gbl->predictedDisplayPeriod = state.predictedDisplayPeriod;

		// FIXME loop until this returns true?
		// OOVR_FALSE_ABORT(state.shouldRender);

		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		XrResult beginResult = xrBeginFrame(xr_session.get(), &beginInfo);
		if (beginResult == XR_ERROR_SESSION_NOT_RUNNING || beginResult == XR_ERROR_CALL_ORDER_INVALID) {
			OOVR_LOGF("WaitGetPoses: xrBeginFrame failed (XrResult %d) - skipping frame", beginResult);
			frameTimings = {};
			frameStartTime = {};
			renderingFrame = false;
			return;
		}
		OOVR_FAILED_XR_ABORT(beginResult);
		frameTimings.beginFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitEnd).count();
	}

	auto locateStart = std::chrono::steady_clock::now();
	xr_gbl->ClearCachedViews();

	XrSpace projectionSpace = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	const XruCachedViews& cachedViews = xr_gbl->GetCachedViews(projectionSpace);
	frameTimings.locateViewsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - locateStart).count();

	const XrViewState& viewState = cachedViews.viewState;
	const std::array<XrView, XruEyeCount>& views = cachedViews.views;

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = views[eye].fov;

		XrPosef pose = views[eye].pose;

		// Make sure we at least have halfway-sane values if the runtime isn't providing them. In particular
		// if the runtime gives us an invalid orientation, that'd otherwise cause XR_ERROR_POSE_INVALID errors later.
		if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		}
		if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
			// About 1.75m up above the origin
			pose.position = XrVector3f{ 0, 1.75, 0 };
		}

		projectionViews[eye].pose = pose;
	}

	// If we're not on the game's graphics API yet, don't actually mark us as having started the frame.
	// Instead, set a different flag so we'll call this method again when it's available.
	if (!usingApplicationGraphicsAPI) {
		deferredRenderingStart = true;
	} else {
		renderingFrame = true;
	}
}

void XrBackend::StoreEyeTexture(
    vr::EVREye eye,
    const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds,
    vr::EVRSubmitFlags submitFlags,
    bool isFirstEye)
{
	CheckOrInitCompositors(texture);

	XrCompositionLayerProjectionView& layer = projectionViews[eye];
	layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

	std::unique_ptr<Compositor>& compPtr = compositors[eye];
	OOVR_FALSE_ABORT(compPtr.get() != nullptr);
	Compositor& comp = *compPtr;

	// If the session is inactive, we may be unable to write to the surface
	if (sessionActive && renderingFrame) {
		auto compStart = std::chrono::steady_clock::now();
		comp.Invoke(texture, bounds, layer.subImage, (XruEye)eye, submitFlags);
		frameTimings.compositorMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compStart).count();
	}

	submittedEyeTextures = true;

	// TODO store view somewhere and use it for submitting our frame

	// If WaitGetPoses was called before the first texture was submitted, we're in a kinda weird state
	// The application will expect it can submit it's frames (and we do too) however xrBeginFrame was
	// never called for this session - it was called for the early session, then when the first texture
	// was published we switched to that, but this new session hasn't had xrBeginFrame called yet.
	// To get around this, we set a flag if we should begin a frame but are still in the early session. At
	// this point we can check for that flag and call xrBeginFrame a second time, on the right session.
	if (deferredRenderingStart && usingApplicationGraphicsAPI) {
		deferredRenderingStart = false;
		WaitForTrackingData();
	}
}

void XrBackend::SubmitFrames(bool showSkybox, bool postPresent)
{
	// Always pump events, even if the session isn't active - this is what makes the session active
	// in the first place.
	PumpEvents();

	// If we are getting calls from PostPresentHandOff then skip the calls from other functions as
	//  there will be other data such as GUI layers to be added before ending the frame.
	bool skipRender = postPresentStatus && !postPresent;
	postPresentStatus = postPresent;

	if (!renderingFrame || skipRender)
		return;

	// All data submitted, rendering has finished, frame can be ended.
	renderingFrame = false;

	// Make sure the OpenXR session is active before doing anything else
	// Note that if the session becomes ready after WaitGetTrackingPoses was called, then
	// renderingFrame will still be false so this won't be a problem in that case.
	if (!sessionActive)
		return;

	XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
	info.environmentBlendMode = environmentBlendMode;
	info.displayTime = xr_gbl->nextPredictedFrameTime;

	XrCompositionLayerBaseHeader const* const* headers = nullptr;
	XrCompositionLayerBaseHeader* app_layer = nullptr;

	int layer_count = 0;

	// Apps can use layers to provide GUIs and loading screens where a 3D environment is not being rendered.
	// Only create the projection layer if we have a 3D environment to submit.
	XrCompositionLayerProjection mainLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	if (submittedEyeTextures) {
		// We have eye textures so setup a projection layer
		mainLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		mainLayer.views = projectionViews;
		mainLayer.viewCount = 2;

		app_layer = (XrCompositionLayerBaseHeader*)&mainLayer;
		for (uint32_t i = 0; i < mainLayer.viewCount; ++i) {
			XrCompositionLayerProjectionView& layer = projectionViews[i];
			layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			// The swapchain may have been rebuilt between the two eye submits (e.g. a resolution change
			// or MSAA switch), which would leave a stale handle in the subImage - refresh it each frame.
			if (compositors[i])
				layer.subImage.swapchain = compositors[i]->GetSwapChain();
			if (layer.subImage.swapchain == XR_NULL_HANDLE)
				app_layer = nullptr;
		}
		submittedEyeTextures = false;
	}

	// If we have an overlay then add
	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	if (overlay) {
		layer_count = overlay->_BuildLayers(app_layer, headers);
	} else if (app_layer) {
		layer_count = 1;
		headers = &app_layer;
	}

	// It's ok if no layers have been added at this point,
	// it will just cause the display to be blanked
	info.layers = headers;
	info.layerCount = layer_count;

	auto endFrameStart = std::chrono::steady_clock::now();
	OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));
	frameTimings.endFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - endFrameStart).count();

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		sys->_OnPostFrame();
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	frameSubmitTimeUs = (double)std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1000000.0;

	nFrameIndex++;
}

IBackend::openvr_enum_t XrBackend::SetSkyboxOverride(const vr::Texture_t* pTextures, uint32_t unTextureCount)
{
	// Needed for rFactor2 loading screens
	if (unTextureCount && pTextures) {
		CheckOrInitCompositors(pTextures);

		if (!sessionActive || !usingApplicationGraphicsAPI)
			return 0;

		// If a frame is already open, we can't begin another until it's been ended - the in-flight
		// frame will be submitted by SubmitFrames as normal, so just skip this call.
		if (renderingFrame) {
			OOVR_LOG_ONCE("SetSkyboxOverride: frame already in flight, skipping");
			return 0;
		}

		// Make sure any unfinished frames don't call xrEndFrame after this call
		renderingFrame = false;

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState state{ XR_TYPE_FRAME_STATE };

		XrResult waitResult = xrWaitFrame(xr_session.get(), &waitInfo, &state);
		if (waitResult == XR_ERROR_SESSION_NOT_RUNNING || waitResult == XR_ERROR_CALL_ORDER_INVALID) {
			OOVR_LOGF("SetSkyboxOverride: xrWaitFrame failed (XrResult %d) - skipping", waitResult);
			return 0;
		}
		OOVR_FAILED_XR_ABORT(waitResult);
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;

		// This submits a frame when a skybox override is set. This is designed around rFactor2 where the skybox is used as
		// a loading screen and is frequently updated, and most other games probably behave in a similar manner. It'd be
		// ideal to run a separate thread while the skybox override is set to submit frames if IVRCompositor->Submit is not
		// being called frequently enough, and that'd need to be carefully synchronised with the main submit thread. That's
		// not yet implemented since it's not currently worth the hassle, but if someone in the future wants to do it:
		// TODO submit skybox frames in their own thread.
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		XrResult beginResult = xrBeginFrame(xr_session.get(), &beginInfo);
		if (beginResult == XR_ERROR_SESSION_NOT_RUNNING || beginResult == XR_ERROR_CALL_ORDER_INVALID) {
			OOVR_LOGF("SetSkyboxOverride: xrBeginFrame failed (XrResult %d) - skipping", beginResult);
			return 0;
		}
		OOVR_FAILED_XR_ABORT(beginResult);

		if (skybox_compositor == nullptr)
			skybox_compositor = BaseCompositor::CreateCompositorAPI(pTextures);

		vr::VRTextureBounds_t bounds;
		bounds.uMin = 0.0;
		bounds.uMax = 1.0;
		bounds.vMin = 1.0;
		bounds.vMax = 0.0;

		XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		skybox_compositor->Invoke(pTextures, &bounds, layerQuad.subImage);
		layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		layerQuad.next = NULL;
		layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
			{ 0.0f, 0.0f, -0.65f } };
		layerQuad.size = { 1.0f, 1.0f / 1.333f };

		XrCompositionLayerBaseHeader* layers[1];
		layers[0] = (XrCompositionLayerBaseHeader*)&layerQuad;
		XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
		info.environmentBlendMode = environmentBlendMode;
		info.displayTime = xr_gbl->nextPredictedFrameTime;
		info.layers = layers;
		info.layerCount = 1;

		OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));

	} else {
		OOVR_SOFT_ABORT("Unsupported texture count");
	}

	return 0;
}

void XrBackend::ClearSkyboxOverride()
{
	OOVR_SOFT_ABORT("No implementation");
}

/* Misc compositor */

/**
 * Get frame timing information to be passed to the application
 *
 * Returns true if successful
 */
bool XrBackend::GetFrameTiming(OOVR_Compositor_FrameTiming* pTiming, uint32_t unFramesAgo)
{
	// m_nSize comes straight from the application and is used as a memset length below, so it has to be
	// sanity-checked in both directions:
	//
	//  - Below sizeof(m_nSize) the subtraction underflows into a gigantic memset.
	//  - Above any plausible struct size it's garbage (uninitialised, or a struct we don't know), and
	//    memsetting that far past the caller's buffer is an access violation inside the CRT. F1 25 dies
	//    here otherwise.
	//
	// The bound is deliberately generous rather than sizeof(OOVR_Compositor_FrameTiming): newer versions
	// of vr::Compositor_FrameTiming legitimately exceed it (they append m_nNumVSyncsReadyForUse and
	// m_nNumVSyncsToFirstView), and we want those callers to keep working.
	constexpr uint32_t maxPlausibleTimingSize = 512; // real structs are ~200 bytes

	// A null pointer is simply a broken call - don't dereference it before we even reach the bounds check.
	if (!pTiming) {
		OOVR_LOG_ONCE("WARNING: Application called GetFrameTiming with a null pointer");
		return false;
	}

	// We only keep data for the most recent frame. OpenVR documents the behaviour for this case as
	// "Sets oldest timing info if nFramesAgo is larger than the stored history" - i.e. clamp, not
	// fail - so hand back the single record we have.
	//
	// This previously returned false. F1 25 asks for 5 frames ago and so got nothing at all, which
	// is not what the API promises.
	if (unFramesAgo > 0) {
		OOVR_LOG_ONCEF("Application requested frame timing %u frames ago; only the latest frame is kept, returning that", unFramesAgo);
	}

	if (pTiming->m_nSize < sizeof(pTiming->m_nSize) || pTiming->m_nSize > maxPlausibleTimingSize) {
		OOVR_LOG_ONCEF("WARNING: Application passed implausible Compositor_FrameTiming size %u (expected %zu..%u), ignoring",
		    pTiming->m_nSize, sizeof(pTiming->m_nSize), maxPlausibleTimingSize);
		return false;
	}

	OOVR_LOG_ONCEF("Compositor_FrameTiming size from app: %u (our struct is %zu)",
	    pTiming->m_nSize, sizeof(OOVR_Compositor_FrameTiming));

	// Zero everything except the size field
	memset(reinterpret_cast<unsigned char*>(pTiming) + sizeof(pTiming->m_nSize), 0, pTiming->m_nSize - sizeof(pTiming->m_nSize));

	if (pTiming->m_nSize >= sizeof(IVRCompositor_018::Compositor_FrameTiming)) {
		pTiming->m_flSystemTimeInSeconds = frameSubmitTimeUs;
		pTiming->m_nFrameIndex = nFrameIndex;

		// A lot of these values we can't get the data for so just use sensible values
		pTiming->m_nNumFramePresents = 1; // number of times this frame was presented
		pTiming->m_nNumMisPresented = 0; // number of times this frame was presented on a vsync other than it was originally predicted to
		pTiming->m_nNumDroppedFrames = 0; // number of additional times previous frame was scanned out
		pTiming->m_nReprojectionFlags = 0;

		// Just use sensible values until GPU timers implemented
		pTiming->m_flPreSubmitGpuMs = 8.0f;
		pTiming->m_flPostSubmitGpuMs = 1.0f;
		pTiming->m_flTotalRenderGpuMs = 9.0f;

		// Use very conservative guesses for these. They are used in F1 22 for dynamic resolution calculations but are not something that is provided
		// through OpenXR. Using conservative values will give a bit more headroom for the game to target realistic frame times.
		pTiming->m_flCompositorRenderGpuMs = 1.5f; // time spend performing distortion correction, rendering chaperone, overlays, etc.
		pTiming->m_flCompositorRenderCpuMs = 3.0f; // time spent on cpu submitting the above work for this frame

		pTiming->m_flCompositorIdleCpuMs = 0.1f;

		/** Miscellaneous measured intervals. */
		// Time between calls to WaitGetPoses. Derived from the runtime's display period rather
		// than the old hardcoded 11.1ms (i.e. 90Hz), which was wrong at every other refresh rate.
		pTiming->m_flClientFrameIntervalMs = xr_gbl && xr_gbl->predictedDisplayPeriod > 0
		    ? (float)(xr_gbl->predictedDisplayPeriod / 1.0e6)
		    : 11.1f;
		pTiming->m_flPresentCallCpuMs = 0.0f; // time blocked on call to present (usually 0.0, but can go long)
		pTiming->m_flWaitForPresentCpuMs = 0.0f; // time spent spin-waiting for frame index to change (not near-zero indicates wait object failure)
		pTiming->m_flSubmitFrameMs = 0.0f; // time spent in IVRCompositor::Submit (not near-zero indicates driver issue)

		/** The following are all relative to this frame's SystemTimeInSeconds */
		pTiming->m_flWaitGetPosesCalledMs = 0.0f;
		pTiming->m_flNewPosesReadyMs = 0.0f;
		pTiming->m_flNewFrameReadyMs = 0.0f; // second call to IVRCompositor::Submit
		pTiming->m_flCompositorUpdateStartMs = 0.0f;
		pTiming->m_flCompositorUpdateEndMs = 0.0f;
		pTiming->m_flCompositorRenderStartMs = 0.0f;

		GetPrimaryHMD()->GetPose(vr::ETrackingUniverseOrigin::TrackingUniverseSeated, &pTiming->m_HmdPose, ETrackingStateType::TrackingStateType_Rendering);

		return true;
	}

	return false;
}

/* D3D Mirror textures */
/* #if defined(SUPPORT_DX) */
IBackend::openvr_enum_t XrBackend::GetMirrorTextureD3D11(vr::EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
	return 0;
}
void XrBackend::ReleaseMirrorTextureD3D11(void* pD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
}
/* #endif */
/** Returns the points of the Play Area. */
bool XrBackend::GetPlayAreaPoints(vr::HmdVector3_t* points, int* count)
{
	if (count)
		*count = 0;

	XrExtent2Df bounds;
	XrResult res = xrGetReferenceSpaceBoundsRect(xr_session.get(), XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);

	if (res == XR_SPACE_BOUNDS_UNAVAILABLE)
		return false;

	OOVR_FAILED_XR_ABORT(res);

	if (count)
		*count = 4;

	// The origin of the free space is centred around the player
	// TODO if we're using the Oculus runtime, grab it's native handle and get the full polygon
	if (points) {
		points[0] = vr::HmdVector3_t{ -bounds.width / 2, 0, -bounds.height / 2 };
		points[1] = vr::HmdVector3_t{ bounds.width / 2, 0, -bounds.height / 2 };
		points[2] = vr::HmdVector3_t{ bounds.width / 2, 0, bounds.height / 2 };
		points[3] = vr::HmdVector3_t{ -bounds.width / 2, 0, bounds.height / 2 };
	}

	return true;
}
/** Determine whether the bounds are showing right now **/
bool XrBackend::AreBoundsVisible()
{
	OOVR_SOFT_ABORT("No implementation");
	return false;
}
/** Set the boundaries to be visible or not (although setting this to false shouldn't affect
 * what happens if the player moves their hands too close and shows it that way) **/
void XrBackend::ForceBoundsVisible(bool status)
{
	OOVR_SOFT_ABORT("No implementation");
}

bool XrBackend::IsInputAvailable()
{
	return sessionState == XR_SESSION_STATE_FOCUSED;
}

void XrBackend::PumpEvents()
{
	// Poll for OpenXR events
	// TODO filter by session?
	while (true) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		XrResult res;
		OOVR_FAILED_XR_ABORT(res = xrPollEvent(xr_instance, &ev));

		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}

		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto* changed = (XrEventDataSessionStateChanged*)&ev;
			OOVR_FALSE_ABORT(changed->session == xr_session.get());
			sessionState = changed->state;

			// Monado bug: it returns 0 for this value (at least for the first two states)
			// Make sure this is actually greater than 0, otherwise this will mess up xr_gbl->GetBestTime()
			if (changed->time > 0 && xr_gbl)
				xr_gbl->latestTime = changed->time;

			OOVR_LOGF("Switch to OpenXR state %d", sessionState);

			switch (sessionState) {
			case XR_SESSION_STATE_READY: {
				OOVR_LOG("Hit ready state, begin session...");
				// Start the session running - this means we're supposed to start submitting frames
				XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				OOVR_FAILED_XR_ABORT(xrBeginSession(xr_session.get(), &beginInfo));
				sessionActive = true;
				break;
			}
			case XR_SESSION_STATE_STOPPING: {
				// End the session. The session is still valid and we can still query some information
				// from it, but we're not allowed to submit frames anymore. This is done when the engagement
				// sensor detects the user has taken off the headset, for example.
				if (sessionActive) // could be the case if we missed XR_SESSION_STATE_READY for some reason
					OOVR_FAILED_XR_ABORT(xrEndSession(xr_session.get()));
				sessionActive = false;
				renderingFrame = false;
				break;
			}
			case XR_SESSION_STATE_EXITING: {
				OOVR_LOGF("Exiting");
				break;
			}
			case XR_SESSION_STATE_LOSS_PENDING: {
				// If the headset is unplugged or the user decides to exit the app
				// TODO just kill the app after awhile, unless it sends a message to stop that - read the OpenVR wiki docs for more info
				VREvent_t quit = { VREvent_Quit };
				auto system = GetBaseSystem();
				if (system)
					system->_EnqueueEvent(quit);
				break;
			}
			default:
				// suppress clion warning about missing branches
				break;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			UpdateInteractionProfile();
		}

	} // while loop

	/*
	   We check for AreActionsLoaded here because:
	   1. Games using legacy input call xrSyncActions every frame anyway, so the runtime should
	      give us an interaction profile without us forcing it
	   2. Games using an action manifest should be calling UpdateActionState every frame, which calls xrSyncActions.
	      This means the only games we wouldn't be able to confidently grab an interaction profile from
	      would be ones where an action manifest is loaded but UpdateActionState is not being called
	      (because the game checks IsTrackedDeviceConnected or something),
	      and hopefully no game like that exists.
	   Some runtimes (WMR) do not instantly return an interaction profile,
	   so we will keep tryinig to query it until it does.

	   Note that we check that the session is focused because this means that the application
	   has already submitted a frame, that frame is visible, and we have input focus.
	   Waiting until the application has input focus allows us to avoid unnecessarily restarting the
	   session when we can't even receive input anyway, as well as before the session is restarted for
	   the temporary session.
   */
	BaseInput* input = GetUnsafeBaseInput();
	if (input && !input->AreActionsLoaded() && sessionState == XR_SESSION_STATE_FOCUSED && !hand_left && !hand_right) {
		QueryForInteractionProfile();
	}
}

void XrBackend::OnSessionCreated()
{
	sessionState = XR_SESSION_STATE_UNKNOWN;
	sessionActive = false;
	renderingFrame = false;

	// Cache the runtime's supported blend modes - pass-through/alpha-blend environments are used by
	// AR-style runtimes, and submitting the wrong mode is an XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED.
	// Prefer OPAQUE (the only mode the compositor knows it renders correctly), falling back to the
	// runtime's first choice if it can't do opaque.
	uint32_t blendModeCount = 0;
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateEnvironmentBlendModes(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blendModeCount, nullptr));
	if (blendModeCount > 0) {
		std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
		OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateEnvironmentBlendModes(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blendModeCount, &blendModeCount, blendModes.data()));
		auto opaque = std::find(blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
		environmentBlendMode = opaque != blendModes.end() ? *opaque : blendModes[0];
		OOVR_LOGF("Environment blend modes supported by runtime: %d, using %d", blendModeCount, environmentBlendMode);
	} else {
		OOVR_LOG_ONCE("WARNING: Runtime reported no supported environment blend modes, assuming OPAQUE");
		environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	}

	PumpEvents();

	// Wait until we transition to the idle state.
	// This sets the time, so OpenXR calls which use that will work correctly.
	while (sessionState == XR_SESSION_STATE_UNKNOWN) {
		const int durationMs = 250;

		OOVR_LOGF("No session transition yet received, waiting %dms ...", durationMs);

#ifdef _WIN32
		Sleep(durationMs);
#else
		struct timespec ts = { 0, durationMs * 1000000 };
		nanosleep(&ts, &ts);
#endif

		PumpEvents();
	}
}

void XrBackend::PrepareForSessionShutdown()
{
	for (std::unique_ptr<Compositor>& c : compositors) {
		c.reset();
	}
	skybox_compositor.reset();
	overlay_compositors.clear();

	// The hand trackers belong to the session that's about to go away - destroy them now, while
	// that's still legal, rather than leaving dangling handles behind for the next session.
	if (BaseInput* input = GetUnsafeBaseInput())
		input->ReleaseSessionHandles();

	if (infoSet != XR_NULL_HANDLE) {
		OOVR_FAILED_XR_ABORT(xrDestroyActionSet(infoSet));
		infoSet = XR_NULL_HANDLE;
		infoAction = XR_NULL_HANDLE;
	}
}

// On Android, add an event poll function for use while sleeping
#ifdef ANDROID
void OpenComposite_Android_EventPoll()
{
	BackendManager::Instance().PumpEvents();
}
#endif

bool XrBackend::IsGraphicsConfigured()
{
	return usingApplicationGraphicsAPI;
}

void XrBackend::OnOverlayTexture(const vr::Texture_t* texture)
{
	if (!usingApplicationGraphicsAPI)
		CheckOrInitCompositors(texture);
}

void XrBackend::RegisterOverlayCompositor(std::shared_ptr<Compositor> compositor)
{
	overlay_compositors.push_back(compositor);
}

void XrBackend::UnregisterOverlayCompositor(std::shared_ptr<Compositor> compositor)
{
	std::erase_if(overlay_compositors, [c = std::move(compositor)](std::shared_ptr<Compositor>& comp) {
		return c == comp;
	});
}

void XrBackend::UpdateInteractionProfile()
{
	struct hand_info {
		const char* pathstr;
		std::shared_ptr<XrController>& controller;
		const XrController::XrControllerType hand;
	};

	hand_info hands[] = {
		{ .pathstr = "/user/hand/left", .controller = hand_left, .hand = XrController::XCT_LEFT },
		{ .pathstr = "/user/hand/right", .controller = hand_right, .hand = XrController::XCT_RIGHT }
	};

	for (hand_info& info : hands) {
		XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
		XrPath path;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, info.pathstr, &path));
		OOVR_FAILED_XR_ABORT(xrGetCurrentInteractionProfile(xr_session.get(), path, &state));

		// interaction profile detected
		if (state.interactionProfile != XR_NULL_PATH) {
			uint32_t tmp;
			char path_name[XR_MAX_PATH_LENGTH];
			OOVR_FAILED_XR_ABORT(xrPathToString(xr_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &tmp, path_name));

			const InteractionProfile* matched = nullptr;
			for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
				if (profile->GetPath() == path_name) {
					matched = profile.get();
					break;
				}
			}

			if (!matched) {
				// Don't kill the game over hardware we don't have a profile for. Every conformant runtime
				// supports the simple controller, so fall back to that - the user gets reduced functionality
				// on unrecognised controllers instead of an immediate crash.
				matched = InteractionProfile::GetProfileByPath("/interaction_profiles/khr/simple_controller");
				OOVR_LOGF("WARNING: %s - runtime returned unknown interaction profile '%s', falling back to the simple controller",
				    info.pathstr, path_name);
			} else {
				OOVR_LOGF("%s - Using interaction profile: %s", info.pathstr, path_name);
			}

			info.controller = std::make_shared<XrController>(info.hand, *matched);
			hmd->SetInteractionProfile(matched);
			BaseSystem* system = GetUnsafeBaseSystem();
			if (system) {
				VREvent_t event = {
					.eventType = VREvent_TrackedDeviceActivated,
					.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
				};
				system->_EnqueueEvent(event);
				event = {
					.eventType = VREvent_TrackedDeviceUpdated,
					.trackedDeviceIndex = 0
				};
				system->_EnqueueEvent(event);
			}
		} else {
			// interaction profile lost/not detected
			OOVR_LOGF("%s - No interaction profile detected", info.pathstr);
			if (info.controller) {
				info.controller.reset();
				BaseSystem* system = GetUnsafeBaseSystem();
				if (system) {
					VREvent_t event = {
						.eventType = VREvent_TrackedDeviceDeactivated,
						.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
					};
					system->_EnqueueEvent(event);
				}
			}
		}
	}

	CreateGenericTrackers();
}

void XrBackend::CreateGenericTrackers()
{
	if (!xr_ext->xrMndxXdevSpace_Available())
		return;

	std::unique_lock lock(generic_trackers_mutex);

	OOVR_LOGF("Checking for generic trackers...");

	// should get cleared every time this function is run to avoid stale trackers
	generic_trackers.clear();
	generic_trackers_count.store(0, std::memory_order_release);

	// list containing all generic tracked devices
	XrXDevListMNDX generic_tracker_list;
	std::vector<XrXDevIdMNDX> generic_tracker_ids(MAX_GENERIC_TRACKERS);
	InteractionProfile* profile = InteractionProfile::GetProfileByPath("/interaction_profiles/htc/vive_tracker_htcx");

	uint32_t xdev_count = 0;

	XrCreateXDevListInfoMNDX create_info = {
		.type = XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX
	};

	XrXDevPropertiesMNDX cur_properties = {
		.type = XR_TYPE_XDEV_PROPERTIES_MNDX,
	};

	XrGetXDevInfoMNDX cur_info = {
		.type = XR_TYPE_GET_XDEV_INFO_MNDX,
	};

	OOVR_FAILED_XR_ABORT(xr_ext->xrCreateXDevListMNDX(xr_session.get(), &create_info, &generic_tracker_list));
	OOVR_FAILED_XR_ABORT(xr_ext->xrEnumerateXDevsMNDX(generic_tracker_list, MAX_GENERIC_TRACKERS, &xdev_count, generic_tracker_ids.data()));

	// The runtime only wrote the first xdev_count entries - drop the rest, so the filter below can be a
	// pure predicate rather than tracking the valid count via a side effect and relying on erase_if
	// visiting elements in order.
	generic_tracker_ids.resize(std::min<uint32_t>(xdev_count, MAX_GENERIC_TRACKERS));

	std::vector<std::string> forced_tracker_serials{};
	{
		auto forced_tracker_serials_str = GetEnv("OPENCOMPOSITE_TRACKER_SERIALS");
		if (!forced_tracker_serials_str.empty()) {
			size_t separator_pos{ std::string::npos };
			while ((separator_pos = forced_tracker_serials_str.find(';')) != std::string::npos) {
				auto serial = forced_tracker_serials_str.substr(0, separator_pos);
				forced_tracker_serials.push_back(serial);
				forced_tracker_serials_str.erase(0, separator_pos + 1);
			}
			if (!forced_tracker_serials_str.empty()) {
				forced_tracker_serials.push_back(forced_tracker_serials_str);
			}
		}
	}

	// filter out non-tracker devices
	std::erase_if(generic_tracker_ids, [&](XrXDevIdMNDX id) {
		cur_info.id = id;

		// This runs during session restarts too, when the session may already be gone - a failed
		// property query must not kill the process mid-teardown, just drop the device.
		XrResult propsResult = xr_ext->xrGetXDevPropertiesMNDX(generic_tracker_list, &cur_info, &cur_properties);
		if (propsResult != XR_SUCCESS) {
			OOVR_LOG_ONCEF("WARNING: xrGetXDevPropertiesMNDX failed (XrResult %d) - dropping tracker", propsResult);
			return true;
		}

		std::string name = cur_properties.name;
		std::string serial = cur_properties.serial;

		if (!cur_properties.canCreateSpace)
			return true;

		OOVR_LOGF("Found usable xdev '%s', serial '%s'", name.c_str(), serial.c_str());

		if (forced_tracker_serials.size() > 0 && std::find(forced_tracker_serials.cbegin(), forced_tracker_serials.cend(), serial) != forced_tracker_serials.cend()) {
			return false;
		}
		return name.find("Tracker") == std::string::npos;
	});

	OOVR_LOGF("Found %zu generic trackers", generic_tracker_ids.size());

	OOVR_LOG("Creating generic trackers...");
	for (const XrXDevIdMNDX id : generic_tracker_ids) {
		int index = generic_trackers.size();

		if (index >= MAX_GENERIC_TRACKERS) {
			OOVR_LOGF("More trackers present than allowed (%d), skipping..", MAX_GENERIC_TRACKERS);
			break;
		}

		cur_info.id = id;

		XrResult propsResult = xr_ext->xrGetXDevPropertiesMNDX(generic_tracker_list, &cur_info, &cur_properties);
		if (propsResult != XR_SUCCESS) {
			OOVR_LOG_ONCEF("WARNING: xrGetXDevPropertiesMNDX failed (XrResult %d) - skipping tracker", propsResult);
			continue;
		}

		std::string serial = cur_properties.serial;
		std::string name = cur_properties.name;

		OOVR_LOGF("Generic Tracker: %s, Serial: %s, Index: %d, ID: %" PRIu64, cur_properties.name, cur_properties.serial, index, id);

		// create space
		XrSpace space;
		XrPosef pose = { .orientation = { 0, 0, 0, 1 }, .position = { 0, 0, 0 } };
		XrCreateXDevSpaceInfoMNDX create_space_info = {
			.type = XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX,
			.next = NULL,
			.xdevList = generic_tracker_list,
			.id = id,
			.offset = pose,
		};

		OOVR_FAILED_XR_ABORT(xr_ext->xrCreateXDevSpaceMNDX(xr_session.get(), &create_space_info, &space));

		generic_trackers.push_back(std::make_unique<XrGenericTracker>(*profile, cur_properties, index, space));
		generic_trackers_count.store(generic_trackers.size(), std::memory_order_release);

		BaseSystem* system = GetUnsafeBaseSystem();
		if (system) {
			VREvent_t event = {
				.eventType = VREvent_TrackedDeviceActivated,
				.trackedDeviceIndex = (TrackedDeviceIndex_t)generic_trackers.at(index)->DeviceIndex()
			};
			system->_EnqueueEvent(event);
		}
	}

	// cleanup
	xr_ext->xrDestroyXDevListMNDX(generic_tracker_list);
}

void XrBackend::MaybeRestartForInputs()
{
	// if we haven't attached any actions to the session (infoSet or game actions), no need to restart
	const std::lock_guard<std::mutex> lock(inputRestartMutex);
	BaseInput* input = GetUnsafeBaseInput();
	if (infoSet == XR_NULL_HANDLE && (!input || !input->AreActionsLoaded()))
		return;

	OOVR_LOG("Restarting session for inputs...");
	DrvOpenXR::SetupSession();
	OOVR_LOG("Session restart successful!");
}

void XrBackend::QueryForInteractionProfile()
{
	// Note that we want to avoid using BaseInput here because it would allow for games to call GetControllerState before rendering
	// and then we'd have to recreate the session twice, once for the input state and once for when the game submits a frame
	const std::lock_guard<std::mutex> lock(inputRestartMutex);
	if (subactionPaths[0] == XR_NULL_PATH) {
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/left", &subactionPaths[0]));
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/right", &subactionPaths[1]));
	}

	if (infoSet == XR_NULL_HANDLE) {
		OOVR_LOG("Creating infoset");
		CreateInfoSet();
		BindInfoSet();
	}

	// Interaction profiles are updated after xrSyncActions, so we'll try to make the runtime give us one by calling xrSyncActions.
	XrActiveActionSet active[2] = {
		{ .actionSet = infoSet, .subactionPath = subactionPaths[0] },
		{ .actionSet = infoSet, .subactionPath = subactionPaths[1] },
	};

	XrActionsSyncInfo info{ XR_TYPE_ACTIONS_SYNC_INFO };
	info.countActiveActionSets = 2;
	info.activeActionSets = active;

	OOVR_FAILED_XR_ABORT(xrSyncActions(xr_session.get(), &info));
}

void XrBackend::CreateInfoSet()
{
	XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "opencomposite-internal-info-set");
	strcpy_s(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "OpenComposite internal info set");
	OOVR_FAILED_XR_ABORT(xrCreateActionSet(xr_instance, &set_info, &infoSet));

	XrActionCreateInfo act_info{ XR_TYPE_ACTION_CREATE_INFO };
	strcpy_s(act_info.actionName, XR_MAX_ACTION_NAME_SIZE, "opencomposite-internal-info-act");
	strcpy_s(act_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "OpenComposite internal info action");
	act_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	act_info.countSubactionPaths = std::size(subactionPaths);
	act_info.subactionPaths = subactionPaths;
	OOVR_FAILED_XR_ABORT(xrCreateAction(infoSet, &act_info, &infoAction));
}

void XrBackend::BindInfoSet()
{
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		XrPath interactionProfilePath;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, profile->GetPath().c_str(), &interactionProfilePath));

		if (!profile->CanHaveBindings())
			continue;

		XrInteractionProfileSuggestedBinding suggestedBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };

		std::vector<XrActionSuggestedBinding> bindings;
		using namespace std::string_literals;
		// grabs the first found paths ending in /click for each subaction path
		const auto& validPaths = profile->GetValidInputPaths();
		for (const std::string& path_name : { "/user/hand/left"s, "/user/hand/right"s }) {
			auto click_path = std::ranges::find_if(validPaths,
			    [&path_name](const std::string& s) -> bool {
				    return s.find("/click") != s.npos && s.find(path_name) != s.npos;
			    });

			// Not every profile has a /click path for every hand - skip rather than dereferencing end()
			if (click_path == validPaths.end()) {
				OOVR_LOGF("No /click input path for %s on profile %s, skipping info binding",
				    path_name.c_str(), profile->GetPath().c_str());
				continue;
			}

			XrPath path;
			OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, click_path->c_str(), &path));
			bindings.push_back({ .action = infoAction, .binding = path });
		}

		if (bindings.empty())
			continue;

		suggestedBindings.interactionProfile = interactionProfilePath;
		suggestedBindings.suggestedBindings = bindings.data();
		suggestedBindings.countSuggestedBindings = bindings.size();

		OOVR_FAILED_XR_ABORT(xrSuggestInteractionProfileBindings(xr_instance, &suggestedBindings));
	}

	// Attach the info set by itself. We will have to restart the session once the game attaches its real inputs.
	XrSessionActionSetsAttachInfo info{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	info.countActionSets = 1;
	info.actionSets = &infoSet;
	OOVR_FAILED_XR_ABORT(xrAttachSessionActionSets(xr_session.get(), &info));
}

const void* XrBackend::GetCurrentGraphicsBinding()
{
	if (graphicsBinding) {
		return graphicsBinding->asVoid();
	}
	OOVR_FALSE_ABORT(temporaryGraphics);
	return temporaryGraphics->GetGraphicsBinding();
}

#ifdef SUPPORT_VK
void XrBackend::VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out)
{
	*out = VK_NULL_HANDLE;

	TemporaryVk* vk = temporaryGraphics->GetAsVk();
	if (vk == nullptr)
		OOVR_ABORT("Not using temporary Vulkan instance");

	// Find the UUID of the physical device the temporary instance is running on
	VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProps };
	vkGetPhysicalDeviceProperties2(vk->physicalDevice, &props);

	// Look through all the physical devices on the target instance and find the matching one
	uint32_t devCount;
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, nullptr));
	std::vector<VkPhysicalDevice> physicalDevices(devCount);
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, physicalDevices.data()));

	for (VkPhysicalDevice phy : physicalDevices) {
		VkPhysicalDeviceIDProperties devIdProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 devProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &devIdProps };
		vkGetPhysicalDeviceProperties2(phy, &devProps);

		if (memcmp(devIdProps.deviceUUID, idProps.deviceUUID, sizeof(devIdProps.deviceUUID)) != 0)
			continue;

		// Found it
		*out = phy;
		return;
	}

	OOVR_ABORT("Could not find matching Vulkan physical device for instance");
}

#endif
