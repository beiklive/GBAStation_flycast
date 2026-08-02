/// @file FlycastEntry.cpp
/// @brief Process entry point for the tico-integrated Flycast NRO. Mirrors
/// tico-ppsspp/tico/PpssppEntry.cpp: wire a log sink + the flycast runtime into
/// the generic Tico::Main driver.

#include "FlycastRuntime.h"
#include "TicoLogger.h"
#include "TicoMain.h"

#ifdef __SWITCH__
#include <switch.h>

extern "C"
{
    u32 __NvOptimusEnablement = 1;
    u32 __NvDeveloperOption = 1;
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;
}
#endif

int main(int argc, char *argv[])
{
#ifdef __SWITCH__
    Logger::Instance().OpenFile("sdmc:/GBAStation/debug/flycast_stub.log", true);
#endif
    Logger::Instance().EnableCategory("HOME", true);
    Logger::Instance().EnableCategory("VK", true);
    Logger::Instance().EnableCategory("CORE", true);
    Logger::Instance().EnableCategory("AUDIO", true);
    Logger::Instance().EnableCategory("LOADER", true);
    Logger::Instance().EnableCategory("OVERLAY", true);
    LOG_INFO("HOME", "flycast entry argc=%d build=20260802-switchvk-drmtrace-v8", argc);

    // Route the agnostic layer's log lines through the project macro logger.
    Tico::LogCallback log = [](const std::string &line) {
        LOG_INFO("HOME", "%s", line.c_str());
    };

    Tico::FlycastRuntime runtime(log);
    Tico::Main app(runtime, log);
    return app.Run(argc, argv);
}
