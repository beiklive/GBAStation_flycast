/// @file GBAStationOverlay.h
/// @brief Overlay UI for GBAStation-integrated Flycast
#pragma once

#include "imgui.h"
#include "GBAStationMain.h"        // GBAStation::FrameInput / PadButton
#include "GBAStationOverlayHost.h" // IOverlayHost / RANotification / RAAlertPosition
#include "GBAStationSlangPreset.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

/// @brief Overlay menu types
enum class OverlayMenu
{
    None,
    QuickMenu,
    SaveStates,
    Settings,
    DiscSelect,
    StartupDiscChoice
};

/// @brief Display mode for the emulator viewport
enum class FlycastDisplayMode
{
    Integer = 0, // Integer pixel scaling
    Display = 1, // Aspect-ratio based display
    COUNT = 2
};

/// @brief Display size (context-dependent on FlycastDisplayMode)
///   Integer → 1x … 5x, Auto
///   Display → Stretch, 4:3, 16:9, Original
enum class FlycastDisplaySize
{
    // Display sizes (0-3)
    Stretch = 0,
    _4_3 = 1,
    _16_9 = 2,
    Original = 3,
    // Integer sizes (4-6)
    _1x = 4,
    _2x = 5,
    Auto = 6,
    _3x = 7,
    _4x = 8,
    _5x = 9
};

/// @brief Overlay UI for Flycast with GBAStation styling
class GBAStationOverlay
{
public:
    GBAStationOverlay();
    ~GBAStationOverlay();

    /// @brief Update overlay animation
    void Update(float deltaTime);

    /// @brief Render the overlay
    void Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                int frameWidth, int frameHeight, int fboWidth = 0, int fboHeight = 0);

    /// @brief Compute the on-screen game viewport for the current display
    /// mode/size. Returns the rect (x,y,w,h) in pixels within a screenW×screenH
    /// surface; coreAspect is the core's reported aspect ratio. The libretro
    /// path uses this to letterbox the Vulkan blit, since the game image is
    /// composited by GBAStationVulkan rather than drawn through ImGui.
    void GetGameViewport(float screenW, float screenH, float coreAspect,
                         float &outX, float &outY, float &outW, float &outH) const;

    /// @brief Handle neutralized input
    /// @return true if input was consumed by overlay
    bool HandleInput(const GBAStation::FrameInput &input);

    /// @brief Show/hide overlay
    void Show();
    void Hide();
    void ShowStartupDiscChoice(const std::string &lastDiscLabel, bool canRestoreState);
    bool IsVisible() const { return m_currentMenu != OverlayMenu::None; }

    /// @brief Set game title for title card
    void SetGameTitle(const std::string &title) { m_gameTitle = title; }
    void SetGameDisplaySettings(int displayMode, const std::string &screenLayout,
                                const std::string &internalResolution, int integerScale = 1);
    void SetMaskSettings(bool enabled, const std::string &path);
    bool IsMaskEnabled() const { return m_maskEnabled; }
    const std::string &MaskPath() const { return m_maskPath; }
    // Keeps the compiled preset alive after validation.  The Vulkan chain can
    // consume its SPIR-V and reflection data without reparsing a file during a
    // frame.
    void SetShaderSettings(bool enabled, const std::string &path,
                           const std::vector<std::string> &names = {},
                           const std::vector<float> &values = {});
    void SetShaderPreset(bool enabled, GBAStationSlang::Preset preset);
    bool IsShaderEnabled() const { return m_shaderEnabled; }
    const std::string &ShaderPath() const { return m_shaderPath; }
    const GBAStationSlang::Preset *ShaderPreset() const { return m_shaderPresetValid ? &m_shaderPreset : nullptr; }
    const std::vector<GBAStationSlang::Parameter> &ShaderParameters() const { return m_shaderPreset.parameters; }
    int GetGameIntegerScale() const;
    bool ConsumeGameDisplaySettingsSaveRequest();
    int GetGameDisplayModeIndex() const { return static_cast<int>(m_displayMode); }
    const char *GetGameScreenLayout() const;

    /// @brief Set the backend host (emulator + renderer adapter). Triggers a
    /// reload of host-backed assets (avatar texture) now that a renderer exists.
    void SetHost(IOverlayHost *host)
    {
        m_host = host;
        // The 3DS-style overlay has no account/avatar surface.  Avoid account
        // service and GPU uploads while the Vulkan render context is starting.
    }

    /// @brief Font used for RA alert descriptions (description.ttf). When unset,
    /// the overlay falls back to the atlas' second font / current font.
    void SetDescriptionFont(ImFont *font) { m_descFont = font; }

    /// @brief Check if user wants to exit
    bool ShouldExit() const { return m_shouldExit; }
    void ClearExit() { m_shouldExit = false; }

    /// @brief Check if user wants to reset
    bool ShouldReset() const { return m_shouldReset; }
    void ClearReset() { m_shouldReset = false; }

