/// @file GBAStationMain.cpp
/// @brief Emulator-agnostic GBAStation driver: Switch platform bring-up, the frame
/// loop, and libnx pad → FrameInput polling. Drives a GBAStation::CoreRuntime and
/// knows nothing about flycast/libretro/SDL. Mirrors GBAStation-ppsspp's GBAStationMain.cpp.

#include "GBAStationMain.h"

#include "GBAStationChainload.h"
#include "GBAStationConfig.h"

#include <cstdarg>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace GBAStation
{

namespace
{

std::unordered_map<std::string, std::string> g_configValues;

bool EndsWithNoCase(const std::string& text, const char* suffix)
{
    const std::size_t suffix_len = std::strlen(suffix);
    if (suffix_len > text.size())
        return false;
    return std::equal(suffix, suffix + suffix_len, text.end() - suffix_len,
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

bool IsNroPath(const std::string& value)
{
    return EndsWithNoCase(value, ".nro");
}

std::string Trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string DecodeConfigValue(std::string_view encoded)
{
    std::string value = Trim(encoded);
    if (value.size() > 2 && value[1] == '|' && value[0] == 's')
    {
        value.erase(0, 2);
        std::string decoded;
        decoded.reserve(value.size());
        bool escaped = false;
        for (char c : value)
        {
            if (escaped)
            {
                decoded.push_back(c);
                escaped = false;
            }
            else if (c == '\\')
                escaped = true;
            else
                decoded.push_back(c);
        }
        if (escaped)
            decoded.push_back('\\');
        return decoded;
    }
    return value;
}

void LoadGBAStationConfig()
{
    g_configValues.clear();
    const char* paths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
    for (const char* path : paths)
    {
        std::ifstream in(path);
        if (!in)
            continue;
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t equal = line.find('=');
            if (equal == std::string::npos)
                continue;
            g_configValues[Trim(std::string_view(line).substr(0, equal))] =
                DecodeConfigValue(std::string_view(line).substr(equal + 1));
        }
        break;
    }
}

u64 TokenHidMask(std::string_view token)
{
    const std::string t = Trim(token);
    if (t == "PAD_A") return HidNpadButton_A;
    if (t == "PAD_B") return HidNpadButton_B;
    if (t == "PAD_X") return HidNpadButton_X;
    if (t == "PAD_Y") return HidNpadButton_Y;
    if (t == "PAD_UP") return HidNpadButton_Up;
    if (t == "PAD_DOWN") return HidNpadButton_Down;
    if (t == "PAD_LEFT") return HidNpadButton_Left;
    if (t == "PAD_RIGHT") return HidNpadButton_Right;
    if (t == "PAD_LB") return HidNpadButton_L;
    if (t == "PAD_RB") return HidNpadButton_R;
    if (t == "PAD_LT" || t == "PAD_ZL") return HidNpadButton_ZL;
    if (t == "PAD_RT" || t == "PAD_ZR") return HidNpadButton_ZR;
    if (t == "PAD_START") return HidNpadButton_Plus;
    if (t == "PAD_BACK") return HidNpadButton_Minus;
    if (t == "PAD_LSB" || t == "PAD_L3") return HidNpadButton_StickL;
    if (t == "PAD_RSB" || t == "PAD_R3") return HidNpadButton_StickR;
    return 0;
}

u64 ParseComboMask(std::string_view combo)
{
    const std::string value = Trim(combo);
    if (value.empty() || value == "none")
        return 0;
    u64 mask = 0;
    std::size_t begin = 0;
    while (begin < value.size())
    {
        const std::size_t end = value.find('+', begin);
        const std::string_view token = std::string_view(value).substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin);
        mask |= TokenHidMask(token);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return mask;
}

bool BindingHeld(const char* key, const char* fallback, u64 held)
{
    const auto it = g_configValues.find(key);
    const std::string value = it == g_configValues.end() ? fallback : it->second;
    std::size_t begin = 0;
    while (begin <= value.size())
    {
        const std::size_t end = value.find('|', begin);
        const std::string_view combo = std::string_view(value).substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin);
        const u64 mask = ParseComboMask(combo);
        if (mask != 0 && (held & mask) == mask)
            return true;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}

#ifdef __SWITCH__
PadState g_pads[MaxPlayers];

/// Map libnx HidNpadButton bits to the positional PadButton scheme. Nintendo
/// physical layout (A=east, B=south, X=north, Y=west) is translated to SDL
/// positional (A=south, B=east, X=west, Y=north) so consumers behave the same
/// as on the SDL libretro path.
uint64_t MapButtons(u64 hid)
{
    uint64_t b = 0;
    if (BindingHeld("dc.handle.b", "PAD_B", hid))      b |= Pad_A;   // south
    if (BindingHeld("dc.handle.a", "PAD_A", hid))      b |= Pad_B;   // east
    if (BindingHeld("dc.handle.y", "PAD_Y", hid))      b |= Pad_X;   // west
    if (BindingHeld("dc.handle.x", "PAD_X", hid))      b |= Pad_Y;   // north
    if (BindingHeld("dc.handle.up", "PAD_UP", hid))    b |= Pad_Up;
    if (BindingHeld("dc.handle.down", "PAD_DOWN", hid)) b |= Pad_Down;
    if (BindingHeld("dc.handle.left", "PAD_LEFT", hid)) b |= Pad_Left;
    if (BindingHeld("dc.handle.right", "PAD_RIGHT", hid)) b |= Pad_Right;
    if (BindingHeld("dc.handle.l", "PAD_LB", hid))     b |= Pad_L;
    if (BindingHeld("dc.handle.r", "PAD_RB", hid))     b |= Pad_R;
    // The Dreamcast pad has no L2/R2/L3/R3; those bindings were copied from
    // another core and must not be parsed.
    if (BindingHeld("dc.handle.start", "PAD_START", hid)) b |= Pad_Start;
    if (BindingHeld("dc.handle.select", "PAD_BACK", hid)) b |= Pad_Select;

    // Frontend actions must use their own config keys.  Deriving the menu
    // hotkey from mapped Start/Select made the original Flycast shortcut win
    // whenever a user changed the launcher mapping.
    if (BindingHeld("dc.hotkey.menu.pad", "PAD_START+PAD_BACK", hid))
        b |= Pad_Guide;
    if (BindingHeld("dc.handle.fastforward", "PAD_LSB", hid))
        b |= Pad_FastForward;
    return b;
}
#endif

}  // namespace

float OverlayModeScale()
{
    // The overlay is laid out in the swapchain's native resolution (720p on
    // Switch) — scaling by operation mode was a dock/handheld misunderstanding
    // that blew the menu up to ~1080p and pushed it off-screen.  Return 1.0.
    return 1.0f;
}

Main::Main(CoreRuntime &runtime, LogCallback log)
    : runtime_(runtime), log_(std::move(log))
{
}

int Main::Run(int argc, char **argv)
{
    LoadGBAStationConfig();
    LaunchInfo launch{};
    launch.argc = argc;
    launch.argv = argv;
    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i])
            continue;
        const std::string argument{argv[i]};
        if (argument == "--return" && i + 1 < argc && argv[i + 1])
        {
            SetLauncherReturnPath(argv[++i]);
            continue;
        }
        if (argument == "--gbastation-session" && i + 1 < argc && argv[i + 1])
        {
            SetExternalSessionToken(argv[++i]);
            continue;
        }
        if (argument.rfind("--", 0) == 0 || IsNroPath(argument))
            continue;
        if (launch.contentPath.empty())
        {
            launch.contentPath = argument;
            continue;
        }
        if (launch.title.empty())
            launch.title = argument;
    }

    if (!InitPlatform())
        return 1;

    Log("GBAStation main start core=%s argc=%d content=%s", runtime_.Name(), argc,
        launch.contentPath.empty() ? "(default)" : launch.contentPath.c_str());

    bool started = runtime_.Configure(launch) && runtime_.Initialize(launch) &&
                   runtime_.LoadContent(launch.contentPath);
    if (!started)
    {
        Log("GBAStation main init failed core=%s", runtime_.Name());
        runtime_.Shutdown();
        ShutdownPlatform();
        return 1;
    }

    Log("GBAStation main loop enter core=%s", runtime_.Name());
    while (!runtime_.ShouldExit())
    {
#ifdef __SWITCH__
        if (!appletMainLoop())
            break;
#endif
        const FrameInput input = PollInput();
        runtime_.HandleInput(input);
        runtime_.RunFrame();
        runtime_.RenderFrame();
    }
    Log("GBAStation main loop exit core=%s", runtime_.Name());

    // Only chainload back to the launcher when it actually launched us
    // (--return present). Launched directly (hbmenu), exit without returning.
    const bool chainload = runtime_.ShouldChainloadLauncher() && HasLauncherReturnPath();
    runtime_.Shutdown();
    ShutdownPlatform();
    if (chainload)
        ChainloadLauncher(log_);
    return 0;
}

