#pragma once

class Config {
public:
	Config();
	~Config();

	bool RenderCustomHands() const { return renderCustomHands; }
	vr::HmdColor_t HandColour() const { return handColour; }
	float SupersampleRatio() const { return supersampleRatio; }
	bool Haptics() const { return haptics; }
	bool AdmitUnknownProps() const { return admitUnknownProps; }
	inline bool LogGetTrackedProperty() const { return logGetTrackedProperty; }
	inline bool StopOnSoftAbort() const { return stopOnSoftAbort; }
	inline bool DX10Mode() const { return dx10Mode; }
	inline bool EnableHiddenMeshFix() const { return enableHiddenMeshFix; }
	inline bool InvertUsingShaders() const { return invertUsingShaders; }
	inline bool InitUsingVulkan() const { return initUsingVulkan; }
	float HiddenMeshVerticalScale() const { return hiddenMeshVerticalScale; }
	inline bool LogAllOpenVRCalls() const { return logAllOpenVRCalls; }

	/**
	 * Frames taking longer than this (in milliseconds) get a one-line breakdown in the log saying
	 * which phase of the frame ate the time. Set to 0 to disable entirely.
	 *
	 * Unlike logAllOpenVRCalls this is cheap enough to leave on: a healthy frame logs nothing, so
	 * the cost is two clock reads per phase.
	 */
	float HitchWarningMs() const { return hitchWarningMs; }

private:
	static int ini_handler(
	    void* user, const char* section,
	    const char* name, const char* value,
	    int lineno);

	bool renderCustomHands = true;
	vr::HmdColor_t handColour = vr::HmdColor_t{ 0.3f, 0.3f, 0.3f, 1 };
	float supersampleRatio = 1.0f;
	bool haptics = true;
	bool admitUnknownProps = false;
	bool logGetTrackedProperty = false;
	bool stopOnSoftAbort = false;
	bool dx10Mode = false;
	bool enableHiddenMeshFix = true;
	bool invertUsingShaders = false;
	bool initUsingVulkan = false;
	float hiddenMeshVerticalScale = 1.0f;
	bool logAllOpenVRCalls = false;
	float hitchWarningMs = 30.0f;
};

extern Config oovr_global_configuration;