private:
    void RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                    float aspectRatio, int width, int height,
                    int fboWidth, int fboHeight);
    void DrawHud(ImDrawList *dl, ImVec2 displaySize);
    void RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize);
    void RenderTitleCard(ImDrawList *dl, ImVec2 displaySize);
    void RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderGBAStationMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderStatusBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderDiscMenu(ImDrawList *dl, ImVec2 displaySize);
    void ScanForDiscs();
    void OpenDiscBrowser();
    void RefreshDiscBrowser();
    void RenderDiscBrowser(ImDrawList *dl, ImVec2 displaySize);
    void RenderSettingsSidebar(ImDrawList *dl, ImVec2 displaySize);
    void RenderFilePicker(ImDrawList *dl, ImVec2 displaySize, bool shaderPicker);
    void OpenSettingsSidebar(bool shader);
    void CloseSettingsSidebar();
    void OpenMaskFilePicker();
    void ReloadMaskFilePicker(const std::string &directory, const std::string &focusPath = {});
    void OpenShaderFilePicker();
    void ReloadShaderFilePicker(const std::string &directory, const std::string &focusPath = {});
    static bool IsMaskImagePath(const std::string &path);
    static bool IsShaderPath(const std::string &path);
    void RenderStartupDiscChoice(ImDrawList *dl, ImVec2 displaySize);
    void RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime);
    void ActivateTab(int tab);
    void EnsureRAIconLoaded();
    void ResolveNotificationTextures();
    void ReloadMaskTexture();
    bool m_raIconLoadAttempted = false;

    OverlayMenu m_currentMenu = OverlayMenu::None;
    std::string m_gameTitle;
    IOverlayHost *m_host = nullptr;
    ImFont *m_descFont = nullptr; // description.ttf for RA alert descriptions

    // Animation
    float m_animTimer = 0.0f;

    // Menu state
    int m_quickMenuSelection = 0;
    bool m_sidebarFocused = true;
    int m_saveStateSlot = 0;
    // Cached state-slot thumbnails (texture + state file mtime).
    struct SlotThumb {
        ImTextureID tex = 0;
        time_t mtime = 0;
    };
    std::array<SlotThumb, 10> m_slotThumbs;
    bool m_isSaveMode = true;
    int m_settingsSelection = 0;
    bool m_gameDisplaySettingsSaveRequested = false;
    FlycastDisplayMode m_displayMode = FlycastDisplayMode::Display;
    FlycastDisplaySize m_displaySize = FlycastDisplaySize::_4_3;
    bool m_maskEnabled = false;
    std::string m_maskPath;
    ImTextureID m_maskTexture = 0;
    bool m_shaderEnabled = false;
    std::string m_shaderPath;
    GBAStationSlang::Preset m_shaderPreset;
    bool m_shaderPresetValid = false;
    enum class SettingsSidebar { None, Shader, ShaderFilePicker, Mask, MaskFilePicker };
    SettingsSidebar m_settingsSidebar = SettingsSidebar::None;
    int m_settingsSidebarSelection = 0;
    struct FileEntry {
        std::string name;
        std::string path;
        bool isDirectory = false;
    };
    std::vector<FileEntry> m_maskFileEntries;
    std::string m_maskFilePickerDirectory;
    std::string m_maskFilePickerRoot;
    std::unordered_map<std::string, int> m_maskFilePickerSelections;
    std::vector<FileEntry> m_shaderFileEntries;
    std::string m_shaderFilePickerDirectory;
    std::string m_shaderFilePickerRoot;
    std::unordered_map<std::string, int> m_shaderFilePickerSelections;
    int m_discSelection = 0;
    float m_discScrollY = 0.0f;
    float m_discTargetScrollY = 0.0f;

    struct DiscEntry {
        std::string displayName;
        std::string romPath;
    };
    std::vector<DiscEntry> m_discs;

    // Disc browser (manual disc swap file picker).
    struct DiscBrowserEntry {
        std::string name;
        std::string path;
        bool isDir = false;
        bool isKnownDisc = false;
        bool isActiveDisc = false;
    };
    std::string m_discBrowserDir;
    std::string m_discBrowserRoot;
    bool m_discBrowserStartupMode = false;
    std::vector<DiscBrowserEntry> m_registeredDiscs;
    std::vector<DiscBrowserEntry> m_discBrowserEntries;
    int m_discBrowserSelection = 0;
    float m_discBrowserScrollY = 0.0f;
    float m_discBrowserTargetScrollY = 0.0f;
    std::string m_discBrowserNotice;
    float m_discBrowserNoticeTimer = 0.0f;
    int m_startupDiscChoice = 0;
    bool m_startupCanRestoreState = false;
    std::string m_startupLastDiscLabel;

    // Settings persistence
    void LoadCoreSettings();
    void SaveCoreSettings();
    void ApplyScalingSettings(bool save = true);

    // Triangle texture
    ImTextureID m_triangleTexture = 0;
    int m_triangleWidth = 0;
    int m_triangleHeight = 0;

