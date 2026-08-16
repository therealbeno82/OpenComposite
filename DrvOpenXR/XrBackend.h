//
// Created by ZNix on 25/10/2020.
//

#pragma once

#include "XrDriverPrivate.h"

#include "XrController.h"
#include "XrHMD.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

class XrGenericTracker;

class XrBackend : public IBackend {
public:
	DECLARE_BACKEND_FUNCS(virtual, override)

	XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx);
	~XrBackend() override;

	/**
	 * The current state of the OpenXR session, or XR_SESSION_STATE_UNKNOWN if there is no session.
	 */
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;

	XrSessionState GetSessionState();

	/**
	 * Whether the session is active or not. This cannot be determined from just the session state, since
	 * we're allowed to send frames after calling xrBeginSession but before the event comes through. Same
	 * with ending the session - we're allowed to submit frames after we receive the stopping event, until
	 * we call xrEndSession.
	 */
	bool sessionActive = false;

	/**
	 * To be called after xrCreateSession. Should only be used by DrvOpenXR.
	 */
	void OnSessionCreated();

	void PrepareForSessionShutdown();

	const void* GetCurrentGraphicsBinding();

	void RegisterOverlayCompositor(std::shared_ptr<Compositor> compositor);
	void UnregisterOverlayCompositor(std::shared_ptr<Compositor> compositor);

	/**
	 * Creates the per-eye compositors for the game's graphics API, restarting the session first if it
	 * was started with a temporary binding. Called from BaseCompositor::Submit (before the session
	 * shared lock is taken - restarting takes the exclusive lock) and from StoreEyeTexture.
	 */
	void CheckOrInitCompositors(const vr::Texture_t* tex);

	/**
	 * Restarts the session to allow for inputs to be attached to the session, if necessary.
	 * To be called from BaseInput whenever it's attempting to attach the game actions.
	 * Restarting the session should only be necessary if we've already created the infoSet.
	 */
	static void MaybeRestartForInputs();

#ifdef SUPPORT_VK
	static void VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out);
#endif

