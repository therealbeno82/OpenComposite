//
// Created by ZNix on 25/10/2020.
//

#pragma once

// FIXME Make it so that this header can be used in the Vulkan compositor - unfortunate and we should probably fix that at some point
#include "pub/DrvOpenXR.h"

#include "../OpenOVR/Misc/xrutil.h"
#include "../OpenOVR/logging.h"

#ifndef _WIN32
#include "../OpenOVR/linux_funcs.h"
#endif

namespace DrvOpenXR {
void SetupSession();

/**
 * Tear down the current OpenXR session.
 *
 * @param forRestart true if a new session is about to be created in its place (the graphics-API
 *   switch and the input-rebind restart both do this), false if we're on the way out for good.
 *   When restarting we need a fresh XrSystemId for the next xrCreateSession; on a final shutdown
 *   asking for one is pointless, and it's actively harmful if the headset has already gone away -
 *   see the comment in ShutdownSession.
 */
void ShutdownSession(bool forRestart = true);
void FullShutdown();
} // namespace DrvOpenXR