bool Main::InitPlatform()
{
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        Log("Unable to ignore SIGPIPE");

#ifdef __SWITCH__
    mkdir(Paths::Root, 0777);
    mkdir("sdmc:/GBAStation/DC", 0777);
    mkdir("sdmc:/GBAStation/bios", 0777);
    mkdir("sdmc:/GBAStation/saves", 0777);
    mkdir("sdmc:/GBAStation/saves/DC", 0777);
    mkdir("sdmc:/GBAStation/config", 0777);
    mkdir("sdmc:/GBAStation/config/cores", 0777);
    mkdir(Paths::Debug, 0777);
    mkdir(Paths::Assets, 0777);
    mkdir(Paths::Lang, 0777);
    mkdir(Paths::System, 0777);
    mkdir(Paths::SavesRoot, 0777);
    mkdir(Paths::StatesRoot, 0777);
    mkdir(Paths::CoreConfigDir, 0777);

    appletLockExit();
    exitLocked_ = true;

    if (R_SUCCEEDED(socketInitializeDefault()))
        socketReady_ = true;
    else
        Log("socketInitializeDefault failed");

    if (R_FAILED(romfsInit()))
    {
        Log("romfsInit failed");
        ShutdownPlatform();
        return false;
    }

    padConfigureInput(MaxPlayers, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pads[0]);
    for (unsigned player = 1; player < MaxPlayers; ++player)
        padInitialize(&g_pads[player], static_cast<HidNpadIdType>(HidNpadIdType_No1 + player));
#endif

    platformReady_ = true;
    return true;
}

