/// @file GBAStationChainload.cpp
#include "GBAStationChainload.h"

#include "GBAStationConfig.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include <switch/runtime/env.h>
#endif

namespace GBAStation
{
namespace
{
std::string g_returnPath;
std::string g_sessionToken;
}

void SetLauncherReturnPath(const char *path)
{
    if (path && path[0])
        g_returnPath = path;
}

bool HasLauncherReturnPath()
{
    return !g_returnPath.empty();
}

void SetExternalSessionToken(const char *token)
{
    if (token && token[0])
        g_sessionToken = token;
}

void ChainloadLauncher(const LogCallback &log)
{
#ifdef __SWITCH__
    const char *primary = g_returnPath.empty() ? Paths::LauncherNro : g_returnPath.c_str();
    const char *fallback = Paths::LauncherNroFallback;
    const char *target = nullptr;

    struct stat st;
    if (stat(primary, &st) == 0)
        target = primary;
    else if (stat(fallback, &st) == 0)
        target = fallback;

    if (target)
    {
        char args[512];
        if (!g_sessionToken.empty())
            std::snprintf(args, sizeof(args), "%s --external-return %s", target, g_sessionToken.c_str());
        else
            std::snprintf(args, sizeof(args), "%s --resume", target);
        envSetNextLoad(target, args);
        if (log)
            log(std::string("Chainloading back to ") + target);
    }
    else if (log)
    {
            log(std::string("No GBAStation launcher found at ") + primary + " or " + fallback);
    }
#else
    (void)log;
#endif
}

}  // namespace GBAStation
