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
#include "GBAStationVulkan.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <SDL.h>
#include <SDL_mixer.h>

#include <algorithm>
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
        if (core_) core_->SwapDiskByPath(path);
    }
    void UpdateGamePath(const std::string &path) override
    {
        if (core_) core_->SetGamePath(path);
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
        const std::string romPath = core_ ? core_->GetGamePath() : std::string();

        // Game name = basename without extension.
        std::string name = romPath;
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);

        // System = the rom's parent dir (roms/<system>/<game>) -> states/<system>/.
        std::string system = "dc";
        if (slash != std::string::npos)
        {
            std::string dir = romPath.substr(0, slash);
            size_t slash2 = dir.find_last_of("/\\");
            if (slash2 != std::string::npos && slash2 + 1 < dir.size())
                system = dir.substr(slash2 + 1);
        }

        const std::string stateDir =
            std::string(GBAStation::Paths::StatesRoot) + "/" + system + "/";

        struct stat st;
        if (stat(GBAStation::Paths::StatesRoot, &st) == -1)
            mkdir(GBAStation::Paths::StatesRoot, 0777);
        if (stat(stateDir.c_str(), &st) == -1)
            mkdir(stateDir.c_str(), 0777);

        return stateDir + name + ".state" + std::to_string(slot);
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

// NAOMI/Atomiswave, detected by extension (matches flycast's libretro frontend).
static bool IsArcadePath(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".lst" || ext == ".bin" || ext == ".dat" || ext == ".zip" || ext == ".7z" ||
           ext == ".chd" || ext == ".gdi" || ext == ".cdi" || ext == ".cue" || ext == ".iso";
}

bool FlycastRuntime::Configure(const LaunchInfo &launch)
{
    romPath_ = launch.contentPath.empty() ? GBAStationConfig::TEST_ROM : launch.contentPath;
    titleArg_ = launch.title;
    isArcade_ = IsArcadePath(romPath_);
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
    LOG_INFO("HOME", "Loading ROM: %s", romPath_.c_str());

    if (!core_->LoadGame(romPath_))
    {
        LOG_ERROR("HOME", "LoadGame failed; idling");
    }
    else if (!InitOverlay(romPath_))
    {
        LOG_WARN("HOME", "Overlay init failed; continuing without GBAStation overlay");
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

        if (isArcade_)
        {
            // Arcade layout based on observed game actions:
            // Switch Y uses the low-kick source, Switch X uses the high-kick source.
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_B, down(Pad_A));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_A, down(Pad_B));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_X, down(Pad_X));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R, down(Pad_Y));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L, down(Pad_L));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_Y, down(Pad_R));
        }
        else
        {
            // Dreamcast follows physical disposition through the neutral pad map.
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_A, down(Pad_A));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_B, down(Pad_B));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_X, down(Pad_X));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_Y, down(Pad_Y));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L, down(Pad_L));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R, down(Pad_R));
        }
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_START, down(Pad_Start));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_SELECT, down(Pad_Select));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_UP, down(Pad_Up));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_DOWN, down(Pad_Down));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_LEFT, down(Pad_Left));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, down(Pad_Right));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L2, down(Pad_L2));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R2, down(Pad_R2));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L3, down(Pad_L3));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R3, down(Pad_R3));

        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, player.leftStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, player.leftStickY);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, player.rightStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, player.rightStickY);

        // Arcade games read directions from either the digital JVS stick
        // (fighters) or the analog axis (racers), so cross-feed d-pad <-> stick.
        // Dreamcast keeps its native d-pad + analog and is left alone.
        if (isArcade_)
        {
            // d-pad -> axis
            if (down(Pad_Left) || down(Pad_Right))
                core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                                      down(Pad_Left) ? -0x7fff : 0x7fff);
            if (down(Pad_Up) || down(Pad_Down))
                core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                                      down(Pad_Up) ? -0x7fff : 0x7fff);

            // stick -> d-pad (lenient threshold to keep diagonals)
            const int16_t kDirThreshold = 0x2800;
            if (player.leftStickX <= -kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_LEFT, true);
            if (player.leftStickX >= kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, true);
            if (player.leftStickY <= -kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_UP, true);
            if (player.leftStickY >= kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_DOWN, true);
        }
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
            if (consumed)
                core_->ClearInputs();
            else
                ApplyCoreInput(input);
        }
    }
}

void FlycastRuntime::RunFrame()
{
    UpdateScreenMode();
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

void FlycastRuntime::CaptureMenuThumbnailToMemory()
{
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
    const uint32_t w = m_thumbW_;
    const uint32_t h = m_thumbH_;
    LOG_INFO("CORE", "State thumbnail written %ux%u for %s", w, h, statePath.c_str());

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(w) * (h + 1) * 3 / 2);
    for (uint32_t y = 0; y < h; ++y)
    {
        raw.push_back(0); // filter: None
        raw.insert(raw.end(), m_thumbMemory_.begin() + static_cast<std::ptrdiff_t>(y) * w * 4,
                   m_thumbMemory_.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 4);
    }

    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()),
                  Z_BEST_SPEED) != Z_OK)
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

}  // namespace GBAStation
