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
#include <cctype>
#include <cstdio>
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

    std::string GetGamePath() override { return core_ ? core_->GetGamePath() : std::string(); }
    bool IsGameLoaded() override { return core_ && core_->IsGameLoaded(); }

    bool StateSlotExists(int slot) override
    {
        struct stat st;
        return stat(StatePath(slot).c_str(), &st) == 0;
    }
    void SaveStateSlot(int slot) override
    {
        if (core_) core_->SaveState(StatePath(slot));
    }
    void LoadStateSlot(int slot) override
    {
        if (core_) core_->LoadState(StatePath(slot));
    }
    void SwapDisc(const std::string &path) override
    {
        if (core_) core_->SwapDiskByPath(path);
    }
    std::string GetCoreOption(const std::string &key, const std::string &fallback = "") override
    {
        return core_ ? core_->GetCoreOption(key, fallback) : fallback;
    }
    void SetCoreOption(const std::string &key, const std::string &value) override
    {
        if (core_) core_->SetCoreOption(key, value);
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
{
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

    // Flycast owns the core command buffers for the game frame.  Do not run
    // ImGui's Vulkan backend while the menu is closed: aside from doing
    // needless work this mixes a second render pass into the first boot frame.
    if (!overlay_->IsVisible())
    {
        GBAStationVulkan::SetOverlayDrawData(nullptr);
        return;
    }

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
        consumed = overlay_->HandleInput(input);
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
    fastForward_ = !overlayVisible && (input.buttons & Pad_FastForward) != 0;
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
        // audio is deliberately dropped, so execute one extra core frame.
        if (fastForward_ && !(overlay_ && overlay_->IsVisible()))
            core_->RunFrame();
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

}  // namespace GBAStation
