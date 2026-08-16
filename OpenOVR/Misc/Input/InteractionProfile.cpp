//
// Created by ZNix on 27/02/2021.
//

#include "stdafx.h"

#include <utility>

#include "HolographicInteractionProfile.h"
#include "IndexControllerInteractionProfile.h"
#include "InteractionProfile.h"
#include "KhrSimpleInteractionProfile.h"
#include "OculusInteractionProfile.h"
#include "ReverbG2InteractionProfile.h"
#include "ViveInteractionProfile.h"
#include "ViveTrackerInteractionProfile.h"

#include "OculusHandPoses.h"

#include "Reimpl/BaseInput.h"
#include "generated/static_bases.gen.h"

std::string InteractionProfile::TranslateAction(const std::string& inputPath) const
{
	if (!pathTranslationMap.empty() && !IsInputPathValid(inputPath)) {
		std::string ret = inputPath;
		for (auto& [key, val] : pathTranslationMap) {
			size_t loc = ret.find(key);
			if (loc != std::string::npos) {
				// translate action!
				ret = ret.substr(0, loc) + val + ret.substr(loc + key.size());
			}
		}
		OOVR_LOGF("Translated path %s to %s for profile %s", inputPath.c_str(), ret.c_str(), GetPath().c_str());
		return ret;
	}
	// either this path is already valid or it's invalid and not translatable
	return inputPath;
}

const std::unordered_set<std::string>& InteractionProfile::GetValidInputPaths() const
{
	return validInputPaths;
}

bool InteractionProfile::IsInputPathValid(const std::string& inputPath) const
{
	return validInputPaths.find(inputPath) != validInputPaths.end();
}

void InteractionProfile::AddLegacyBindings(const LegacyControllerActions& ctrl, std::vector<XrActionSuggestedBinding>& bindings) const
{
	auto create = [&](XrAction action, const char* path) {
		// Not supported on this controller?
		if (path == nullptr)
			return;

		// Bind the action to a suggested binding
		std::string realPath = ctrl.handPath + "/" + path;
		if (!IsInputPathValid(realPath)) {
			// No need for a soft abort, will only be called if an input profile gets it's paths wrong
			OOVR_ABORTF("Found legacy input path %s, not supported by profile", realPath.c_str());
		}

		// The action must have been defined, if not we'll get a harder-to-debug error from OpenXR later.
		OOVR_FALSE_ABORT(action != XR_NULL_HANDLE);

		XrActionSuggestedBinding binding = {};
		binding.action = action;

		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, realPath.c_str(), &binding.binding));

		bindings.push_back(binding);
	};

	const LegacyBindings* paths = GetLegacyBindings(ctrl.handPath);

	create(ctrl.system, paths->system);
	create(ctrl.menu, paths->menu);
	create(ctrl.menuTouch, paths->menuTouch);
	create(ctrl.btnA, paths->btnA);
	create(ctrl.btnATouch, paths->btnATouch);
	create(ctrl.trackpadX, paths->trackpadX);
	create(ctrl.trackpadY, paths->trackpadY);
	create(ctrl.trackpadTouch, paths->trackpadTouch);
	create(ctrl.trackpadForce, paths->trackpadForce);
	create(ctrl.stickX, paths->stickX);
	create(ctrl.stickY, paths->stickY);
	create(ctrl.stickBtn, paths->stickBtn);
	create(ctrl.stickBtnTouch, paths->stickBtnTouch);
	create(ctrl.trigger, paths->trigger);
	create(ctrl.triggerTouch, paths->triggerTouch);
	create(ctrl.triggerClick, paths->triggerClick);
	create(ctrl.grip, paths->grip);
	create(ctrl.gripClick, paths->grip);
	create(ctrl.haptic, paths->haptic);
	create(ctrl.gripPoseAction, paths->gripPoseAction);
	create(ctrl.aimPoseAction, paths->aimPoseAction);
}

