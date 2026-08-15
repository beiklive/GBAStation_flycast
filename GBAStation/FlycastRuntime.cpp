/// @file FlycastRuntime.cpp
/// @brief Flycast/libretro CoreRuntime. Orchestration extracted from the old
/// monolithic GBAStationMain.cpp; the v1 bring-up sequence and frame ordering are
/// preserved (Acquire → retro_run → composite overlay → present).

#include "FlycastRuntime.h"

#include "GBAStationAudio.h"
#include "GBAStationConfig.h"
#include "GBAStationCore.h"
#include "GBAStationLogger.h"
#include "GBAStationOverlay.h"
#include "json.hpp"
#include "GBAStationVulkan.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <SDL.h>
#include <SDL_mixer.h>

#include <algorithm>
#include <ctime>
#include <vector>
#include <zlib.h>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace GBAStation
{

namespace {

constexpr ImWchar kGBAStationMaterialRanges[] = {
    0xE000, 0xF8FF,
    0,
};

// ABXY / L / R key-glyph block of the Nintendo shared font.  These codepoints
// overlap the Material Icons private-use area, and ImGui keeps the glyph of
// whichever font is packed first — so this range must be registered on the
// base (shared) fonts, which are always added before the Material font.
constexpr ImWchar kGBAStationNintendoKeyRanges[] = {
    0xE0E0, 0xE0E5,
    0,
};

const ImWchar *GetGBAStationMenuGlyphRanges(ImGuiIO &io) {
    static ImVector<ImWchar> ranges;
    if (!ranges.empty()) return ranges.Data;

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(kGBAStationNintendoKeyRanges);
    // Core option names may contain glyphs outside ImGui's common Chinese set.
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
    builder.AddText(u8"返回游戏 保存状态 读取状态 金手指 画面设置 功能设置 重置游戏 退出游戏 核心设置 按键映射 "
                    u8"系统 BIOS 视频 渲染 性能 纹理 输入 网络 光枪 VMU 设置 语言 地区 自动 开启 关闭 "
                    u8"屏幕 过滤 分辨率 跳帧 宽屏 存档 槽位 游戏 模拟器 菜单 暂停 快进 确认 取消");
    builder.BuildRanges(&ranges);
    return ranges.Data;
}

ImFont *AddGBAStationSystemFont(ImGuiIO &io, float size) {
#ifdef __SWITCH__
    if (R_FAILED(plInitialize(PlServiceType_User))) {
        LOG_WARN("OVERLAY", "Switch shared font service is unavailable");
        return nullptr;
    }

    // Keep the same source priority as the working GBAStation 3DS menu.
    // The extended face contains the broader Simplified Chinese repertoire.
    const PlSharedFontType fontTypes[] = {
        PlSharedFontType_ExtChineseSimplified,
        PlSharedFontType_ChineseSimplified,
        PlSharedFontType_Standard,
    };
    ImFont *font = nullptr;
    int loadedFonts = 0;
    for (const PlSharedFontType type : fontTypes) {
        PlFontData sharedFont{};
        if (R_FAILED(plGetSharedFontByType(&sharedFont, type)) || !sharedFont.address || sharedFont.size == 0)
            continue;

        void *fontData = std::malloc(sharedFont.size);
        if (!fontData) continue;
        std::memcpy(fontData, sharedFont.address, sharedFont.size);

        ImFontConfig config;
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.MergeMode = font != nullptr;
        ImFont *added = io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(sharedFont.size),
                                                         size, &config, GetGBAStationMenuGlyphRanges(io));
        if (!added) {
            std::free(fontData);
            continue;
        }
        if (!font) font = added;
        ++loadedFonts;
    }
    // The Nintendo shared font carries the ABXY / L / R key glyphs used by the
    // 3DS-style menu chrome and its LR value selectors.
    PlFontData nintendoFont{};
    if (R_SUCCEEDED(plGetSharedFontByType(&nintendoFont, PlSharedFontType_NintendoExt)) &&
        nintendoFont.address && nintendoFont.size > 0) {
        void *fontData = std::malloc(nintendoFont.size);
        if (fontData) {
            std::memcpy(fontData, nintendoFont.address, nintendoFont.size);
            ImFontConfig config;
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.MergeMode = true;
            if (io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(nintendoFont.size),
                                               size, &config, GetGBAStationMenuGlyphRanges(io))) {
                ++loadedFonts;
            } else {
                std::free(fontData);
            }
        }
    }
    plExit();
    if (!font) {
        LOG_ERROR("OVERLAY", "No usable Switch shared font was loaded");
        return nullptr;
    }
    io.FontDefault = font;

    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    io.Fonts->AddFontFromFileTTF("romfs:/fonts/MaterialIcons-Regular.ttf", size,
                                 &iconConfig, kGBAStationMaterialRanges);
    LOG_INFO("OVERLAY", "Switch menu font atlas loaded from %d shared font source(s), Material Icons=%s",
             loadedFonts, io.Fonts->Fonts.Size > 0 ? "merged" : "unavailable");
    return font;
#else
    (void)io;
    (void)size;
    return nullptr;
#endif
}

void ApplyGBAStationMenuStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.075f, 0.090f, 0.97f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.070f, 0.105f, 0.120f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.050f, 0.420f, 0.390f, 1.00f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.070f, 0.520f, 0.480f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.050f, 0.420f, 0.390f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.070f, 0.520f, 0.480f, 1.00f);
}

} // namespace


namespace
{

/// State file path for a slot (shared by the overlay host and the runtime).
std::string FlycastStatePath(GBAStationCore *core, int slot, const std::string &savePath)
{
    const std::string romPath = core ? core->GetGamePath() : std::string();

    // Game name = basename without extension.
    std::string name = romPath;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);

    // Save dir comes from the launcher GameDB (savePath field); fall back to
    // the same default layout the launcher would have generated otherwise.
    std::string saveDir = savePath;
    if (saveDir.empty())
        saveDir = std::string(GBAStation::Paths::StatesRoot) + "/" + name;

    struct stat st;
    if (stat(saveDir.c_str(), &st) == -1)
        mkdir(saveDir.c_str(), 0777);

    return saveDir + "/" + name + ".ss" + std::to_string(slot);
}

std::string FlycastSessionPath(const std::string &launchDisc, const std::string &savePath)
{
    std::string name = launchDisc;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);

    std::string saveDir = savePath;
    if (saveDir.empty())
        saveDir = std::string(GBAStation::Paths::StatesRoot) + "/" + name;

    struct stat st;
    if (stat(saveDir.c_str(), &st) == -1)
        mkdir(saveDir.c_str(), 0777);
    return saveDir + "/flycast-session.json";
}

