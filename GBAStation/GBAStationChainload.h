/// @file GBAStationChainload.h
/// @brief Chainload back to the GBAStation launcher NRO. Emulator-agnostic.
#pragma once

#include "GBAStationLogger.h"

namespace GBAStation
{

/// Queue the GBAStation launcher (sdmc:/switch/GBAStation.nro, with fallback) as the next
/// homebrew to load via envSetNextLoad. Call after the runtime has shut down.
/// No-op off Switch.
void SetLauncherReturnPath(const char *path);
bool HasLauncherReturnPath();
void SetExternalSessionToken(const char *token);
void ChainloadLauncher(const LogCallback &log = {});

}  // namespace GBAStation
