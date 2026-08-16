//
// Created by ZNix on 25/10/2020.
//

#pragma once

#include "XrDriverPrivate.h"

#include "Misc/Input/InteractionProfile.h"
#include "XrTrackedDevice.h"

#include <mutex>

// This warning tells us that a method (GetPose) was overridden by one of our parent classes
// Totally fine, that's the reason why we include XrTrackedDevice in the first place
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4250)
#endif

class XrHMD : public XrTrackedDevice, public IHMD {
	const InteractionProfile* profile = nullptr;
	std::shared_mutex profile_mutex;

	// Last-known-good view data, used when xrLocateViews hasn't produced valid poses yet or hiccups
	// mid-session. These used to be function-local statics written without any synchronisation -
	// games call GetEyeToHeadTransform and GetIPD from whatever thread they like.
	//
	// Keeping the last good value also means a transient runtime failure no longer kills the game:
	// the call sites used to OOVR_FALSE_ABORT on a short viewCount.
	std::mutex cachedViewMutex;
	XrTime cachedViewTime = ~0; // Not zero, or we'd return an identity matrix before the first frame
	XrView cachedEyeViews[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	// 64mm is roughly the average human IPD - note this is in metres, so 0.064 and not 0.0064.
	float cachedIpd = 0.064f;
	bool haveCachedProjection = false;
	XrFovf cachedFov[XruEyeCount] = {};

	/**
	 * The FOV for an eye, refreshed from the runtime if it is answering and falling back to the
	 * last known-good values if it isn't.
	 */
	XrFovf GetCachedFov(vr::EVREye eEye);

public:
	// Override the GetPose implementation to use the difference between spaces, in the hope it'll make the
	// head positioning possibly more accurate.
	void GetPose(
	    vr::ETrackingUniverseOrigin origin,
	    vr::TrackedDevicePose_t* pose,
	    ETrackingStateType trackingState) override;

	// The HMD is the reference the frame is timed to, so prediction is deliberately ignored (the
	// frame's display time is already the predicted pose).
	void GetPose(
	    vr::ETrackingUniverseOrigin origin,
	    vr::TrackedDevicePose_t* pose,
	    ETrackingStateType trackingState,
	    float predictedSecondsToPhotonsFromNow) override;

	// from BaseSystem

	void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override;

	/** The projection matrix for the specified eye */
	vr::HmdMatrix44_t GetProjectionMatrix(vr::EVREye eEye, float fNearZ, float fFarZ, EGraphicsAPIConvention convention) override;

	/** The components necessary to build your own projection matrix in case your
	 * application is doing something fancy like infinite Z */
	void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override;

	/** Gets the result of the distortion function for the specified eye and input UVs. UVs go from 0,0 in
	 * the upper left of that eye's viewport and 1,1 in the lower right of that eye's viewport.
	 * Returns true for success. Otherwise, returns false, and distortion coordinates are not suitable. */
	bool ComputeDistortion(vr::EVREye eEye, float fU, float fV, vr::DistortionCoordinates_t* pDistortionCoordinates) override;

	/** Returns the transform from eye space to the head space. Eye space is the per-eye flavor of head
	 * space that provides stereo disparity. Instead of Model * View * Projection the sequence is Model * View * Eye^-1 * Projection.
	 * Normally View and Eye^-1 will be multiplied together and treated as View in your application.
	 */
	vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eEye) override;

	float GetIPD() override;

	/** Returns the number of elapsed seconds since the last recorded vsync event. This
	 *	will come from a vsync timer event in the timer if possible or from the application-reported
	 *   time if that is not available. If no vsync times are available the function will
	 *   return zero for vsync time and frame counter and return false from the method. */
	bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) override;

	/** Returns the hidden area mesh for the current HMD. The pixels covered by this mesh will never be seen by the user after the lens distortion is
	 * applied based on visibility to the panels. If this HMD does not have a hidden area mesh, the vertex data and count will be NULL and 0 respectively.
	 * This mesh is meant to be rendered into the stencil buffer (or into the depth buffer setting nearz) before rendering each eye's view.
	 * This will improve performance by letting the GPU early-reject pixels the user will never see before running the pixel shader.
	 * NOTE: Render this mesh with backface culling disabled since the winding order of the vertices can be different per-HMD or per-eye.
	 * Setting the bInverse argument to true will produce the visible area mesh that is commonly used in place of full-screen quads.
	 * The visible area mesh covers all of the pixels the hidden area mesh does not cover.
	 * Setting the bLineLoop argument will return a line loop of vertices in HiddenAreaMesh_t->pVertexData with
	 * HiddenAreaMesh_t->unTriangleCount set to the number of vertices.
	 */
	vr::HiddenAreaMesh_t GetHiddenAreaMesh(vr::EVREye eEye, vr::EHiddenAreaMeshType type) override;

	// Properties
	bool GetBoolTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL) override;
	float GetFloatTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL) override;
	uint32_t GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, char* pchValue,
	    uint32_t unBufferSize, vr::ETrackedPropertyError* pErrorL) override;
	int32_t GetInt32TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL) override;

	void SetInteractionProfile(const InteractionProfile* profile);
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