std::string NormalizeFlycastRomPath(std::string path)
{
    for (char &ch : path)
    {
        if (ch == '\\')
            ch = '/';
    }
    if (path.rfind("sdmc:", 0) == 0)
        path.erase(0, 5);
    while (path.size() > 1 && path[0] == '/' && path[1] == '/')
        path.erase(0, 1);
    return path;
}

std::string CurrentFlycastTimestamp()
{
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%y-%m-%d %H-%M-%S", &local);
    return buf;
}

}  // namespace
// Adapts the libretro GBAStationCore + GBAStationVulkan renderer to the overlay's
// backend-agnostic IOverlayHost / IOverlayRAHost interfaces.
class FlycastOverlayHost final : public IOverlayHost, public IOverlayRAHost
{
public:
    explicit FlycastOverlayHost(GBAStationCore *core) : core_(core) {}
    void SetRuntime(FlycastRuntime *runtime) { runtime_ = runtime; }

    std::string GetGamePath() override { return core_ ? core_->GetGamePath() : std::string(); }
    bool IsGameLoaded() override { return core_ && core_->IsGameLoaded(); }

    bool StateSlotExists(int slot) override
    {
        struct stat st;
        return stat(StatePath(slot).c_str(), &st) == 0;
    }
    time_t StateSlotTime(int slot) override
    {
        struct stat st;
        if (stat(StatePath(slot).c_str(), &st) == 0)
            return st.st_mtime;
        return 0;
    }
    void SaveStateSlot(int slot) override
    {
        if (core_) core_->SaveState(StatePath(slot));
        // Write the menu-open thumbnail (captured into memory before the menu
        // rendered) next to the state file.
        if (runtime_)
            runtime_->WriteStateThumbnailFromMemory(StatePath(slot));
    }
    void LoadStateSlot(int slot) override
    {
        if (core_) core_->LoadState(StatePath(slot));
    }
    void SwapDisc(const std::string &path) override
    {
        if (core_ && core_->SwapDiskByPath(path) && runtime_)
            runtime_->pendingDiscPath_ = path;
    }
    void UpdateGamePath(const std::string &) override
    {
        // The launch path owns the GameDB identity and save directory. Disc 2
        // must not become a separate game merely because it is mounted now.
    }
    std::vector<IOverlayHost::KnownDisc> GetKnownDiscs() override
    {
        std::vector<IOverlayHost::KnownDisc> result;
        if (!runtime_)
            return result;
        for (const auto &disc : runtime_->GetKnownDiscs())
            result.push_back({disc.path, disc.label, disc.path == runtime_->activeDiscPath_});
        return result;
    }
    void ResumeLastDiscSession() override
    {
        if (runtime_)
            runtime_->ResumeLastDiscSession();
    }
    std::string StateThumbPath(int slot) override
    {
        return StatePath(slot) + ".png";
    }
    ImTextureID CreateThumbTexture(const unsigned char *rgba, int width, int height) override
    {
        return GBAStationVulkan::CreateOverlayTextureRGBA(rgba, static_cast<uint32_t>(width),
                                                          static_cast<uint32_t>(height));
    }
    void DestroyThumbTexture(ImTextureID tex) override
    {
        if (tex)
            GBAStationVulkan::DestroyOverlayTexture(tex);
    }
    std::string GetCoreOption(const std::string &key, const std::string &fallback = "") override
    {
        return core_ ? core_->GetCoreOption(key, fallback) : fallback;
    }
    void SetCoreOption(const std::string &key, const std::string &value) override
    {
        if (core_) core_->SetCoreOption(key, value);
    }

    float GetFastForwardMultiplier() override
    {
        return runtime_->fastForwardMultiplier_;
    }
    void SetFastForwardMultiplier(float multiplier) override
    {
        runtime_->SetFastForwardMultiplier(multiplier);
    }
    bool GetFastForwardToggleMode() override
    {
        return runtime_->fastForwardToggleMode_;
    }
    void SetFastForwardToggleMode(bool toggleMode) override
    {
        runtime_->SetFastForwardToggleMode(toggleMode);
    }
    bool GetFastForwardActive() override
    {
        return runtime_->fastForward_;
    }
    bool GetShowFps() override
    {
        return runtime_->showFps_;
    }
    double GetCoreFps() override
    {
        return core_ ? core_->GetFPS() : 0.0;
    }

    ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height) override
    {
        return GBAStationVulkan::CreateOverlayTextureRGBA(rgba, static_cast<uint32_t>(width),
                                                    static_cast<uint32_t>(height));
    }
    void DestroyTexture(ImTextureID tex) override
    {
        if (tex) GBAStationVulkan::DestroyOverlayTexture(tex);
    }

    IOverlayRAHost *RA() override { return this; }

    // IOverlayRAHost — backed by GBAStationCore's RA state.
    std::mutex &Mutex() override { return core_->m_raCallbackMutex; }
    std::vector<RANotification> &Notifications() override { return core_->m_raNotifications; }
    RAAlertPosition AlertPosition() const override { return core_->m_raAlertPosition; }
    ImTextureID IconTexture() const override { return core_->m_raIconTexture; }
    void SetIconTexture(ImTextureID tex) override { core_->m_raIconTexture = tex; }
    ImTextureID BadgeTexture(const std::string &badge) const override
    {
        auto it = core_->m_raBadgeCache.find(badge);
        return it != core_->m_raBadgeCache.end() ? it->second : (ImTextureID)0;
    }

private:

    std::string StatePath(int slot) const
    {
        return FlycastStatePath(core_, slot, runtime_ ? runtime_->savePath_ : std::string());
    }

    GBAStationCore *core_ = nullptr;
    FlycastRuntime *runtime_ = nullptr;
};

namespace
{

#ifdef __SWITCH__
u8 s_lastOperationMode = 255;

/// Always 1920×1080 surface; crop selects the visible sub-region for handheld
/// vs docked. (Owned by the runtime now that GBAStation::Main is display-agnostic.)
void UpdateScreenMode()
{
    u8 op = appletGetOperationMode();
    if (op == s_lastOperationMode)
        return;
    if (op == AppletOperationMode_Handheld)
        nwindowSetCrop(nwindowGetDefault(), 0, 360, 1280, 1080);
    else
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
    s_lastOperationMode = op;
}
#else
void UpdateScreenMode() {}
#endif

// GBAStationCore::SetAudioCallbacks takes plain function pointers, so route them
// through a file-static audio sink set up in Initialize().
GBAStationAudio *s_audio = nullptr;

void AudioSampleCallback(int16_t left, int16_t right)
{
    if (s_audio)
        s_audio->PushSample(left, right);
}

size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return s_audio ? s_audio->PushSamples(data, frames) : frames;
}

void AudioFlushCallback()
{
    if (s_audio)
        s_audio->Flush();
}

const char *FirstExistingPath(const char *const *paths, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        FILE *fp = std::fopen(paths[i], "rb");
        if (fp)
        {
            std::fclose(fp);
            return paths[i];
        }
    }
    return nullptr;
}