const InteractionProfile::ProfileList& InteractionProfile::GetProfileList()
{
	// Built inside the initialiser of a function-local static rather than with an `if (empty())`
	// check afterwards. Both give a single fill, but only this form is thread-safe: the old shape
	// let the event-pump thread (UpdateInteractionProfile) and the game thread (SetActionManifestPath)
	// both see an empty vector and both fill it. C++11 guarantees exactly one thread runs this
	// lambda while the others block.
	static const std::vector<std::unique_ptr<InteractionProfile>> profiles = [] {
		std::vector<std::unique_ptr<InteractionProfile>> list;
		if (xr_ext->G2Controller_Available())
			list.emplace_back(std::make_unique<ReverbG2InteractionProfile>());

		list.emplace_back(std::make_unique<HolographicInteractionProfile>());
		list.emplace_back(std::make_unique<IndexControllerInteractionProfile>());
		list.emplace_back(std::make_unique<ViveWandInteractionProfile>());
		list.emplace_back(std::make_unique<OculusTouchInteractionProfile>());
		list.emplace_back(std::make_unique<KhrSimpleInteractionProfile>());
		list.emplace_back(std::make_unique<ViveTrackerInteractionProfile>());
		return list;
	}();
	return profiles;
}

InteractionProfile* InteractionProfile::GetProfileByPath(const string& name)
{
	// Same reasoning as GetProfileList above.
	static const std::map<std::string, InteractionProfile*> byPath = [] {
		std::map<std::string, InteractionProfile*> map;
		for (const std::unique_ptr<InteractionProfile>& profile : GetProfileList()) {
			map[profile->GetPath()] = profile.get();
		}
		return map;
	}();

	auto it = byPath.find(name);
	if (it == byPath.end())
		OOVR_ABORTF("Could not find interaction profile '%s'", name.c_str());
	return it->second;
}

glm::mat4 InteractionProfile::GetGripToSteamVRTransform(ITrackedDevice::TrackedDeviceType hand) const
{
	if (hand == ITrackedDevice::TrackedDeviceType::HAND_LEFT) {
		return leftHandGripTransform;
	} else if (hand == ITrackedDevice::TrackedDeviceType::HAND_RIGHT) {
		return rightHandGripTransform;
	}
	return glm::identity<glm::mat4>();
}

std::optional<glm::mat4> InteractionProfile::GetComponentTransform(ITrackedDevice::TrackedDeviceType hand, const std::string& name) const
{
	// Reference, not copy - this is called on the per-frame input path.
	const auto& transforms = hand == ITrackedDevice::HAND_RIGHT ? rightComponentTransforms : leftComponentTransforms;
	const auto iter = transforms.find(name);
	if (iter == transforms.end())
		return {};
	else
		return iter->second;
}

std::optional<std::array<vr::VRBoneTransform_t, 31>> InteractionProfile::GetSkeletalReferencePose(ITrackedDevice::TrackedDeviceType hand, int pose) const
{
	// Reference, not copy: this runs per frame from GetSkeletalBoneData and the hand-tracking
	// fallback, and the maps hold 31-bone arrays - copying them per call allocates constantly.
	const auto& poses = hand == ITrackedDevice::HAND_LEFT ? leftHandPoses : rightHandPoses;

	// Fall back to oculus poses, which should be close enough for most controllers. Built once,
	// not per call.
	static const std::array<std::array<vr::VRBoneTransform_t, 31>, 4> leftFallback = {
		oculus::leftBindPose,
		oculus::leftOpenHandPose,
		oculus::leftFistPose,
		oculus::leftGripLimitPose,
	};
	static const std::array<std::array<vr::VRBoneTransform_t, 31>, 4> rightFallback = {
		oculus::rightBindPose,
		oculus::rightOpenHandPose,
		oculus::rightFistPose,
		oculus::rightGripLimitPose,
	};

	// The fallback is indexed by the OpenVR reference-pose enum (BindPose..GripLimit are 0..3)
	if (poses.empty()) {
		OOVR_LOG_ONCE("WARNING: No reference poses defined for interaction profile, using fallback.");
		const auto& fallback = hand == ITrackedDevice::HAND_LEFT ? leftFallback : rightFallback;
		if (pose >= 0 && pose < (int)fallback.size())
			return fallback[pose];
		return {};
	}

	const auto iter = poses.find(pose);
	if (iter == poses.end())
		return {};
	else
		return iter->second;
}