// Directional nav repeat (3DS-style time-based: first repeat waits 280 ms,
// then repeats speed up 128 ms -> 48 ms while held).
uint64_t m_navHeldPrev = 0;
uint64_t m_navFireAtMs = 0;
uint64_t m_navStartMs = 0;
static constexpr uint64_t NAV_INITIAL_DELAY_MS = 280;
// Never accelerate past roughly eleven focus moves per second.  The previous
// 48 ms floor made long lists almost uncontrollable after holding a direction.
static constexpr uint64_t NAV_MIN_REPEAT_MS = 88;
static constexpr uint64_t NAV_START_REPEAT_MS = 156;

    // Start+Select opens the overlay only; it never closes it (Back closes), so
    // no combo-edge/flicker state is needed.

    // Exit/Reset flags
    bool m_shouldExit = false;
    bool m_shouldReset = false;
    // Gates the Minus=reset shortcut so the open combo (Start+Select) can't
    // trigger it immediately.
    float m_quickMenuOpenTime = 0.0f;

    // Battery Status
    uint32_t m_batteryLevel = 100;
    bool m_isCharging = false;
    float m_batteryTimer = 0.0f;
    float m_chargingStateProgress = 0.0f;
    ImTextureID m_boltTexture = 0;
    int m_boltWidth = 0;
    int m_boltHeight = 0;

    // Config
    bool m_isDarkMode = true;
    bool m_showNickname = false; // Added
    std::string m_hourFormat = "24h";
    void LoadConfig();
    void LoadGeneralConfig();
    void LoadSVGIcon();

    // Social Area
    ImTextureID m_avatarTexture = 0;
    int m_avatarWidth = 0;
    int m_avatarHeight = 0;
    std::string m_nickname;
    void LoadAccountData();
    bool LoadAvatarTextureFromFile(const char *path);
    bool LoadAvatarTextureFromMemory(const unsigned char *data, size_t size, const char *tag);
    void ReleaseAvatarTexture();
    void RenderSocialArea(ImDrawList *dl, ImVec2 displaySize);
    void LoadFocusTexture();
    void ReleaseFocusTexture();
    void DrawFocusBorder(ImVec2 min, ImVec2 max, float thickness);
    ImTextureID m_focusTexture = 0;
    int m_focusTextureWidth = 0;
    int m_focusTextureHeight = 0;
};