std::string GameTitleFromPath(const std::string &path)
{
    std::string title = path;
    const size_t slash = title.find_last_of("/\\");
    if (slash != std::string::npos)
        title = title.substr(slash + 1);
    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos)
        title = title.substr(0, dot);
    return title.empty() ? "Flycast" : title;
}

}  // namespace

FlycastRuntime::FlycastRuntime(LogCallback log) : log_(std::move(log)) {}

FlycastRuntime::~FlycastRuntime() = default;

bool FlycastRuntime::Configure(const LaunchInfo &launch)
{
    romPath_ = launch.contentPath.empty() ? GBAStationConfig::TEST_ROM : launch.contentPath;
    titleArg_ = launch.title;
    return true;
}

bool FlycastRuntime::InitAudio()
{
    if (GBAStationConfig::USE_SDLQUEUEAUDIO)
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = GBAStationAudio::SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = GBAStationAudio::CHANNELS;
        want.samples = 2048;
        want.callback = nullptr;

        audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audioDevice_ == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
            return false;
        }
        LOG_INFO("AUDIO", "SDL_QueueAudio initialized (deviceID=%u, freq=%d)", audioDevice_, have.freq);
    }
    else
    {
        if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0)
        {
            LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
            return false;
        }
        LOG_INFO("AUDIO", "SDL_mixer initialized");
    }
    return true;
}

bool FlycastRuntime::Initialize(const LaunchInfo &)
{    // Fast forward defaults from the launcher config (same keys as 3DS/PSP).
    {
        const char *cfgPaths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
        for (const char *path : cfgPaths)
        {
            std::ifstream in(path);
            if (!in)
                continue;
            std::string line;
            while (std::getline(in, line))
            {
                const std::size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                if (value.size() > 2 && value[0] == 's' && value[1] == '|')
                    value = value.substr(2);
                if (key == "fastforward.multiplier")
                {
                    try
                    {
                        fastForwardMultiplier_ = std::clamp(std::stof(value), 0.5f, 5.0f);
                    }
                    catch (...)
                    {
                    }
                }
                else if (key == "fastforward.mode")
                {
                    fastForwardToggleMode_ = (value == "toggle");
                }
                else if (key == "display.showFps")
                {
                    showFps_ = (value == "true" || value == "1");
                }
                else if (key == "save.autoLoadState0")
                {
                    try
                    {
                        autoLoadStateSlot_ = std::clamp(std::stoi(value), 0, 10);
                    }
                    catch (...)
                    {
                    }
                }
                else if (key == "save.autoSaveOnExit")
                {
                    try
                    {
                        autoSaveOnExitSlot_ = std::clamp(std::stoi(value), 0, 10);
                    }
                    catch (...)
                    {
                    }
                }
            }
            break;
        }
    }

    // GBAStation::Main is SDL-free (shared with the standalone target), so the
    // libretro path initializes the SDL subsystems it needs (audio + timer).
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
        LOG_WARN("HOME", "SDL_Init(AUDIO|TIMER) failed: %s", SDL_GetError());

#ifdef __SWITCH__
    // The libretro Vulkan surface and compositor both use the same fixed
    // handheld/docked output.  Do not briefly configure 1080p here and then
    // recreate the VI surface at 720p: the stale window dimensions can leave
    // the first swapchain image scaled from the lower-left quadrant.
    nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
    UpdateScreenMode();
#endif

    InitAudio();

    // VkInstance + VkSurface must exist before retro_set_environment so the
    // negotiation interface can refer to a real surface at device creation.
    if (!GBAStationVulkan::CreateInstance())
    {
        LOG_ERROR("HOME", "GBAStationVulkan::CreateInstance failed");
        return false;
    }

    core_ = std::make_unique<GBAStationCore>();
    core_->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);

    audio_ = std::make_unique<GBAStationAudio>();
    s_audio = audio_.get();
    if (!audio_->Init(audioDevice_))
        LOG_WARN("HOME", "GBAStationAudio init failed");

    if (!core_->Init())
    {
        LOG_ERROR("HOME", "GBAStationCore::Init failed");
        return false;
    }
    return true;
}

bool FlycastRuntime::LoadContent(const std::string &path)
{
    romPath_ = path.empty() ? romPath_ : path;
    launchDiscPath_ = romPath_;
    LOG_INFO("HOME", "Loading ROM: %s", romPath_.c_str());

    // savePath is a GameDB property of the title, not of the currently mounted
    // disc. Load it before creating the per-game disc-session metadata.
    LoadFlycastPlayStats(launchDiscPath_);
    LoadDiscSession();

    if (!core_->LoadGame(romPath_))
    {
        LOG_ERROR("HOME", "LoadGame failed; idling");
    }
    else if (!InitOverlay(romPath_))
    {
        LOG_WARN("HOME", "Overlay init failed; continuing without GBAStation overlay");
    }

    const bool showStartupDiscChoice = HasPreviousDiscSession() && overlay_;
    if (showStartupDiscChoice)
    {
        overlay_->ShowStartupDiscChoice(lastActiveDiscPath_, CanRestorePreviousDiscSession());
    }
    else if (autoLoadStateSlot_ > 0 && core_ && core_->IsGameLoaded())
    {
        const int slot = std::clamp(autoLoadStateSlot_ - 1, 0, 9);
        const std::string statePath = FlycastStatePath(core_.get(), slot, savePath_);
        LOG_INFO("HOME", "GBAStation auto load state slot=%d path=%s", slot, statePath.c_str());
        struct stat st;
        if (stat(statePath.c_str(), &st) == 0)
            core_->LoadState(statePath);
        else
            LOG_WARN("HOME", "GBAStation auto load state missing slot=%d path=%s", slot, statePath.c_str());
    }

    lastTicks_ = SDL_GetTicks();
    return true; // Idle (matching v1) even if the game failed to load.
}

