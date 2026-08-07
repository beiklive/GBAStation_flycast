/// @file GBAStationOverlayHost.h
/// @brief Backend-agnostic host interface for GBAStationOverlay.
///
/// The overlay used to call GBAStationCore directly and create GPU textures through
/// GBAStationVulkan/GL #ifdefs. To run the same overlay on both the libretro path
/// (GBAStationCore + GBAStationVulkan) and the native standalone path (flycast emu +
/// imguiDriver), all of that goes through IOverlayHost. Mirrors how
/// GBAStation-ppsspp's overlay talks to its host via commands + a Draw context.
#pragma once

#include "imgui.h"

#include <mutex>
#include <string>
#include <vector>

/// Where RetroAchievements toasts are anchored.
enum class RAAlertPosition
{
    TopLeft = 0,
    TopRight,
    BottomLeft,
    BottomRight
};

/// A RetroAchievements toast queued for the overlay.
struct RANotification
{
    std::string title;
    std::string description;
    std::string badge_name;     // badge id, or "ra_icon" for the session toast
    ImTextureID textureId = 0;  // resolved by the overlay from the host's cache
    float timer = 0.0f;
    float duration = 4.0f;
    float slideIn = 0.4f;
    float slideOut = 0.4f;
};

/// Optional RetroAchievements data source. Only the libretro path provides one;
/// the standalone path returns nullptr from IOverlayHost::RA().
class IOverlayRAHost
{
public:
    virtual ~IOverlayRAHost() = default;

    virtual std::mutex &Mutex() = 0;
    virtual std::vector<RANotification> &Notifications() = 0;
    virtual RAAlertPosition AlertPosition() const = 0;
    virtual ImTextureID IconTexture() const = 0;
    virtual void SetIconTexture(ImTextureID tex) = 0;
    virtual ImTextureID BadgeTexture(const std::string &badge) const = 0; // 0 if absent
};

/// Everything the overlay needs from the emulator + renderer, abstracted so the
/// overlay is free of GBAStationCore / GBAStationVulkan / GL.
class IOverlayHost
{
public:
    virtual ~IOverlayHost() = default;

    // Content / save states / discs.
    virtual std::string GetGamePath() = 0;
    virtual bool IsGameLoaded() = 0;
    virtual bool StateSlotExists(int slot) = 0;
    // Modification time of a save-state file, 0 when absent (for the slot list
    // timestamp line).
    virtual time_t StateSlotTime(int slot) { return 0; }
    virtual void SaveStateSlot(int slot) = 0;
    virtual void LoadStateSlot(int slot) = 0;
    virtual void SwapDisc(const std::string &path) = 0;

    // Live libretro options. Implementations that do not expose options retain
    // the fallback values and leave the menu rows read-only.
    virtual std::string GetCoreOption(const std::string &, const std::string &fallback = "") { return fallback; }
    virtual void SetCoreOption(const std::string &, const std::string &) {}

    // Fast forward (toggle/hold mode + multiplier).
    virtual float GetFastForwardMultiplier() { return 2.0f; }
    virtual void SetFastForwardMultiplier(float) {}
    virtual bool GetFastForwardToggleMode() { return false; }
    virtual void SetFastForwardToggleMode(bool) {}
    virtual bool GetFastForwardActive() { return false; }

    // HUD.
    virtual bool GetShowFps() { return false; }
    virtual double GetCoreFps() { return 0.0; }

    // GPU textures (RGBA8). Backend-specific implementation lives in the host.
    virtual ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height) = 0;
    virtual void DestroyTexture(ImTextureID tex) = 0;

    // Optional RetroAchievements source (nullptr when unsupported).
    virtual IOverlayRAHost *RA() { return nullptr; }
};
