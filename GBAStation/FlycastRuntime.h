/// @file FlycastRuntime.h
/// @brief Flycast/libretro implementation of GBAStation::CoreRuntime.
///
/// Owns the libretro driver (GBAStationCore), the Vulkan host (GBAStationVulkan), SDL audio
/// (GBAStationAudio), and the overlay. All flycast-specific orchestration that used
/// to live in the monolithic GBAStationMain.cpp lives here, behind the agnostic
/// CoreRuntime interface that GBAStation::Main drives.
#pragma once

#include "GBAStationMain.h"

#include <SDL.h>

#include <cstdint>
#include <memory>
#include <string>

class GBAStationCore;
class GBAStationOverlay;
class GBAStationAudio;

namespace GBAStation
{

class FlycastOverlayHost;  // libretro IOverlayHost adapter (defined in the .cpp)

class FlycastRuntime final : public CoreRuntime
{
public:
    explicit FlycastRuntime(LogCallback log = {});
    ~FlycastRuntime() override;

    const char *Name() const override { return "flycast"; }
    bool Configure(const LaunchInfo &launch) override;
    bool Initialize(const LaunchInfo &launch) override;
    bool LoadContent(const std::string &path) override;
    void HandleInput(const FrameInput &input) override;
    void RunFrame() override;
    void RenderFrame() override;
    bool ShouldExit() const override { return exitRequested_; }
    bool ShouldChainloadLauncher() const override { return chainload_; }
    void RequestExit() override { exitRequested_ = true; }
    void Shutdown() override;

private:
    friend class FlycastOverlayHost;
    bool InitAudio();
    bool InitOverlay(const std::string &romPath);
    void ShutdownOverlay();
    void RenderOverlayFrame(float deltaTime);
    void ApplyCoreInput(const FrameInput &input);

    void SetFastForwardMultiplier(float multiplier);
    void SetFastForwardToggleMode(bool toggleMode);
    // Close the menu and capture the pure gameplay frame as a state thumbnail.
    void RequestStateThumbnail(const std::string &statePath);
    void CaptureStateThumbnail(const std::string &statePath);

    LogCallback log_;
    std::unique_ptr<GBAStationCore> core_;
    std::unique_ptr<GBAStationOverlay> overlay_;
    std::unique_ptr<FlycastOverlayHost> overlayHost_;
    std::unique_ptr<GBAStationAudio> audio_;
    SDL_AudioDeviceID audioDevice_ = 0;

    bool overlayReady_ = false;
    bool exitRequested_ = false;
    bool chainload_ = false;
    bool frameInFlight_ = false;
    bool fastForward_ = false;
    bool fastForwardToggle_ = false;
    bool fastForwardToggleMode_ = false;
    float fastForwardMultiplier_ = 2.0f;
    bool showFps_ = false;
    std::string m_stateThumbPending_;
    int m_stateThumbDelay_ = 0;
    std::string romPath_;
    std::string titleArg_;     // Display title from the launcher (argv[2])
    bool isArcade_ = false;    // NAOMI / Atomiswave (directionals -> analog axis)
    uint32_t lastTicks_ = 0;
    float overlayBaseFontScale_ = 1.0f;
};

}  // namespace GBAStation