bool FlycastRuntime::InitOverlay(const std::string &romPath)
{
    if (!GBAStationVulkan::IsReady())
        return false;

    uint32_t width = 0, height = 0;
    GBAStationVulkan::GetSwapExtent(width, height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    overlayBaseFontScale_ = 1.0f;

    const char *const titleFontPaths[] = {
        "romfs:/fonts/font.ttf",
        "sdmc:/GBAStation/DC/fonts/font.ttf",
        "sdmc:/GBAStation/DC/assets/fonts/font.ttf",
        "sdmc:/GBAStation/DC/assets/font.ttf",
    };
    const char *const descriptionFontPaths[] = {
        "romfs:/fonts/description.ttf",
        "sdmc:/GBAStation/DC/fonts/description.ttf",
        "sdmc:/GBAStation/DC/assets/fonts/description.ttf",
        "sdmc:/GBAStation/DC/assets/description.ttf",
    };

    ImFont *systemFont = AddGBAStationSystemFont(io, 30.0f);
    const char *titleFont = FirstExistingPath(titleFontPaths, sizeof(titleFontPaths) / sizeof(titleFontPaths[0]));
    const char *descriptionFont = FirstExistingPath(descriptionFontPaths, sizeof(descriptionFontPaths) / sizeof(descriptionFontPaths[0]));
    if (!systemFont && titleFont)
        io.Fonts->AddFontFromFileTTF(titleFont, 30.0f);
    if (!systemFont && descriptionFont)
        io.Fonts->AddFontFromFileTTF(descriptionFont, 22.0f);
    if (io.Fonts->Fonts.Size == 0)
    {
        io.Fonts->AddFontDefault();
        overlayBaseFontScale_ = 1.55f;
    }
    io.FontGlobalScale = overlayBaseFontScale_ * OverlayModeScale();

    ApplyGBAStationMenuStyle();

    if (!GBAStationVulkan::InitOverlayRenderer())
    {
        ImGui::DestroyContext();
        LOG_WARN("OVERLAY", "ImGui Vulkan overlay renderer unavailable");
        return false;
    }

    LOG_INFO("OVERLAY", "creating GBAStation menu model");
    overlay_ = std::make_unique<GBAStationOverlay>();
    LOG_INFO("OVERLAY", "creating GBAStation menu host");
    overlayHost_ = std::make_unique<FlycastOverlayHost>(core_.get());
    overlayHost_->SetRuntime(this);
    LOG_INFO("OVERLAY", "binding GBAStation menu host");
    overlay_->SetHost(overlayHost_.get());
    // Prefer the launcher-supplied title; fall back to the rom filename.
    overlay_->SetGameTitle(titleArg_.empty() ? GameTitleFromPath(romPath) : titleArg_);
    overlay_->SetGameDisplaySettings(gameDisplayMode_, gameScreenLayout_, gameInternalResolution_);
    overlay_->SetMaskSettings(gameMaskEnabled_, gameMaskPath_);
    overlay_->SetShaderSettings(gameShaderEnabled_, gameShaderPath_);
    overlayReady_ = true;
    LOG_INFO("OVERLAY", "GBAStation overlay initialized");
    return true;
}

void FlycastRuntime::ShutdownOverlay()
{
    overlay_.reset();       // dtor frees textures via overlayHost_ (still alive)
    overlayHost_.reset();
    GBAStationVulkan::ShutdownOverlayRenderer();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    overlayReady_ = false;
}

void FlycastRuntime::RenderOverlayFrame(float deltaTime)
{
    if (!overlayReady_ || !overlay_)
        return;

    // Flycast owns the core command buffers for the game frame.  While the
    // menu is closed we still run the overlay frame so the HUD (FPS / fast
    // forward badge) can draw; RenderGame skips itself when the game texture
    // is 0, so no game render pass is mixed in.
    uint32_t width = 0, height = 0;
    GBAStationVulkan::GetSwapExtent(width, height);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = deltaTime > 0.0f ? deltaTime : (1.0f / 60.0f);
    io.FontGlobalScale = overlayBaseFontScale_ * OverlayModeScale();

    GBAStationVulkan::BeginOverlayFrame();
    ImGui::NewFrame();
    overlay_->Update(io.DeltaTime);
    overlay_->Render(io.DisplaySize, 0, 4.0f / 3.0f,
                     static_cast<int>(width), static_cast<int>(height),
                     static_cast<int>(width), static_cast<int>(height));
    ImGui::Render();
    GBAStationVulkan::SetOverlayDrawData(ImGui::GetDrawData());
}

void FlycastRuntime::ApplyCoreInput(const FrameInput &input)
{
    if (!core_)
        return;

    core_->ClearInputs();

    for (unsigned port = 0; port < MaxPlayers; ++port)
    {
        const PlayerInput &player = input.players[port];
        const uint64_t b = player.buttons;
        auto down = [&](PadButton bit) { return (b & bit) != 0; };

        // Dreamcast follows physical disposition through the neutral pad map.
        // NAOMI/Atomiswave key mapping is handled by the core (map_gamepad_button
        // selects the per-platform joymap after the game is loaded), so the
        // frontend always forwards the neutral layout.
        //
        // The stock Dreamcast pad exposes L/R as analog triggers on the
        // L2/R2 channels (the JOYPAD_L/R bits map to the DC C/Z bits, which
        // the stock pad masks off), so L/R are fed on both channels.
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_A, down(Pad_A));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_B, down(Pad_B));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_X, down(Pad_X));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_Y, down(Pad_Y));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L, down(Pad_L));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R, down(Pad_R));
        // Preserve the classic L/R defaults while allowing dedicated ZL/ZR
        // bindings through dc.handle.l2 / dc.handle.r2.
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L2, down(Pad_L) || down(Pad_L2)); // DC left trigger
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R2, down(Pad_R) || down(Pad_R2)); // DC right trigger
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_START, down(Pad_Start));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_SELECT, down(Pad_Select));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_UP, down(Pad_Up));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_DOWN, down(Pad_Down));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_LEFT, down(Pad_Left));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, down(Pad_Right));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L3, down(Pad_L3));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R3, down(Pad_R3));

        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, player.leftStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, player.leftStickY);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, player.rightStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, player.rightStickY);
    }
}

void FlycastRuntime::HandleInput(const FrameInput &input)
{
    bool consumed = false;
    if (overlay_)
    {
        const bool wasVisible = overlay_->IsVisible();
        consumed = overlay_->HandleInput(input);
        // The menu was just opened: capture the pure gameplay frame before the
        // menu renders (the menu is delayed one frame in RenderFrame).
        if (!wasVisible && overlay_->IsVisible())
            m_menuPendingThumb_ = true;
        // The menu just closed: suppress game input for a few frames so the
        // confirm/back button press does not bleed into the game.
        if (wasVisible && !overlay_->IsVisible())
            inputSuppressFrames_ = 3;
        if (overlay_->ShouldReset())
        {
            LOG_INFO("OVERLAY", "Reset requested");
            if (core_)
                core_->Reset();
            overlay_->ClearReset();
        }
        if (overlay_->ShouldExit())
        {
            LOG_INFO("OVERLAY", "Exit requested");
            overlay_->ClearExit();
            chainload_ = true;
            exitRequested_ = true;
        }
        if (overlay_->ConsumeGameDisplaySettingsSaveRequest())
            SaveFlycastDisplaySettings();
    }
    if (exitRequested_)
        return;

    const bool overlayVisible = overlay_ && overlay_->IsVisible();
    const bool ffHeld = (input.buttons & Pad_FastForward) != 0;
    const bool ffPressed = (input.pressed & Pad_FastForward) != 0;
    if (fastForwardToggleMode_) {
        if (ffPressed) {
            fastForwardToggle_ = !fastForwardToggle_;
        }
    }
    fastForward_ = !overlayVisible && (fastForwardToggleMode_ ? fastForwardToggle_ : ffHeld);
    if (audio_)
        audio_->SetFastForward(fastForward_);
    if (core_)
    {
        if (overlayVisible)
        {
            core_->ClearInputs();
            core_->Pause();
        }
        else
        {
            core_->Resume();
            if (consumed || inputSuppressFrames_ > 0)
            {
                core_->ClearInputs();
                if (inputSuppressFrames_ > 0)
                {
                    --inputSuppressFrames_;
                    // Renew while the confirm/back button is still held so the
                    // release does not bleed into the game.
                    if (input.buttons & (Pad_A | Pad_B))
                        inputSuppressFrames_ = 3;
                }
            }
            else
            {
                ApplyCoreInput(input);
            }
        }
    }
}