void Main::ShutdownPlatform()
{
    platformReady_ = false;
#ifdef __SWITCH__
    romfsExit();
    if (socketReady_)
    {
        socketExit();
        socketReady_ = false;
    }
    if (exitLocked_)
    {
        appletUnlockExit();
        exitLocked_ = false;
    }
#endif
}

FrameInput Main::PollInput()
{
    FrameInput in{};
#ifdef __SWITCH__
    for (unsigned player = 0; player < MaxPlayers; ++player)
    {
        padUpdate(&g_pads[player]);

        PlayerInput &slot = in.players[player];
        const uint64_t rawButtons = padGetButtons(&g_pads[player]);
        slot.buttons = MapButtons(rawButtons);
        const HidAnalogStickState left = padGetStickPos(&g_pads[player], 0);
        const HidAnalogStickState right = padGetStickPos(&g_pads[player], 1);
        // libnx reports +Y up; convert to the SDL convention (+Y down) consumers use.
        slot.leftStickX = left.x;
        slot.leftStickY = -left.y;
        slot.rightStickX = right.x;
        slot.rightStickY = -right.y;
        slot.pressed = slot.buttons & ~prevButtons_[player];
        slot.released = prevButtons_[player] & ~slot.buttons;
        prevButtons_[player] = slot.buttons;
        if (player == 0)
        {
            in.rawButtons = rawButtons;
            in.rawPressed = rawButtons & ~prevRawButtons_[player];
        }
        prevRawButtons_[player] = rawButtons;
    }
#endif
    in.buttons = in.players[0].buttons;
    in.pressed = in.players[0].pressed;
    in.released = in.players[0].released;
    in.leftStickX = in.players[0].leftStickX;
    in.leftStickY = in.players[0].leftStickY;
    in.rightStickX = in.players[0].rightStickX;
    in.rightStickY = in.players[0].rightStickY;
    return in;
}

void Main::Log(const char *fmt, ...) const
{
    if (!log_ || !fmt)
        return;

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_(buffer);
}

}  // namespace GBAStation
