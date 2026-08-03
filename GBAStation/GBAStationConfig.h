/// @file GBAStationConfig.h
/// @brief Minimal hardcoded configuration for GBAStation overlay
#pragma once

#include <string>

namespace GBAStationConfig {
    // Hardcoded test ROM for easy testing
    constexpr const char* TEST_ROM = "sdmc:/GBAStation/DC/roms/Sonic Adventure (USA).cue";
    
    // Asset paths
    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";
    constexpr const char* IMAGES_PATH = "romfs:/images/";
    // NOTE: Flycast's libretro shell appends its own "dc/" subfolder to the
    // system directory (shell/libretro/libretro.cpp: game_dir = "<dir>/dc/"),
    // for Dreamcast, NAOMI and Atomiswave alike. So this must be the PARENT
    // dir; BIOS (dc_boot.bin, naomi.zip, awbios.zip) resolves to system/dc/.
    // (Passing ".../system/dc/" here would double it to ".../system/dc/dc/".)
    constexpr const char* SYSTEM_PATH = "sdmc:/GBAStation/bios/";
    constexpr const char* SAVES_PATH = "sdmc:/GBAStation/saves/DC/";
    constexpr const char* STATES_PATH = "sdmc:/GBAStation/saves/DC/";
    
    // Window settings
    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    // Audio backend configuration
    // true  = SDL_QueueAudio (Push model). Used on Flycast: the DC+Vulkan load
    //         starves the SDL_mixer pull-callback in Callback mode, causing
    //         choppy game audio. The RA trophy chime is mixed into this queue
    //         manually by GBAStationAudio (no second device, which the Switch can't do).
    // false = Mix_HookMusic + RingBuffer (Callback model, used by lighter cores).
    constexpr bool USE_SDLQUEUEAUDIO = true;
}

// Emulator-agnostic paths consumed by the generic GBAStation layer (GBAStation::Main,
// GBAStation::ChainloadLauncher). Core-specific subpaths (dc states/saves/system)
// stay in GBAStationConfig above and remain a flycast concern.
namespace GBAStation { namespace Paths {
    constexpr const char* Root              = "sdmc:/GBAStation";
    constexpr const char* Debug             = "sdmc:/GBAStation/debug";
    constexpr const char* Assets            = "sdmc:/GBAStation/DC/assets";
    constexpr const char* Lang              = "sdmc:/GBAStation/DC/lang";
    constexpr const char* System            = "sdmc:/GBAStation/bios";
    constexpr const char* SavesRoot         = "sdmc:/GBAStation/saves/DC";
    constexpr const char* StatesRoot        = "sdmc:/GBAStation/saves/DC";
    constexpr const char* CoreConfigDir     = "sdmc:/GBAStation/config/cores";
    constexpr const char* LauncherNro       = "sdmc:/switch/GBAStation.nro";
    constexpr const char* LauncherNroFallback = "sdmc:/GBAStation/GBAStation.nro";
    constexpr const char* DefaultTitleFont       = "romfs:/fonts/font.ttf";
    constexpr const char* DefaultDescriptionFont = "romfs:/fonts/description.ttf";
}}  // namespace GBAStation::Paths

// UI Actions for HelpersBar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