void FlycastRuntime::RunFrame()
{
    UpdateScreenMode();

    // Play time: accumulate wall time while the game is actually running;
    // menu open pauses the session so menu time is not counted.
    const uint32_t nowTicks = SDL_GetTicks();
    if (overlay_ && overlay_->IsVisible())
        playTimeLastTicks_ = 0;
    else
    {
        if (playTimeLastTicks_ != 0)
        {
            const uint32_t delta = nowTicks - playTimeLastTicks_;
            if (delta <= 1000)
                sessionPlayFraction_ += (double)delta / 1000.0;
        }
        playTimeLastTicks_ = nowTicks;
    }
    static uint32_t loggedFrames = 0;
    const bool traceFrame = loggedFrames < 3;
    if (traceFrame)
        LOG_INFO("FRAME", "frame %u begin", loggedFrames);
    frameInFlight_ = GBAStationVulkan::BeginFrame();
    if (traceFrame)
        LOG_INFO("FRAME", "frame %u acquire=%d", loggedFrames, frameInFlight_ ? 1 : 0);
    if (frameInFlight_ && core_)
    {
        if (traceFrame)
            LOG_INFO("FRAME", "frame %u retro_run begin", loggedFrames);
        if (overlay_ && overlay_->IsVisible())
            core_->ApplyPendingOptions();
        else
            core_->RunFrame();
        if (traceFrame)
            LOG_INFO("FRAME", "frame %u retro_run complete", loggedFrames);
        // The frontend owns pacing through SDL audio.  While fast-forwarding
        // audio is deliberately dropped, so execute extra core frames for the
        // configured multiplier (1x => none, 2x => one extra frame, ...).
        if (fastForward_ && !(overlay_ && overlay_->IsVisible()))
        {
            const int extra = static_cast<int>(fastForwardMultiplier_) - 1;
            for (int i = 0; i < extra; ++i)
            {
                core_->RunFrame();
            }
        }
        bool swapSucceeded = false;
        if (core_->ConsumeDiskSwapResult(swapSucceeded))
        {
            if (swapSucceeded && !pendingDiscPath_.empty())
            {
                RecordSuccessfulDiscSwap(pendingDiscPath_);
                if (pendingDiscStateSlot_ >= 0)
                    core_->LoadState(FlycastStatePath(core_.get(), pendingDiscStateSlot_, savePath_));
            }
            else if (!swapSucceeded)
            {
                LOG_WARN("CORE", "Disc swap failed; keeping the previous session metadata");
            }
            pendingDiscPath_.clear();
            pendingDiscStateSlot_ = -1;
        }
    }
    if (traceFrame)
        ++loggedFrames;
}

void FlycastRuntime::RenderFrame()
{
    if (!frameInFlight_)
        return;

    const uint32_t now = SDL_GetTicks();
    float deltaTime = static_cast<float>(now - lastTicks_) / 1000.0f;
    lastTicks_ = now;
    if (deltaTime <= 0.0f || deltaTime > 0.25f)
        deltaTime = 1.0f / 60.0f;

    static uint32_t loggedRenderFrames = 0;
    const bool traceRender = loggedRenderFrames < 3;
    if (traceRender)
        LOG_INFO("FRAME", "render %u overlay begin", loggedRenderFrames);
    // The frame the menu was just opened: skip the overlay render so the
    // captured backbuffer stays pure gameplay; the menu shows next frame.
    if (!m_menuPendingThumb_)
        RenderOverlayFrame(deltaTime);
    if (traceRender)
        LOG_INFO("FRAME", "render %u overlay complete", loggedRenderFrames);

    // Apply the overlay's screen-size / display-mode selection to the game blit.
    // The game image is composited by GBAStationVulkan (not ImGui), so the destination
    // rect has to be handed to it here; otherwise it always fills the screen.
    if (overlay_)
    {
        uint32_t sw = 0, sh = 0;
        GBAStationVulkan::GetSwapExtent(sw, sh);
        if (sw > 0 && sh > 0)
        {
            const float coreAspect = core_ ? core_->GetAspectRatio() : (4.0f / 3.0f);
            float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
            overlay_->GetGameViewport(static_cast<float>(sw), static_cast<float>(sh),
                                      coreAspect, vx, vy, vw, vh);
            GBAStationVulkan::SetGameViewport(static_cast<int>(vx + 0.5f), static_cast<int>(vy + 0.5f),
                                        static_cast<int>(vw + 0.5f), static_cast<int>(vh + 0.5f));
        }
    }
    else
    {
        GBAStationVulkan::SetGameViewport(0, 0, 0, 0); // full screen
    }

    if (traceRender)
        LOG_INFO("FRAME", "render %u viewport complete", loggedRenderFrames);
    if (traceRender)
        LOG_INFO("FRAME", "render %u present begin", loggedRenderFrames);
    GBAStationVulkan::EndFrame();
    if (traceRender)
    {
        LOG_INFO("FRAME", "render %u present complete", loggedRenderFrames);
        ++loggedRenderFrames;
    }
    frameInFlight_ = false;

    // Menu-open thumbnail: after the pure-gameplay frame was presented, copy
    // it into memory for the state thumbnail.
    if (m_menuPendingThumb_)
    {
        CaptureMenuThumbnailToMemory();
        m_menuPendingThumb_ = false;
    }
}

