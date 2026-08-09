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
#include <vector>

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
    void LoadFlycastPlayStats(const std::string &romPath);
    void SaveFlycastPlayStats(const std::string &romPath);
    void SetFastForwardToggleMode(bool toggleMode);
    struct KnownDisc {
        std::string path;
        std::string label;
    };
    void LoadDiscSession();
    void SaveDiscSession() const;
    void RecordSuccessfulDiscSwap(const std::string &path);
    std::vector<KnownDisc> GetKnownDiscs() const;
    bool HasPreviousDiscSession() const;
    bool CanRestorePreviousDiscSession() const;
    void ResumeLastDiscSession();
    // Menu-open thumbnail: capture the pure gameplay frame into memory when
    // the menu is opened; write it next to the state file on save.
    void CaptureMenuThumbnailToMemory();
    void WriteStateThumbnailFromMemory(const std::string &statePath);

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
    // Menu-open thumbnail state (captured before the menu renders).
    bool m_menuPendingThumb_ = false;
    std::vector<uint8_t> m_thumbMemory_;
    int inputSuppressFrames_ = 0; // game input cleared for a few frames after the menu closes
    uint32_t m_thumbW_ = 0;
    uint32_t m_thumbH_ = 0;
    std::string romPath_;
    std::string titleArg_;     // Display title from the launcher (argv[2])
    bool isArcade_ = false;    // NAOMI / Atomiswave (directionals -> analog axis)
    // Play stats + auto save/load (launcher config.cfg keys).
    int playCount_ = 0;
    int playTimeTotal_ = 0;
    bool playStatsFound_ = false;
    int sessionPlaySeconds_ = 0;
    double sessionPlayFraction_ = 0.0;
    uint32_t playTimeLastTicks_ = 0;
    int autoLoadStateSlot_ = 0;
    int autoSaveOnExitSlot_ = 0;
    std::string savePath_; // per-game save dir from the launcher GameDB (savePath field)
    std::string launchDiscPath_;
    std::string activeDiscPath_;
    std::string lastActiveDiscPath_;
    std::string pendingDiscPath_;
    int pendingDiscStateSlot_ = -1;
    std::vector<KnownDisc> knownDiscs_;
        uint32_t lastTicks_ = 0;
    float overlayBaseFontScale_ = 1.0f;
};

}  // namespace GBAStation