private:
	std::shared_ptr<XrHMD> hmd = std::make_shared<XrHMD>();
	std::shared_ptr<XrController> hand_left;
	std::shared_ptr<XrController> hand_right;

	std::vector<std::shared_ptr<XrGenericTracker>> generic_trackers;
	std::shared_mutex generic_trackers_mutex;

	// Mirrors generic_trackers.size(), maintained under generic_trackers_mutex but readable without it.
	// WaitGetPoses walks all k_unMaxTrackedDeviceCount indices every frame, and almost all of them are
	// past the end of this list - this lets GetDevice reject them without taking the lock at all.
	std::atomic<size_t> generic_trackers_count{ 0 };

	std::unique_ptr<Compositor> compositors[XruEyeCount];
	std::unique_ptr<Compositor> skybox_compositor;
	std::vector<std::shared_ptr<Compositor>> overlay_compositors;

	/**
	 * Updates the current interaction profile in use according to the runtime.
	 * This will set the XrHMD's interaction profile, as well as create the XrControllers
	 * and set their interaction profiles accordingly.
	 * This allows for games to retrieve correct per-controller OpenVR properties that they request.
	 * Called from PumpEvents on an INTERACTION_PROFILE_CHANGED event.
	 */
	void UpdateInteractionProfile();

	/**
	 * Queries xdevs from MNDX_xdev_space, if the runtime supports it. Ignores HMD & Controllers.
	 * These "Generic Trackers" act like an HTC vive tracker, but support no bindings.
	 */
	void CreateGenericTrackers();

	/**
	 * Attempts to force the runtime to expose an interaction profile
	 * (i.e., send an INTERACTION_PROFILE_CHANGED event).
	 * If this is called before the game's actions have been attached to the session by BaseInput
	 * (which is the case for essentially all legacy input games), the session will have to be restarted
	 * to attach the actual inputs. BaseInput handles this when attaching its inputs.
	 */
	void QueryForInteractionProfile();
	void CreateInfoSet();
	void BindInfoSet();

	// Whether we've restarted the session to use the application's rendering API yet
	bool usingApplicationGraphicsAPI = false;

	// The views for the two main eye layers
	XrCompositionLayerProjectionView projectionViews[XruEyeCount];

	// Have we started rendering a frame yet? If not, calling xrEndFrame would result in an error
	bool renderingFrame = false;

	// Were we supposed to start rendering a frame, but couldn't since we were on the
	// early (pre switch to application graphics instance) OpenXR session?
	bool deferredRenderingStart = false;

	// Keep track of if eye textures have been submitted and if we need to create a projection layer for them
	bool submittedEyeTextures = false;

	// If the app is using PostPresentHandoff then we need to delay when we submit frame data through xrEndFrame
	// until after all frame and layer data has been submitted and PostPresentHandoff is called. Otherwise we
	// might miss overlay elements for GUI or HUDs
	bool postPresentStatus = false;

	// Number of frames rendered for use in frame timing data
	uint32_t nFrameIndex = 0;

	double frameSubmitTimeUs = 0.0;

	/**
	 * The blend mode to use for xrEndFrame, cached at session setup from the runtime's capabilities.
	 * Defaults to OPAQUE - the only mode that is guaranteed to be supported everywhere.
	 */
	XrEnvironmentBlendMode environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	// --- Frame hitch watchdog -------------------------------------------------------------------
	//
	// A frame is spread over three calls from the game (WaitGetPoses, Submit per eye, and the
	// PostPresentHandoff/Submit that ends it), so the per-phase times are accumulated here and
	// only reported once the frame is complete.
	//
	// Nothing is logged for a frame that came in under Config::HitchWarningMs, which makes this
	// cheap enough to leave enabled during normal play - unlike logAllOpenVRCalls, which writes a
	// flushed line per OpenVR call.
	struct FrameTimings {
		double waitFrameMs = 0.0; // xrWaitFrame - blocks until the runtime wants the next frame
		double beginFrameMs = 0.0; // xrBeginFrame
		double locateViewsMs = 0.0; // xrLocateViews for the projection views
		double compositorMs = 0.0; // swapchain acquire/wait/copy/release, both eyes
		double endFrameMs = 0.0; // xrEndFrame - hands the layers to the runtime
	};
	FrameTimings frameTimings;
	std::chrono::steady_clock::time_point frameStartTime;
	uint32_t hitchCount = 0;

	// --- Rolling frame-timing summary -----------------------------------------------------------
	//
	// The watchdog above only fires on frames that bust the threshold, so the steady-state frame -
	// the one that actually sets your framerate - never appears in the log. These accumulate every
	// frame and get averaged into a single line every Config::FrameTimingSummaryFrames() frames.
	//
	// This is what distinguishes "the runtime is pacing us" (time in xrWaitFrame) from "our copy
	// path is slow" (time in compositor) from "the game's own rendering is slow" (the remainder).
	FrameTimings frameTimingSum;
	double frameTotalSumMs = 0.0;
	double frameTotalMinMs = 0.0;
	double frameTotalMaxMs = 0.0;
	uint32_t framesSummarised = 0;
	std::chrono::steady_clock::time_point summaryStartTime;

	/**
	 * Log a one-line breakdown if the frame that just ended busted the hitch threshold, fold it
	 * into the rolling summary, then reset the accumulator ready for the next frame.
	 */
	void ReportFrameTimings();

	/**
	 * Emit the rolling average line and start a fresh window. Called by ReportFrameTimings once
	 * enough frames have accumulated; assumes the session lock is already held.
	 */
	void LogFrameTimingSummary();

	// Action set and action used for querying for the interaction profile
	inline static XrActionSet infoSet = XR_NULL_HANDLE;
	XrAction infoAction = XR_NULL_HANDLE;
	XrPath subactionPaths[2] = { XR_NULL_PATH, XR_NULL_PATH };

	// Abstract class for holding a graphics binding.
	class BindingBase {
	public:
		virtual const void* asVoid() = 0;
		virtual ~BindingBase() = default;
	};

	// A wrapper around a graphics binding type, subclassing BindingBase
	// It is templated to allow storing any of the possible graphics bindings and getting a void* to it,
	// as necessary for xrCreateSession (in DrvOpenXR::SetupSession)
	template <typename T>
	class BindingWrapper : public BindingBase {
		const T data;

	public:
		BindingWrapper(T data)
		    : data(data) {}
		~BindingWrapper() override
		{
#if defined(SUPPORT_GL) && !defined(_WIN32)
			if constexpr (std::is_same_v<T, struct XrGraphicsBindingOpenGLXlibKHR>)
				glXDestroyContext(data.xDisplay, data.glxContext);
#endif
		}
		const void* asVoid() override { return &data; }
	};

	// The current graphics binding, used for restarting the session
	inline static std::unique_ptr<BindingBase> graphicsBinding = nullptr;
	static std::unique_ptr<class TemporaryGraphics> temporaryGraphics;
};