// Bilinear scale an RGBA image so its width is at most kThumbMaxWidth.
// Thumbnails are menu previews; keeping them small keeps PNGs well under
// 500 KB even at high internal resolutions.
static void ScaleRgbaForThumb(const uint8_t *src, int sw, int sh, std::vector<uint8_t> &dst, int &dw, int &dh)
{
    const int maxW = 320;
    if (sw <= maxW)
    {
        dst.assign(src, src + static_cast<size_t>(sw) * sh * 4);
        dw = sw;
        dh = sh;
        return;
    }
    dw = maxW;
    dh = std::max(1, static_cast<int>(static_cast<double>(sh) * maxW / sw + 0.5));
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    const double sx = static_cast<double>(sw) / dw;
    const double sy = static_cast<double>(sh) / dh;
    for (int y = 0; y < dh; ++y)
    {
        double fy = (y + 0.5) * sy - 0.5;
        int y0 = static_cast<int>(fy);
        if (y0 < 0)
            y0 = 0;
        int y1 = y0 + 1;
        if (y1 >= sh)
            y1 = sh - 1;
        const double ty = fy - y0;
        const uint8_t *row0 = src + static_cast<size_t>(y0) * sw * 4;
        const uint8_t *row1 = src + static_cast<size_t>(y1) * sw * 4;
        uint8_t *out = dst.data() + static_cast<size_t>(y) * dw * 4;
        for (int x = 0; x < dw; ++x)
        {
            double fx = (x + 0.5) * sx - 0.5;
            int x0 = static_cast<int>(fx);
            if (x0 < 0)
                x0 = 0;
            int x1 = x0 + 1;
            if (x1 >= sw)
                x1 = sw - 1;
            const double tx = fx - x0;
            for (int c = 0; c < 4; ++c)
            {
                const double v = (1.0 - tx) * (1.0 - ty) * row0[x0 * 4 + c] +
                                 tx * (1.0 - ty) * row0[x1 * 4 + c] +
                                 (1.0 - tx) * ty * row1[x0 * 4 + c] +
                                 tx * ty * row1[x1 * 4 + c];
                out[x * 4 + c] = static_cast<uint8_t>(v + 0.5);
            }
        }
    }
}
void FlycastRuntime::CaptureMenuThumbnailToMemory()
{
#ifdef __SWITCH__
    // NVK/VI cannot safely re-submit a swapchain image that has already been
    // handed to present. The old readback raced the presentation engine and
    // poisoned the shared queue; the next overlay texture upload (typically a
    // newly selected mask) then failed with VK_ERROR_DEVICE_LOST. State
    // thumbnails are optional, so keep the GPU path stable until capture is
    // implemented from a dedicated offscreen image.
    m_thumbMemory_.clear();
    m_thumbW_ = 0;
    m_thumbH_ = 0;
    LOG_INFO("CORE", "Menu thumbnail capture skipped on Switch Vulkan");
    return;
#endif
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!GBAStationVulkan::CaptureCurrentFrameRGBA(rgba, w, h) || w == 0 || h == 0)
    {
        LOG_WARN("CORE", "Menu thumbnail capture failed");
        return;
    }
    m_thumbMemory_ = std::move(rgba);
    m_thumbW_ = w;
    m_thumbH_ = h;
    LOG_INFO("CORE", "Menu thumbnail captured %ux%u", w, h);
}

void FlycastRuntime::WriteStateThumbnailFromMemory(const std::string &statePath)
{
    if (m_thumbMemory_.empty() || m_thumbW_ == 0 || m_thumbH_ == 0)
    {
        LOG_WARN("CORE", "State thumbnail: no captured frame in memory for %s", statePath.c_str());
        return;
    }
    // Downscale to a compact thumbnail before encoding.
    std::vector<uint8_t> scaled;
    int w = 0, h = 0;
    ScaleRgbaForThumb(m_thumbMemory_.data(), m_thumbW_, m_thumbH_, scaled, w, h);
    LOG_INFO("CORE", "State thumbnail written %ux%u for %s", w, h, statePath.c_str());

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(w) * (h + 1) * 3 / 2);
    for (int y = 0; y < h; ++y)
    {
        raw.push_back(0); // filter: None
        // Force opaque alpha: the swapchain readback alpha may be 0.
        const uint8_t *row = scaled.data() + static_cast<std::size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x)
        {
            const uint8_t *p = row + static_cast<std::size_t>(x) * 4;
            raw.push_back(p[0]);
            raw.push_back(p[1]);
            raw.push_back(p[2]);
            raw.push_back(255);
        }
    }

    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()),
                  Z_BEST_COMPRESSION) != Z_OK)
        return;
    compressed.resize(compressedSize);

    FILE *fp = fopen((statePath + ".png").c_str(), "wb");
    if (!fp)
        return;
    auto writeU32 = [&](uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                              static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        fwrite(b, 1, 4, fp);
    };
    auto writeChunk = [&](const char tag[4], const uint8_t *data, uint32_t len) {
        writeU32(len);
        fwrite(tag, 1, 4, fp);
        fwrite(data, 1, len, fp);
        uint32_t crc = crc32(0, reinterpret_cast<const uint8_t *>(tag), 4);
        if (len)
            crc = crc32(crc, data, len);
        writeU32(crc);
    };

    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(signature, 1, 8, fp);

    uint8_t ihdr[13];
    ihdr[0] = static_cast<uint8_t>(w >> 24); ihdr[1] = static_cast<uint8_t>(w >> 16);
    ihdr[2] = static_cast<uint8_t>(w >> 8);  ihdr[3] = static_cast<uint8_t>(w);
    ihdr[4] = static_cast<uint8_t>(h >> 24); ihdr[5] = static_cast<uint8_t>(h >> 16);
    ihdr[6] = static_cast<uint8_t>(h >> 8);  ihdr[7] = static_cast<uint8_t>(h);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    writeChunk("IHDR", ihdr, sizeof(ihdr));
    writeChunk("IDAT", compressed.data(), static_cast<uint32_t>(compressed.size()));
    writeChunk("IEND", nullptr, 0);
    fclose(fp);
}

void FlycastRuntime::Shutdown()
{
    LOG_INFO("HOME", "Shutting down");
    if (autoSaveOnExitSlot_ > 0 && core_)
    {
        const int slot = std::clamp(autoSaveOnExitSlot_ - 1, 0, 9);
        const std::string statePath = FlycastStatePath(core_.get(), slot, savePath_);
        LOG_INFO("HOME", "GBAStation auto save on exit slot=%d path=%s", slot, statePath.c_str());
        core_->SaveState(statePath);
    }
    SaveFlycastPlayStats(romPath_);
    ShutdownOverlay();
    core_.reset();
    GBAStationVulkan::Shutdown();

    if (audio_)
        audio_->Shutdown();
    s_audio = nullptr;
    if (!GBAStationConfig::USE_SDLQUEUEAUDIO)
        Mix_CloseAudio();
    SDL_Quit();
}

namespace
{
void WriteFlycastConfigValue(const char *key, const std::string &value)
{
    const char *paths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
    std::string cfgPath;
    for (const char *path : paths)
    {
        std::ifstream in(path);
        if (in.good())
        {
            cfgPath = path;
            break;
        }
    }
    if (cfgPath.empty())
        return;

    std::vector<std::string> lines;
    {
        std::ifstream in(cfgPath);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
    }

    const std::string keyPrefix = std::string(key) + "=";
    const std::string encoded = "s|" + value;
    bool replaced = false;
    for (std::string &line : lines)
    {
        if (line.compare(0, keyPrefix.size(), keyPrefix) == 0)
        {
            line = keyPrefix + encoded;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        lines.push_back(keyPrefix + encoded);

    std::ofstream out(cfgPath, std::ios::trunc);
    if (!out)
        return;
    for (const std::string &line : lines)
        out << line << "\n";
}
}  // namespace

void FlycastRuntime::SetFastForwardMultiplier(float multiplier)
{
    fastForwardMultiplier_ = std::clamp(multiplier, 0.5f, 5.0f);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", fastForwardMultiplier_);
    WriteFlycastConfigValue("fastforward.multiplier", buf);
}

void FlycastRuntime::SetFastForwardToggleMode(bool toggleMode)
{
    fastForwardToggleMode_ = toggleMode;
    if (!toggleMode)
        fastForwardToggle_ = false;
    WriteFlycastConfigValue("fastforward.mode", toggleMode ? "toggle" : "hold");
}

void FlycastRuntime::LoadDiscSession()
{
    knownDiscs_.clear();
    activeDiscPath_ = launchDiscPath_;
    lastActiveDiscPath_ = launchDiscPath_;
    if (launchDiscPath_.empty())
        return;

    bool changed = false;
    const std::string sessionPath = FlycastSessionPath(launchDiscPath_, savePath_);
    std::ifstream in(sessionPath);
    bool sessionLoaded = false;
    if (in)
    {
        try
        {
            nlohmann::json session;
            in >> session;
            if (session.is_object())
            {
                // activeDisc was used by the first metadata revision. Keep it
                // as a read-compatible fallback for existing session files.
                lastActiveDiscPath_ = session.value("lastActiveDisc",
                                                   session.value("activeDisc", launchDiscPath_));
                const auto discs = session.find("knownDiscs");
                if (discs != session.end() && discs->is_array())
                {
                    for (const auto &item : *discs)
                    {
                        if (!item.is_object())
                            continue;
                        const std::string discPath = item.value("path", std::string());
                        if (discPath.empty())
                            continue;
                        knownDiscs_.push_back({discPath, item.value("label", std::string())});
                    }
                }
            }
            sessionLoaded = true;
        }
        catch (...)
        {
            LOG_WARN("HOME", "Ignoring invalid disc session metadata: %s", sessionPath.c_str());
            knownDiscs_.clear();
            activeDiscPath_ = launchDiscPath_;
        }
    }

    const auto hasDisc = [this](const std::string &path) {
        return std::any_of(knownDiscs_.begin(), knownDiscs_.end(), [&path](const KnownDisc &disc) {
            return disc.path == path;
        });
    };
    if (!hasDisc(launchDiscPath_))
    {
        knownDiscs_.insert(knownDiscs_.begin(), {launchDiscPath_, "Disc 1"});
        changed = true;
    }
    if (!hasDisc(lastActiveDiscPath_))
    {
        lastActiveDiscPath_ = launchDiscPath_;
        changed = true;
    }
    // A successful JSON read normally leaves the stream at EOF.  Do not use
    // the stream's current state here: EOF would make every launch look like
    // a missing session file and cause an unnecessary rewrite.
    if (changed || !sessionLoaded)
        SaveDiscSession();
    LOG_INFO("HOME", "Disc session %s: %s (%zu known discs, last=%s)",
             sessionLoaded ? "loaded" : "created", sessionPath.c_str(),
             knownDiscs_.size(), lastActiveDiscPath_.c_str());
}

void FlycastRuntime::SaveDiscSession() const
{
    if (launchDiscPath_.empty())
        return;

    nlohmann::json session;
    session["version"] = 1;
    session["launchDisc"] = launchDiscPath_;
    // The next launch begins with launchDisc. This field records the disc that
    // was last mounted at the end of the prior session, for the future resume
    // flow; it is deliberately distinct from this runtime's activeDiscPath_.
    session["lastActiveDisc"] = lastActiveDiscPath_;
    session["knownDiscs"] = nlohmann::json::array();
    for (const auto &disc : knownDiscs_)
        session["knownDiscs"].push_back({{"path", disc.path}, {"label", disc.label}});

    const std::string path = FlycastSessionPath(launchDiscPath_, savePath_);
    const std::string tempPath = path + ".tmp";
    std::ofstream out(tempPath, std::ios::trunc);
    if (!out)
    {
        LOG_WARN("HOME", "Unable to save disc session metadata: %s", tempPath.c_str());
        return;
    }
    out << session.dump(2);
    out.close();
    if (std::rename(tempPath.c_str(), path.c_str()) != 0)
    {
        LOG_WARN("HOME", "Unable to replace disc session metadata: %s", path.c_str());
        std::remove(tempPath.c_str());
        return;
    }
    LOG_INFO("HOME", "Saved disc session: %s (%zu known discs)", path.c_str(), knownDiscs_.size());
}

void FlycastRuntime::RecordSuccessfulDiscSwap(const std::string &path)
{
    activeDiscPath_ = path;
    lastActiveDiscPath_ = path;
    const auto found = std::find_if(knownDiscs_.begin(), knownDiscs_.end(), [&path](const KnownDisc &disc) {
        return disc.path == path;
    });
    if (found == knownDiscs_.end())
        knownDiscs_.push_back({path, "Disc " + std::to_string(knownDiscs_.size() + 1)});
    SaveDiscSession();
    LOG_INFO("CORE", "Registered active disc in session metadata: %s", path.c_str());
}

std::vector<FlycastRuntime::KnownDisc> FlycastRuntime::GetKnownDiscs() const
{
    return knownDiscs_;
}

bool FlycastRuntime::HasPreviousDiscSession() const
{
    if (lastActiveDiscPath_.empty() || lastActiveDiscPath_ == launchDiscPath_)
        return false;
    struct stat st;
    return stat(lastActiveDiscPath_.c_str(), &st) == 0;
}

bool FlycastRuntime::CanRestorePreviousDiscSession() const
{
    if (!HasPreviousDiscSession() || autoLoadStateSlot_ <= 0 || !core_)
        return false;
    const int slot = std::clamp(autoLoadStateSlot_ - 1, 0, 9);
    struct stat st;
    return stat(FlycastStatePath(core_.get(), slot, savePath_).c_str(), &st) == 0;
}

void FlycastRuntime::ResumeLastDiscSession()
{
    if (!core_ || !HasPreviousDiscSession())
        return;
    pendingDiscPath_ = lastActiveDiscPath_;
    pendingDiscStateSlot_ = CanRestorePreviousDiscSession()
        ? std::clamp(autoLoadStateSlot_ - 1, 0, 9)
        : -1;
    if (!core_->SwapDiskByPath(lastActiveDiscPath_))
    {
        pendingDiscPath_.clear();
        pendingDiscStateSlot_ = -1;
        LOG_WARN("CORE", "Unable to start restoration of the previous disc session");
    }
}

void FlycastRuntime::LoadFlycastPlayStats(const std::string &romPath)
{
    playCount_ = 0;
    playTimeTotal_ = 0;
    playStatsFound_ = false;
    savePath_.clear();
    gameDisplayMode_ = -1;
    gameScreenLayout_.clear();
    gameInternalResolution_.clear();
    gameMaskEnabled_ = false;
    gameMaskPath_.clear();
    gameShaderEnabled_ = false;
    gameShaderPath_.clear();
    if (romPath.empty())
        return;

    const char *dbPaths[] = {
        "sdmc:/GBAStation/data/GameData_DC.json",
        "/GBAStation/data/GameData_DC.json",
    };
    const std::string normalized = NormalizeFlycastRomPath(romPath);
    for (const char *dbPath : dbPaths)
    {
        std::ifstream file(dbPath, std::ios::binary);
        if (!file)
            continue;
        try
        {
            nlohmann::json data;
            file >> data;
            if (!data.is_array())
                continue;
            for (auto &item : data)
            {
                if (!item.is_object())
                    continue;
                const std::string itemPath = item.value("path", std::string());
                if (itemPath != romPath && NormalizeFlycastRomPath(itemPath) != normalized)
                    continue;
                playStatsFound_ = true;
                playCount_ = item.value("playCount", 0) + 1;
                playTimeTotal_ = item.value("playTime", 0);
                savePath_ = item.value("savePath", std::string());
                gameDisplayMode_ = item.value("displayMode", -1);
                gameScreenLayout_ = item.value("ndsScreenLayout", std::string());
                if (const auto it = item.find("reicastInternalResolution");
                    it != item.end() && it->is_string()) {
                    gameInternalResolution_ = it->get<std::string>();
                } else if (const auto legacy = item.find("ndsInternalResolution");
                           legacy != item.end() && legacy->is_number_integer()) {
                    // Older generic GameDB entries store a multiplier.  Map it
                    // to the exact libretro option spelling instead of relying
                    // on an unsafe JSON string conversion.
                    static constexpr const char *kResolutionValues[] = {
                        "320x240", "640x480", "960x720", "1280x960", "1920x1440"};
                    const int index = std::clamp(legacy->get<int>() - 1, 0, 4);
                    gameInternalResolution_ = kResolutionValues[index];
                }
                gameMaskEnabled_ = item.value("overlayEnabled", false);
                gameMaskPath_ = item.value("overlayPath", std::string());
                gameShaderEnabled_ = item.value("shaderEnabled", false);
                gameShaderPath_ = item.value("shaderPath", std::string());
                item["playCount"] = playCount_;
                // Close the read stream first: the Switch stdio/fs layer refuses a
                // second handle (write/trunc) on a file that is still open for read.
                file.close();
                std::ofstream out(dbPath, std::ios::trunc);
                if (out) {
                    out << data.dump(4);
                    LOG_INFO("HOME", "GBAStation play stats start playCount=%d playTime=%d", playCount_, playTimeTotal_);
                } else {
                    LOG_WARN("HOME", "GBAStation play stats write failed: %s", dbPath);
                }
                return;
            }
        }
        catch (...)
        {
            continue;
        }
    }
}

void FlycastRuntime::SaveFlycastDisplaySettings()
{
    if (!overlay_ || romPath_.empty())
        return;
    const char *dbPaths[] = {"sdmc:/GBAStation/data/GameData_DC.json", "/GBAStation/data/GameData_DC.json"};
    const std::string normalized = NormalizeFlycastRomPath(romPath_);
    for (const char *dbPath : dbPaths)
    {
        std::ifstream file(dbPath, std::ios::binary);
        if (!file)
            continue;
        try
        {
            nlohmann::json data;
            file >> data;
            if (!data.is_array())
                continue;
            for (auto &item : data)
            {
                const std::string itemPath = item.value("path", std::string());
                if (!item.is_object() || (itemPath != romPath_ && NormalizeFlycastRomPath(itemPath) != normalized))
                    continue;
                item["displayMode"] = overlay_->GetGameDisplayModeIndex();
                item["ndsScreenLayout"] = overlay_->GetGameScreenLayout();
                item["ndsIntegerScale"] = overlay_->GetGameDisplayModeIndex() == static_cast<int>(FlycastDisplayMode::Integer);
                if (overlayHost_)
                    item["reicastInternalResolution"] = overlayHost_->GetCoreOption("reicast_internal_resolution", "640x480");
                item["overlayEnabled"] = overlay_->IsMaskEnabled();
                item["overlayPath"] = overlay_->MaskPath();
                item["shaderEnabled"] = overlay_->IsShaderEnabled();
                item["shaderPath"] = overlay_->ShaderPath();
                file.close();
                std::ofstream out(dbPath, std::ios::trunc);
                if (out)
                    out << data.dump(4);
                return;
            }
        }
        catch (...)
        {
        }
    }
}

void FlycastRuntime::SaveFlycastPlayStats(const std::string &romPath)
{
    if (!playStatsFound_ || romPath.empty())
        return;
    if (sessionPlayFraction_ >= 0.5)
        ++sessionPlaySeconds_;
    const int totalPlayTime = playTimeTotal_ + std::max(0, sessionPlaySeconds_);
    const std::string lastPlayed = CurrentFlycastTimestamp();

    const char *dbPaths[] = {
        "sdmc:/GBAStation/data/GameData_DC.json",
        "/GBAStation/data/GameData_DC.json",
    };
    const std::string normalized = NormalizeFlycastRomPath(romPath);
    for (const char *dbPath : dbPaths)
    {
        std::ifstream file(dbPath, std::ios::binary);
        if (!file)
            continue;
        try
        {
            nlohmann::json data;
            file >> data;
            if (!data.is_array())
                continue;
            for (auto &item : data)
            {
                if (!item.is_object())
                    continue;
                const std::string itemPath = item.value("path", std::string());
                if (itemPath != romPath && NormalizeFlycastRomPath(itemPath) != normalized)
                    continue;
                item["playCount"] = playCount_;
                item["playTime"] = std::max(0, totalPlayTime);
                item["lastPlayed"] = lastPlayed;
                // Close the read stream first: the Switch stdio/fs layer refuses a
                // second handle (write/trunc) on a file that is still open for read.
                file.close();
                std::ofstream out(dbPath, std::ios::trunc);
                if (out) {
                    out << data.dump(4);
                    LOG_INFO("HOME", "GBAStation play stats exit playCount=%d playTime=%d lastPlayed=%s", playCount_, totalPlayTime, lastPlayed.c_str());
                } else {
                    LOG_WARN("HOME", "GBAStation play stats write failed: %s", dbPath);
                }
                return;
            }
        }
        catch (...)
        {
            continue;
        }
    }
}

}  // namespace GBAStation
