/// @file GBAStationOverlay.cpp
/// @brief Overlay UI for GBAStation-integrated Flycast
/// Based on EmulatorScreen overlay rendering

// The overlay uses ImVec2 math operators. The libretro target defines this as a
// compile flag; the standalone target does not (and forcing it project-wide
// clashes with implot's own #define), so define it per-TU before imgui.h.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "GBAStationOverlay.h"
#include "GBAStationAudio.h"
#include "GBAStationConfig.h"
#include "GBAStationTranslationManager.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <string_view>

// Use relative path to json.hpp
#include "../core/deps/json/json.hpp"

#include "../core/deps/stb/stb_image.h"

#include <sys/stat.h>
#include <string>

static float OverlayScale()
{
    const float scale = ImGui::GetIO().FontGlobalScale;
    return scale > 0.0f ? scale : 1.0f;
}

static std::string TrimConfigValue(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

static std::string DecodeConfigValue(std::string_view encoded)
{
    std::string value = TrimConfigValue(encoded);
    if (value.size() > 2 && value[1] == '|' && value[0] == 's')
    {
        std::string decoded;
        decoded.reserve(value.size() - 2);
        bool escaped = false;
        for (std::size_t i = 2; i < value.size(); ++i)
        {
            const char c = value[i];
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

static std::string ReadGBAStationConfigValue(const char *key)
{
    const char *paths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
    for (const char *path : paths)
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
            if (TrimConfigValue(std::string_view(line).substr(0, equal)) == key)
                return DecodeConfigValue(std::string_view(line).substr(equal + 1));
        }
        break;
    }
    return {};
}

static void CycleFlycastOption(IOverlayHost *host, const char *key,
                               std::initializer_list<const char *> values, int direction)
{
    if (!host || values.size() == 0)
        return;
    const std::string current = host->GetCoreOption(key, *values.begin());
    int index = 0;
    int candidate = 0;
    bool found = false;
    for (const char *value : values)
    {
        if (current == value)
        {
            index = candidate;
            found = true;
            break;
        }
        ++candidate;
    }
    const int count = static_cast<int>(values.size());
    // The core reports an unset option as "默认" which is not part of the
    // selectable list.  Treat it as "before the first item" so the first
    // Right press lands on the first real option (and Left wraps to the last),
    // and once changed it can never fall back to the phantom "默认".
    if (!found)
        index = direction >= 0 ? -1 : count;
    index = (index + direction + count) % count;
    auto value = values.begin();
    std::advance(value, index);
    host->SetCoreOption(key, *value);
}

#ifdef __SWITCH__
#include <switch.h>
#endif

// Nanosvg for vector icons. Backend-agnostic now: the overlay decodes to RGBA
// and uploads through IOverlayHost::CreateTextureRGBA (GBAStationVulkan on libretro,
// imguiDriver on the standalone), so there is no GL/Vulkan #ifdef here.
#define NANOSVG_IMPLEMENTATION
#include "deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "deps/nanosvg/nanosvgrast.h"

// Save-state slot handling now lives behind IOverlayHost (libretro builds a
// per-ROM .state path + calls GBAStationCore; the standalone uses flycast's native
// save states), so the overlay no longer constructs paths itself.

//==============================================================================
// UI Style Helpers (simplified from UIStyle.h)
//==============================================================================

namespace UIStyle
{

    // Draw text with shadow
    inline void DrawTextWithShadow(ImDrawList *dl, ImVec2 pos, ImU32 color,
                                   const char *text, float shadowOffset = 1.5f)
    {
        ImU32 shadowColor = IM_COL32(0, 0, 0, 50);
        dl->AddText(ImVec2(pos.x + shadowOffset, pos.y + shadowOffset), shadowColor, text);
        dl->AddText(pos, color, text);
    }

    // Draw switch button prompt - Forced Dark Mode Logic (Filled Style)
    static void DrawSwitchButton(ImDrawList *dl, ImFont *font, float fontSize, ImVec2 center, float size, const char *symbol, float alpha, bool isDark)
    {
        // Dark Mode = Filled Light Button with Dark Text
        ImU32 fillCol = IM_COL32(220, 220, 220, (int)(255 * alpha)); // Light Grey Fill
        ImU32 textCol = IM_COL32(40, 40, 40, (int)(255 * alpha));    // Dark Text

        // Filled Circle
        dl->AddCircleFilled(center, size * 0.5f, fillCol, 12);

        // Text inside
        float symSize = fontSize * 0.75f;
        ImVec2 textSize = font->CalcTextSizeA(symSize, FLT_MAX, 0.0f, symbol);

        dl->AddText(font, symSize, center - (textSize * 0.5f), textCol, symbol);
    }

} // namespace UIStyle

// Encode a codepoint as UTF-8 into a 4-byte buffer (icon glyphs).
void EncodeUtf8(char *out, int codepoint)
{
    if (codepoint <= 0x7F)
    {
        out[0] = (char)codepoint;
        out[1] = '\0';
    }
    else if (codepoint <= 0x7FF)
    {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        out[2] = '\0';
    }
    else
    {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        out[3] = '\0';
    }
}

//==============================================================================
// Construction
//==============================================================================

GBAStationOverlay::GBAStationOverlay()
{
    m_gameTitle = "Flycast";
    LoadConfig();
    LoadGeneralConfig();
    LoadCoreSettings();
    GBAStationTranslationManager::Instance().Init();

    // Battery/account UI was part of the removed Tico chrome.  Do not open
    // extra Switch services while the Flycast Vulkan context is coming up.
}

GBAStationOverlay::~GBAStationOverlay()
{
    ReleaseAvatarTexture();
    ReleaseFocusTexture();
}

void GBAStationOverlay::ReleaseFocusTexture()
{
    if (m_focusTexture && m_host)
        m_host->DestroyTexture(m_focusTexture);
    m_focusTexture = 0;
    m_focusTextureWidth = 0;
    m_focusTextureHeight = 0;
}

void GBAStationOverlay::LoadFocusTexture()
{
    if (m_focusTexture || !m_host)
        return;
#ifdef __SWITCH__
    const char *path = "romfs:/assets/ui/border_gradient.png";
#else
    const char *path = "GBAStation/assets/ui/border_gradient.png";
#endif
    int width = 0, height = 0, channels = 0;
    unsigned char *rgba = stbi_load(path, &width, &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0)
    {
        if (rgba)
            stbi_image_free(rgba);
        return;
    }
    m_focusTexture = m_host->CreateTextureRGBA(rgba, width, height);
    stbi_image_free(rgba);
    if (m_focusTexture)
    {
        m_focusTextureWidth = width;
        m_focusTextureHeight = height;
    }
}

void GBAStationOverlay::DrawFocusBorder(ImVec2 min, ImVec2 max, float thickness)
{
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    const float x = min.x, y = min.y, w = max.x - min.x, h = max.y - min.y;
    const float rounding = 0.0f;
    if (m_focusTexture)
    {
        // Animated flowing gradient: advance a UV window around the border so
        // the highlight travels, matching the 3DS menu's FlowBorder.  The four
        // straight edge quads get a rounded-corner cap on top so the focus
        // frame follows the cell's rounded corners.
        const float borderWidth = std::max(4.0f, thickness * 2.0f);
        const double milliseconds = static_cast<double>(SDL_GetTicks64());
        float uv = static_cast<float>(std::fmod(milliseconds / 3600.0, 1.0));
        const float topLength = w + borderWidth * 2.0f;
        const float sideLength = h;
        const float advance = 1.0f / 256.0f;
        float next = uv + topLength * advance;
        fg->AddImage(m_focusTexture, ImVec2(x - borderWidth, y - borderWidth),
                     ImVec2(x + w + borderWidth, y),
                     ImVec2(uv, 0.0f), ImVec2(next, 1.0f));
        uv = next;
        next = uv + sideLength * advance;
        fg->AddImage(m_focusTexture, ImVec2(x + w, y), ImVec2(x + w + borderWidth, y + h),
                     ImVec2(uv, 0.0f), ImVec2(next, 1.0f));
        uv = next;
        next = uv + topLength * advance;
        fg->AddImage(m_focusTexture, ImVec2(x - borderWidth, y + h),
                     ImVec2(x + w + borderWidth, y + h + borderWidth),
                     ImVec2(next, 0.0f), ImVec2(uv, 1.0f));
        uv = next;
        next = uv + sideLength * advance;
        fg->AddImage(m_focusTexture, ImVec2(x - borderWidth, y), ImVec2(x, y + h),
                     ImVec2(next, 0.0f), ImVec2(uv, 1.0f));
        // Rounded corners: the flow quads meet at square corners; paint small
        // rounded corner patches in the focus accent to soften them.
        const ImU32 corner = IM_COL32(79, 179, 255, 255);
        fg->AddCircleFilled(ImVec2(x + rounding, y + rounding), rounding, corner, 16);
        fg->AddCircleFilled(ImVec2(x + w - rounding, y + rounding), rounding, corner, 16);
        fg->AddCircleFilled(ImVec2(x + rounding, y + h - rounding), rounding, corner, 16);
        fg->AddCircleFilled(ImVec2(x + w - rounding, y + h - rounding), rounding, corner, 16);
    }
    else
    {
        fg->AddRect(min, max, IM_COL32(79, 179, 255, 255), rounding, 0, 2.0f);
    }
}

void GBAStationOverlay::ReleaseAvatarTexture()
{
    if (m_avatarTexture && m_host)
        m_host->DestroyTexture(m_avatarTexture);
    m_avatarTexture = 0;
    m_avatarWidth = 0;
    m_avatarHeight = 0;
}

bool GBAStationOverlay::LoadAvatarTextureFromMemory(const unsigned char *data, size_t size, const char * /*tag*/)
{
    if (!data || size == 0)
        return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *rgba = stbi_load_from_memory(data, static_cast<int>(size),
                                                &width, &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0)
    {
        if (rgba)
            stbi_image_free(rgba);
        return false;
    }

    ReleaseAvatarTexture();
    ImTextureID texture = m_host ? m_host->CreateTextureRGBA(rgba, width, height) : 0;
    stbi_image_free(rgba);
    if (!texture)
        return false;
    m_avatarTexture = texture;
    m_avatarWidth = width;
    m_avatarHeight = height;
    return true;
}

bool GBAStationOverlay::LoadAvatarTextureFromFile(const char *path)
{
    if (!path)
        return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *rgba = stbi_load(path, &width, &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0)
    {
        if (rgba)
            stbi_image_free(rgba);
        return false;
    }

    ReleaseAvatarTexture();
    ImTextureID texture = m_host ? m_host->CreateTextureRGBA(rgba, width, height) : 0;
    stbi_image_free(rgba);
    if (!texture)
        return false;
    m_avatarTexture = texture;
    m_avatarWidth = width;
    m_avatarHeight = height;
    return true;
}

void GBAStationOverlay::LoadConfig()
{
    const char *configPaths[] = {
        "sdmc:/GBAStation/config/display.jsonc",
        "GBAStation/config/display.jsonc",
        "assets/config/display.jsonc",
        "../assets/config/display.jsonc"};

    // Default to dark
    m_isDarkMode = true;
    m_showNickname = false;

    FILE *fp = nullptr;
    for (const char *path : configPaths)
    {
        fp = fopen(path, "rb");
        if (fp)
            break;
    }

    // ... rest of LoadConfig ...
    if (fp)
    {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size > 0)
        {
            std::string content;
            content.resize(size);
            fread(&content[0], 1, size, fp);

            try
            {
                nlohmann::json j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded())
                {
                    // Check snake_case (standard) first, then camelCase fallback
                    if (j.contains("dark_mode") && j["dark_mode"].is_boolean())
                    {
                        m_isDarkMode = j["dark_mode"].get<bool>();
                    }
                    else if (j.contains("darkMode") && j["darkMode"].is_boolean())
                    {
                        m_isDarkMode = j["darkMode"].get<bool>();
                    }

                    if (j.contains("show_nickname") && j["show_nickname"].is_boolean())
                    {
                        m_showNickname = j["show_nickname"].get<bool>();
                    }
                    else if (j.contains("showNickname") && j["showNickname"].is_boolean())
                    {
                        m_showNickname = j["showNickname"].get<bool>();
                    }
                }
            }
            catch (...)
            {
                // Parse error
            }
        }
        fclose(fp);
    }
}

void GBAStationOverlay::LoadGeneralConfig()
{
    const char *configPaths[] = {
        "sdmc:/GBAStation/config/general.jsonc",
        "GBAStation/config/general.jsonc",
        "assets/config/general.jsonc",
        "../assets/config/general.jsonc"};

    m_hourFormat = "24h";

    FILE *fp = nullptr;
    for (const char *path : configPaths)
    {
        fp = fopen(path, "rb");
        if (fp)
            break;
    }

    if (fp)
    {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size > 0)
        {
            std::string content;
            content.resize(size);
            fread(&content[0], 1, size, fp);

            try
            {
                nlohmann::json j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded())
                {
                    if (j.contains("hour_format") && j["hour_format"].is_string())
                    {
                        m_hourFormat = j["hour_format"].get<std::string>();
                    }
                }
            }
            catch (...)
            {
            }
        }
        fclose(fp);
    }
}

void GBAStationOverlay::LoadAccountData()
{
    m_nickname = "Player 1";

#ifdef __SWITCH__
    const char *const avatarPaths[] = {
        "sdmc:/GBAStation/DC/assets/avatar.jpg",
        "sdmc:/GBAStation/DC/assets/avatar.jpeg",
        "sdmc:/GBAStation/DC/assets/avatar.png",
    };

    for (const char *path : avatarPaths)
    {
        if (LoadAvatarTextureFromFile(path))
            return;
    }

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc))
        return;

    AccountUid uid = {0};
    bool found = false;

    if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid))
        found = true;
    if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid))
        found = true;

    if (!found)
    {
        s32 userCount = 0;
        if (R_SUCCEEDED(accountGetUserCount(&userCount)) && userCount > 0)
        {
            AccountUid uids[ACC_USER_LIST_SIZE];
            s32 actualTotal = 0;
            if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actualTotal)) && actualTotal > 0)
            {
                uid = uids[0];
                found = accountUidIsValid(&uid);
            }
        }
    }

    if (found)
    {
        AccountProfile profile;
        AccountProfileBase profileBase;
        if (R_SUCCEEDED(accountGetProfile(&profile, uid)))
        {
            if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profileBase)))
            {
                std::string profileName(profileBase.nickname);
                if (!profileName.empty())
                    m_nickname = profileName;
            }

            u32 imageSize = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imageSize)) && imageSize > 0)
            {
                std::vector<unsigned char> jpegData(imageSize);
                u32 actualSize = 0;
                if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpegData.data(), imageSize, &actualSize)) && actualSize > 0)
                {
                    LoadAvatarTextureFromMemory(jpegData.data(), actualSize, "switch-account-avatar");
                }
            }
            accountProfileClose(&profile);
        }
    }
    accountExit();
#else
    const char *const avatarPaths[] = {
        "GBAStation/assets/avatar.jpg",
        "GBAStation/assets/avatar.jpeg",
        "GBAStation/assets/avatar.png",
        "assets/images/avatar.jpg",
        "assets/images/avatar.jpeg",
        "assets/images/avatar.png",
    };
    for (const char *path : avatarPaths)
    {
        if (LoadAvatarTextureFromFile(path))
            return;
    }
#endif
}

//==============================================================================
// Update
//==============================================================================

void GBAStationOverlay::Update(float deltaTime)
{
    // switchVK's first frame runs immediately after Flycast's Vulkan context
    // reset.  Texture uploads from here re-enter the renderer before its first
    // present and crash on hardware.  The menu has a vector focus fallback.
    // Pre-load RA icon texture BEFORE rendering (GL-safe: outside ImGui frame)
    EnsureRAIconLoaded();

    // Resolve any pending notification badge textures from cache (no GL calls)
    ResolveNotificationTextures();

    if (m_currentMenu != OverlayMenu::None)
    {
        m_animTimer += deltaTime;
        if (m_currentMenu == OverlayMenu::QuickMenu)
            m_quickMenuOpenTime += deltaTime;

    }
}

//==============================================================================
// Show/Hide
//==============================================================================

void GBAStationOverlay::Show()
{
    if (m_currentMenu == OverlayMenu::None)
    {
        m_currentMenu = OverlayMenu::QuickMenu;
        m_animTimer = 0.0f;
        m_quickMenuOpenTime = 0.0f;
        m_quickMenuSelection = 0;
        m_sidebarFocused = true;
        LoadConfig();        // Reload config on open
        LoadGeneralConfig(); // Reload hour format on open
        ScanForDiscs();      // Scan for multi-disc
    }
}

void GBAStationOverlay::Hide()
{
    m_currentMenu = OverlayMenu::None;
    // HandleInput() returns immediately while hidden, so any navigation key
    // released after save/load would otherwise remain latched forever.
    m_navHeldPrev = 0;
    m_navFireAtMs = 0;
    m_navStartMs = 0;
}

void GBAStationOverlay::ActivateTab(int tab)
{
    m_quickMenuSelection = std::clamp(tab, 0, 8);
    m_settingsSelection = 0;
    m_sidebarFocused = true;

    switch (m_quickMenuSelection)
    {
    case 1:
        m_isSaveMode = true;
        m_currentMenu = OverlayMenu::SaveStates;
        break;
    case 2:
        m_isSaveMode = false;
        m_currentMenu = OverlayMenu::SaveStates;
        break;
    case 3:
    case 4:
    case 5:
        m_currentMenu = OverlayMenu::Settings;
        break;
    default:
        m_currentMenu = OverlayMenu::QuickMenu;
        break;
    }
    m_animTimer = 0.4f;
}

//==============================================================================
// Rendering
//==============================================================================

void GBAStationOverlay::Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                         int frameWidth, int frameHeight, int fboWidth, int fboHeight)
{
    ImDrawList *bgDrawList = ImGui::GetBackgroundDrawList();
    ImDrawList *fgDrawList = ImGui::GetForegroundDrawList();

    // Always render the game
    RenderGame(bgDrawList, displaySize, gameTexture, aspectRatio, frameWidth, frameHeight, fboWidth, fboHeight);

    // HUD (FPS + fast forward badge) while playing; hidden with the menu open.
    if (m_currentMenu == OverlayMenu::None)
    {
        DrawHud(fgDrawList, displaySize);
    }

    // Render overlay if visible
    if (m_currentMenu != OverlayMenu::None)
    {
        RenderOverlayBackground(fgDrawList, displaySize);
        RenderGBAStationMenu(fgDrawList, displaySize);

        RenderHelpersBar(fgDrawList, displaySize);
    }

    // RA alerts always render (even during gameplay, not just when menu is open)
    RenderRAAlerts(fgDrawList, displaySize, ImGui::GetIO().DeltaTime);
}

void GBAStationOverlay::DrawHud(ImDrawList *dl, ImVec2 displaySize)
{
    if (!dl || displaySize.x <= 0.0f || displaySize.y <= 0.0f)
        return;
    const bool showFps = m_host && m_host->GetShowFps();
    const bool fastForward = m_host && m_host->GetFastForwardActive();
    if (!showFps && !fastForward)
        return;

    const float em = std::max(14.0f, std::round(displaySize.y / 32.0f));
    const float margin = std::round(em * 0.6f);
    const float pad = std::round(em * 0.35f);
    const float fontScale = em / 21.0f;

    std::string text;
    if (showFps)
    {
        char buf[24];
        const double fps = m_host ? m_host->GetCoreFps() : 0.0;
        std::snprintf(buf, sizeof(buf), "FPS: %.1f", fps);
        text = buf;
    }
    if (fastForward)
    {
        if (!text.empty())
            text += "   ";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%dx>>", static_cast<int>(m_host ? m_host->GetFastForwardMultiplier() : 2.0f));
        text += buf;
    }

    ImFont *font = ImGui::GetFont();
    if (!font)
        return;
    const float baseFontSize = ImGui::GetFontSize();
    const ImVec2 textSize = font->CalcTextSizeA(baseFontSize * fontScale, FLT_MAX, 0.0f, text.c_str());
    const ImVec2 min(margin, margin);
    const ImVec2 max(margin + textSize.x + pad * 2.0f, margin + textSize.y + pad * 2.0f);
    dl->AddRectFilled(min, max, IM_COL32(0, 0, 0, 140), std::round(em * 0.25f));
    dl->AddText(font, baseFontSize * fontScale, ImVec2(min.x + pad, min.y + pad),
                IM_COL32(135, 255, 135, 255), text.c_str());
}

void GBAStationOverlay::RenderSocialArea(ImDrawList *dl, ImVec2 displaySize)
{
    if (m_animTimer <= 0.0f)
        return;

    // Animation: Slide from left (200px to 0px)
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);

    if (ease < 0.01f)
        return;

    const float scale = OverlayScale();
    float startOffset = 200.0f * scale;
    float currentOffset = startOffset * (1.0f - ease); // Moves closer to 0 as ease -> 1

    float AVATAR_SIZE = 72.0f * scale;
    float sideMargin = 32.0f * scale; // As per EmulatorScreen logic for consistency
    float topMargin = 32.0f * scale;
    float barHeight = 50.0f * scale; // Matching StatusBar height

    // Center Y
    ImVec2 avatarCenter(sideMargin + AVATAR_SIZE * 0.5f - currentOffset,
                        topMargin + barHeight * 0.5f);

    // Draw Avatar Circle (NO Shadow per request)
    float radius = AVATAR_SIZE * 0.5f;

    ImU32 baseCol;
    if (m_isDarkMode)
    {
        baseCol = IM_COL32(45, 45, 45, (int)(255 * ease));
    }
    else
    {
        baseCol = IM_COL32(245, 247, 250, (int)(200 * ease));
    }

    dl->AddCircleFilled(avatarCenter, radius, baseCol);

    // Draw Image
    if (m_avatarTexture != 0)
    {
        float imgRadius = radius - 4.0f * scale;
        ImVec2 p_min = ImVec2(avatarCenter.x - imgRadius, avatarCenter.y - imgRadius);
        ImVec2 p_max = ImVec2(avatarCenter.x + imgRadius, avatarCenter.y + imgRadius);

        ImTextureID avatarTexture = m_avatarTexture;
        dl->AddImageRounded(avatarTexture, p_min, p_max,
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, imgRadius);

        dl->AddCircle(avatarCenter, imgRadius, IM_COL32(255, 255, 255, 60), 0, scale);
    }
    else
    {
        float imgRadius = radius - 4.0f * scale;
        dl->AddCircleFilled(avatarCenter, imgRadius, IM_COL32(200, 200, 210, 255));
    }

    // Draw Nickname
    if (m_showNickname && !m_nickname.empty())
    {
        // ... (Nickname drawing logic if needed)
    }
}

void GBAStationOverlay::GetGameViewport(float screenW, float screenH, float coreAspect,
                                  float &outX, float &outY, float &outW, float &outH) const
{
    // Dreamcast base resolution (most common mode) — mirrors RenderGame().
    static constexpr int DC_BASE_W = 640;
    static constexpr int DC_BASE_H = 480;

    float dstWidth = screenW;
    float dstHeight = screenH;

    if (m_displayMode == FlycastDisplayMode::Integer)
    {
        int scale;
        if (m_displaySize == FlycastDisplaySize::Auto)
        {
            int scaleX = (int)screenW / DC_BASE_W;
            int scaleY = (int)screenH / DC_BASE_H;
            scale = (scaleX < scaleY) ? scaleX : scaleY;
            if (scale < 1)
                scale = 1;
        }
        else
        {
            // _1x=4 → scale 1, _2x=5 → scale 2
            scale = (int)m_displaySize - 3;
            if (scale < 1)
                scale = 1;
        }
        dstWidth = (float)(DC_BASE_W * scale);
        dstHeight = (float)(DC_BASE_H * scale);
        if (dstWidth > screenW)
            dstWidth = screenW;
        if (dstHeight > screenH)
            dstHeight = screenH;
    }
    else
    {
        const float dstAspect = screenW / screenH;
        float ar;
        switch (m_displaySize)
        {
        case FlycastDisplaySize::Stretch:
            outX = 0.0f;
            outY = 0.0f;
            outW = screenW;
            outH = screenH;
            return;
        case FlycastDisplaySize::_4_3:
            ar = 4.0f / 3.0f;
            break;
        case FlycastDisplaySize::_16_9:
            ar = 16.0f / 9.0f;
            break;
        case FlycastDisplaySize::Original:
        default:
            ar = (coreAspect > 0.0f) ? coreAspect : (4.0f / 3.0f);
            break;
        }
        if (ar > dstAspect)
        {
            dstWidth = screenW;
            dstHeight = screenW / ar;
        }
        else
        {
            dstHeight = screenH;
            dstWidth = screenH * ar;
        }
    }

    outW = dstWidth;
    outH = dstHeight;
    outX = (screenW - dstWidth) / 2.0f;
    outY = (screenH - dstHeight) / 2.0f;
}

void GBAStationOverlay::RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                             float aspectRatio, int width, int height,
                             int fboWidth, int fboHeight)
{
    if (texture == 0)
        return;

    // Dreamcast base resolution (most common mode)
    static constexpr int DC_BASE_W = 640;
    static constexpr int DC_BASE_H = 480;

    float dstWidth = displaySize.x;
    float dstHeight = displaySize.y;
    float offsetX = 0;
    float offsetY = 0;

    if (m_displayMode == FlycastDisplayMode::Integer)
    {
        // ── Integer Scaling ──────────────────────────────────────────────
        int scale;
        if (m_displaySize == FlycastDisplaySize::Auto)
        {
            int scaleX = (int)displaySize.x / DC_BASE_W;
            int scaleY = (int)displaySize.y / DC_BASE_H;
            scale = (scaleX < scaleY) ? scaleX : scaleY;
            if (scale < 1)
                scale = 1;
        }
        else
        {
            // _1x=4 → scale 1, _2x=5 → scale 2
            scale = (int)m_displaySize - 3;
            if (scale < 1)
                scale = 1;
        }

        dstWidth = DC_BASE_W * scale;
        dstHeight = DC_BASE_H * scale;

        // Clamp to screen
        if (dstWidth > displaySize.x)
            dstWidth = displaySize.x;
        if (dstHeight > displaySize.y)
            dstHeight = displaySize.y;
    }
    else
    {
        // ── Display Mode (aspect-ratio based) ────────────────────────────
        switch (m_displaySize)
        {
        case FlycastDisplaySize::Stretch:
            // Fill entire screen (no aspect correction)
            dstWidth = displaySize.x;
            dstHeight = displaySize.y;
            break;
        case FlycastDisplaySize::_4_3:
        {
            float ar = 4.0f / 3.0f;
            float dstAspect = displaySize.x / displaySize.y;
            if (ar > dstAspect)
            {
                dstWidth = displaySize.x;
                dstHeight = displaySize.x / ar;
            }
            else
            {
                dstHeight = displaySize.y;
                dstWidth = displaySize.y * ar;
            }
            break;
        }
        case FlycastDisplaySize::_16_9:
        {
            float ar = 16.0f / 9.0f;
            float dstAspect = displaySize.x / displaySize.y;
            if (ar > dstAspect)
            {
                dstWidth = displaySize.x;
                dstHeight = displaySize.x / ar;
            }
            else
            {
                dstHeight = displaySize.y;
                dstWidth = displaySize.y * ar;
            }
            break;
        }
        case FlycastDisplaySize::Original:
        default:
        {
            // Use the core's reported aspect ratio (fit)
            float dstAspect = displaySize.x / displaySize.y;
            if (aspectRatio > dstAspect)
            {
                dstWidth = displaySize.x;
                dstHeight = displaySize.x / aspectRatio;
            }
            else
            {
                dstHeight = displaySize.y;
                dstWidth = displaySize.y * aspectRatio;
            }
            break;
        }
        }
    }

    // Center on screen
    offsetX = (displaySize.x - dstWidth) / 2.0f;
    offsetY = (displaySize.y - dstHeight) / 2.0f;

    // Black background
    dl->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 255));

    // Calculate UV sub-region: core renders to bottom-left of FBO
    // (FBO may be larger than the rendered area, e.g. square max dims)
    float u_max = (fboWidth > 0 && width > 0) ? (float)width / fboWidth : 1.0f;
    float v_max = (fboHeight > 0 && height > 0) ? (float)height / fboHeight : 1.0f;

    // Game texture (V-flipped for OpenGL FBO on Switch)
#ifdef __SWITCH__
    dl->AddImage((ImTextureID)(intptr_t)texture,
                 ImVec2(offsetX, offsetY),
                 ImVec2(offsetX + dstWidth, offsetY + dstHeight),
                 ImVec2(0.0f, v_max), ImVec2(u_max, 0.0f));
#else
    dl->AddImage((ImTextureID)(intptr_t)texture,
                 ImVec2(offsetX, offsetY),
                 ImVec2(offsetX + dstWidth, offsetY + dstHeight),
                 ImVec2(0.0f, 0.0f), ImVec2(u_max, v_max));
#endif
}

void GBAStationOverlay::RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize)
{
    // Animation ease
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);

    // 3-Part Gradient (20% Top, 60% Center, 20% Bottom)
    // Top/Bottom: Fade to Opaque (250)
    // Center: Base Transparency (200)

    float baseAlphaVal = 72.0f;
    float maxAlphaVal = 110.0f;

    int baseAlpha = (int)(baseAlphaVal * ease);
    int maxAlpha = (int)(maxAlphaVal * ease);

    if (baseAlpha > 0)
    {
        float topH = displaySize.y * 0.20f;
        float botH = displaySize.y * 0.20f;
        float centerH = displaySize.y - topH - botH;

        ImU32 colMax = IM_COL32(0, 0, 0, maxAlpha);
        ImU32 colBase = IM_COL32(0, 0, 0, baseAlpha);

        // Top Band
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(displaySize.x, topH),
                                    colMax, colMax, colBase, colBase);

        // Center Band
        dl->AddRectFilled(ImVec2(0, topH), ImVec2(displaySize.x, topH + centerH), colBase);

        // Bottom Band
        dl->AddRectFilledMultiColor(ImVec2(0, displaySize.y - botH), ImVec2(displaySize.x, displaySize.y),
                                    colBase, colBase, colMax, colMax);
    }
}

void GBAStationOverlay::RenderTitleCard(ImDrawList *dl, ImVec2 displaySize)
{
    if (m_animTimer <= 0.0f)
        return;

    std::string titleStr = m_gameTitle;
    if (m_currentMenu == OverlayMenu::SaveStates)
    {
        titleStr = m_isSaveMode ? tr("emulator_save_state") : tr("emulator_load_state");
    }
    else if (m_currentMenu == OverlayMenu::Settings)
    {
        titleStr = tr("emulator_settings");
    }
    else if (m_currentMenu == OverlayMenu::DiscSelect)
    {
        titleStr = tr("emulator_select_disc");
    }

    // The launcher bakes newlines into the title for its own wrapping; flatten
    // them so we can re-wrap and center each line ourselves.
    std::replace(titleStr.begin(), titleStr.end(), '\n', ' ');
    std::replace(titleStr.begin(), titleStr.end(), '\r', ' ');
    std::replace(titleStr.begin(), titleStr.end(), '\t', ' ');

    // Trim trailing whitespace.
    titleStr.erase(titleStr.find_last_not_of(" \n\r\t") + 1);

    const float scale = OverlayScale();
    const float AVAILABLE_TOP_SPACE = 110.0f * scale;

    // Animation: slide from top.
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    // Word-wrap long titles; each line is centered below.
    const float maxWidth = displaySize.x * 0.7f;
    std::vector<std::string> lines;
    {
        std::string cur;
        size_t pos = 0;
        while (pos < titleStr.size())
        {
            while (pos < titleStr.size() && titleStr[pos] == ' ')
                pos++;
            size_t end = titleStr.find(' ', pos);
            if (end == std::string::npos)
                end = titleStr.size();
            std::string word = titleStr.substr(pos, end - pos);
            pos = end;
            if (word.empty())
                continue;
            std::string candidate = cur.empty() ? word : cur + " " + word;
            if (cur.empty() || ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
                cur = candidate;
            else
            {
                lines.push_back(cur);
                cur = word;
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
        if (lines.empty())
            lines.push_back(titleStr);
        if (lines.size() > 3)
        {
            lines.resize(3);
            lines[2] += "...";
        }
    }

    const float lineHeight = ImGui::GetTextLineHeight();
    const float blockHeight = lineHeight * (float)lines.size();

    float targetTop = (AVAILABLE_TOP_SPACE - blockHeight) * 0.5f;
    float startY = -150.0f * scale;
    float blockTop = startY + (targetTop - startY) * easeOut;

    const ImU32 textColor = IM_COL32(200, 200, 200, 255);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        ImVec2 sz = ImGui::CalcTextSize(lines[i].c_str());
        float lineX = (displaySize.x - sz.x) * 0.5f;
        float lineY = blockTop + (float)i * lineHeight;
        UIStyle::DrawTextWithShadow(dl, ImVec2(lineX, lineY), textColor, lines[i].c_str());
    }
}

void GBAStationOverlay::RenderGBAStationMenu(ImDrawList *dl, ImVec2 displaySize)
{
    const float scale = OverlayScale();
    const float t = std::min(m_animTimer / 0.4f, 1.0f);
    const float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    const float width = displaySize.x;
    const float height = displaySize.y;
    const ImVec2 min(0.0f, 0.0f);
    const ImVec2 max(min.x + width, min.y + height);
    const std::string tabs[] = {tr("返回游戏"), tr("保存状态"), tr("读取状态"), tr("金手指"), tr("画面设置"), tr("功能设置"), tr("换碟"), tr("重置游戏"), tr("退出游戏")};
    const int icons[] = {0xE5C4, 0xE161, 0xE2C6, 0xE3AE, 0xE333, 0xE8B8, 0xE161, 0xE5D5, 0xE879};
    const std::string descriptions[] = {
        tr("继续当前游戏。"), tr("创建当前游戏的即时存档。"), tr("从即时存档恢复游戏。"), tr("管理当前游戏的金手指。"),
        tr("调整画面比例和缩放方式。"), tr("调整可即时生效的核心选项。"), tr("更换游戏碟片，可浏览任意目录选择新的镜像文件。"), tr("重新启动当前游戏。"), tr("返回 GBAStation。")};

    const int activeTab = m_quickMenuSelection;

    // 3DS palette
    const ImU32 white = IM_COL32(240, 247, 255, (int)(255.0f * ease));
    const ImU32 muted = IM_COL32(184, 204, 224, (int)(199.0f * ease));
    const ImU32 cyan = IM_COL32(112, 204, 255, (int)(255.0f * ease));
    const ImU32 focusBg = IM_COL32(0, 77, 128, (int)(133.0f * ease));
    const ImU32 contentFocusBg = IM_COL32(33, 107, 179, (int)(51.0f * ease));
    const ImU32 rowBg = IM_COL32(255, 255, 255, (int)(11.0f * ease));
    const ImU32 rowBorder = IM_COL32(255, 255, 255, (int)(26.0f * ease));
    const ImU32 focusBorder = IM_COL32(79, 179, 255, (int)(128.0f * ease));

    ImFont *font = ImGui::GetFont();

    // Background: vertical gradient strips like the 3DS shell.
    for (int strip = 0; strip < 8; ++strip)
    {
        const float ft = (float)strip / 7.0f;
        const int r = (int)((20.0f - ft * 8.0f) * ease);
        const int g = (int)((25.0f - ft * 10.0f) * ease);
        const int b = (int)((33.0f - ft * 13.0f) * ease);
        dl->AddRectFilled(ImVec2(0.0f, strip * (height / 8.0f)),
                          ImVec2(width, (strip + 1) * (height / 8.0f)), IM_COL32(r, g, b, 240));
    }

    // Title
    dl->AddText(font, 26.0f * scale, ImVec2(64.0f * scale, 58.0f * scale), white, tr("游戏菜单").c_str());
    dl->AddRectFilled(ImVec2(56.0f * scale, 92.0f * scale),
                      ImVec2(width - 56.0f * scale, 93.0f * scale), IM_COL32(255, 255, 255, (int)(46.0f * ease)));

    // Sidebar
    const float sidebarX = 48.0f * scale;
    const float sidebarY = 116.0f * scale;
    const float sidebarW = 336.0f * scale;
    const float itemH = 58.0f * scale;
    const float step = 64.0f * scale;
    for (int i = 0; i < 9; ++i)
    {
        const float y = sidebarY + i * step;
        const bool selected = i == activeTab;
        const bool tabFocused = selected && m_sidebarFocused;
        const ImVec2 itemMin(sidebarX, y), itemMax(sidebarX + sidebarW, y + itemH);
        if (selected)
        {
            dl->AddRectFilled(itemMin, itemMax, tabFocused ? focusBg : contentFocusBg);
            if (tabFocused)
            {
                DrawFocusBorder(itemMin, itemMax, 3.0f * scale);
            }
            else
            {
                dl->AddRect(itemMin, itemMax, focusBorder, 0.0f, 0, 1.0f * scale);
            }
        }
        char iconBuf[8];
        EncodeUtf8(iconBuf, icons[i]);
        // ImGui AddText's pos.y is the glyph top; the 3DS renderer passes a
        // baseline (y + 38 for a 58px row).  Compensate so the text is
        // vertically centered like the icon beside it.
        const float textY = y + itemH * 0.5f - 21.0f * scale * 0.43f;
        dl->AddText(font, 25.0f * scale, ImVec2(sidebarX + 34.0f * scale, y + itemH * 0.5f - 12.5f * scale),
                    selected ? white : muted, iconBuf);
        dl->AddText(font, 21.0f * scale, ImVec2(sidebarX + 64.0f * scale, textY),
                    selected ? white : muted, tabs[i].c_str());
    }
    // Reset separator (between the disc-swap item and Reset).
    dl->AddRectFilled(ImVec2(sidebarX + 18.0f * scale, sidebarY + 7.0f * step - 9.0f * scale),
                      ImVec2(sidebarX + sidebarW - 18.0f * scale, sidebarY + 7.0f * step - 8.0f * scale),
                      IM_COL32(255, 255, 255, (int)(36.0f * ease)));
    // Divider
    dl->AddRectFilled(ImVec2(404.0f * scale, 110.0f * scale),
                      ImVec2(405.0f * scale, 700.0f * scale), IM_COL32(255, 255, 255, (int)(20.0f * ease)));

    // Content area
    const float contentX = 432.0f * scale;
    const float contentW = 790.0f * scale;
    const float viewTop = 176.0f * scale;
    const float viewBottom = 664.0f * scale;
    const float rowH = 48.0f * scale;
    const float rowGap = 4.0f * scale;

    dl->AddText(font, 27.0f * scale, ImVec2(contentX, 116.0f * scale), white, tabs[activeTab].c_str());
    dl->AddRectFilled(ImVec2(contentX, 162.0f * scale),
                      ImVec2(contentX + contentW, 163.0f * scale), IM_COL32(0, 122, 204, (int)(71.0f * ease)));

    auto drawRow = [&](int row, bool focused, const char *iconUtf8, const std::string &label,
                       const std::string &value, bool selector) {
        const float y = viewTop + row * (rowH + rowGap);
        if (y + rowH < viewTop || y > viewBottom)
        {
            return;
        }
        const ImVec2 rowMin(contentX, y), rowMax(contentX + contentW, y + rowH);
        dl->AddRectFilled(rowMin, rowMax, focused ? focusBg : rowBg);
        if (focused)
        {
            DrawFocusBorder(rowMin, rowMax, 3.0f * scale);
        }
        else
        {
            dl->AddRect(rowMin, rowMax, rowBorder, 0.0f, 0, 1.0f * scale);
        }
        // AddText's y is the glyph top: compensate for the 3DS baseline (y+32).
        dl->AddText(font, 20.0f * scale, ImVec2(contentX + 24.0f * scale, y + rowH * 0.5f - 20.0f * scale * 0.43f),
                    selector ? cyan : (focused ? white : muted), iconUtf8);
        dl->AddText(font, 20.0f * scale, ImVec2(contentX + 46.0f * scale, y + rowH * 0.5f - 20.0f * scale * 0.43f),
                    focused ? white : muted, label.c_str());
        if (selector)
        {
            char iconL[8], iconR[8];
            EncodeUtf8(iconL, 0xE0E4);
            EncodeUtf8(iconR, 0xE0E5);
            dl->AddText(font, 26.0f * scale, ImVec2(contentX + contentW - 208.0f * scale, y + rowH * 0.5f - 26.0f * scale * 0.43f),
                        cyan, iconL);
            // Value with truncation + focus-scroll for long text.
            const float valueSize = 18.0f * scale;
            const float valueCenterX = contentX + contentW - 122.0f * scale;
            const float valueMaxW = 86.0f * scale;
            const float valueW = font->CalcTextSizeA(valueSize, FLT_MAX, 0.0f, value.c_str()).x;
            if (valueW <= valueMaxW)
            {
                dl->AddText(font, valueSize,
                            ImVec2(valueCenterX - valueW * 0.5f, y + rowH * 0.5f - valueSize * 0.43f),
                            cyan, value.c_str());
            }
            else if (focused)
            {
                // Focus-scroll: slide the text through the fixed window.
                const float scroll = std::fmod((float)(SDL_GetTicks64() % 8000) / 1000.0f, 1.0f);
                const float travel = valueW + valueMaxW;
                const float offset = (valueW + valueMaxW) * 0.5f - scroll * travel;
                dl->PushClipRect(ImVec2(valueCenterX - valueMaxW * 0.5f, y),
                                 ImVec2(valueCenterX + valueMaxW * 0.5f, y + rowH), true);
                dl->AddText(font, valueSize, ImVec2(valueCenterX - valueW * 0.5f + offset, y + rowH * 0.5f - valueSize * 0.43f),
                            cyan, value.c_str());
                dl->PopClipRect();
            }
            else
            {
                // Truncate with an ellipsis when idle.
                std::string clipped = value;
                while (!clipped.empty() &&
                       font->CalcTextSizeA(valueSize, FLT_MAX, 0.0f, (clipped + "...").c_str()).x > valueMaxW)
                {
                    clipped.pop_back();
                }
                clipped += "...";
                const float cw = font->CalcTextSizeA(valueSize, FLT_MAX, 0.0f, clipped.c_str()).x;
                dl->AddText(font, valueSize, ImVec2(valueCenterX - cw * 0.5f, y + rowH * 0.5f - valueSize * 0.43f),
                            cyan, clipped.c_str());
            }
            dl->AddText(font, 26.0f * scale, ImVec2(contentX + contentW - 38.0f * scale, y + rowH * 0.5f - 26.0f * scale * 0.43f),
                        cyan, iconR);
        }
        else
        {
            const float valueW = font->CalcTextSizeA(18.0f * scale, FLT_MAX, 0.0f, value.c_str()).x;
            dl->AddText(font, 18.0f * scale, ImVec2(contentX + contentW - valueW - 18.0f * scale, y + rowH * 0.5f - 18.0f * scale * 0.43f),
                        cyan, value.c_str());
        }
    };

    const bool inContent = !m_sidebarFocused;
    if (m_currentMenu == OverlayMenu::SaveStates)
    {
        // Two-column scrolling grid of save slots: snapshot thumbnail on the
        // left, slot name + save time on the right.
        const int total = 10;
        constexpr int kColumns = 2;
        const float cellW = (contentW - 14.0f * scale) * 0.5f;
        const float cellH = 112.0f * scale;
        const float cellGapX = 14.0f * scale;
        const float cellGapY = 10.0f * scale;
        const int gridH = (total + kColumns - 1) / kColumns;
        // Visible rows fit the viewport; the focused row scrolls to centre.
        const float viewportH = viewBottom - viewTop;
        const int visibleRows = std::max(1, (int)(viewportH / (cellH + cellGapY)));
        const int kRows = std::min(gridH, visibleRows);
        const int selectedRow = m_saveStateSlot / kColumns;
        const int firstRow = std::clamp(selectedRow - kRows / 2, 0, std::max(0, gridH - kRows));
        for (int r = 0; r < kRows; ++r)
        {
            const int row = firstRow + r;
            for (int c = 0; c < kColumns; ++c)
            {
                const int slot = row * kColumns + c;
                if (slot >= total)
                    continue;
                const float x = contentX + c * (cellW + cellGapX);
                const float y = viewTop + r * (cellH + cellGapY);
                if (y + cellH < viewTop || y > viewBottom)
                    continue;
                const bool focused = inContent && slot == m_saveStateSlot;
                const bool exists = m_host && m_host->IsGameLoaded() && m_host->StateSlotExists(slot);
                const ImVec2 cellMin(x, y), cellMax(x + cellW, y + cellH);
                dl->AddRectFilled(cellMin, cellMax, focused ? focusBg : rowBg, 8.0f * scale);
                if (focused)
                {
                    DrawFocusBorder(cellMin, cellMax, 3.0f * scale);
                }
                else
                {
                    dl->AddRect(cellMin, cellMax, rowBorder, 8.0f * scale, 0, 1.0f * scale);
                }
                // Snapshot thumbnail on the left (screenshots land here later).
                const float snapX = x + 8.0f * scale;
                const float snapW = cellW * 0.40f - 8.0f * scale;
                const float snapY = y + 8.0f * scale;
                const float snapH = cellH - 16.0f * scale;
                dl->AddRectFilled(ImVec2(snapX, snapY), ImVec2(snapX + snapW, snapY + snapH),
                                  IM_COL32(255, 255, 255, focused ? 18 : 10), 6.0f * scale);
                dl->AddRect(ImVec2(snapX, snapY), ImVec2(snapX + snapW, snapY + snapH),
                            IM_COL32(255, 255, 255, focused ? 60 : 34), 6.0f * scale, 0, 1.0f * scale);
                char snapIcon[8];
                EncodeUtf8(snapIcon, 0xE413);
                const float snapIconSize = 30.0f * scale;
                dl->AddText(font, snapIconSize,
                            ImVec2(snapX + snapW * 0.5f - snapIconSize * 0.5f,
                                   snapY + snapH * 0.5f - snapIconSize * 0.43f),
                            IM_COL32(160, 200, 230, (int)(110.0f * ease)), snapIcon);
                // Right side: slot name + save time.
                const float textX = snapX + snapW + 12.0f * scale;
                const std::string title = tr("存档槽 ") + std::to_string(slot + 1);
                dl->AddText(font, 20.0f * scale, ImVec2(textX, y + 26.0f * scale),
                            focused ? white : muted, title.c_str());
                if (exists)
                {
                    char timeBuf[32]{};
                    const time_t mtime = m_host ? m_host->StateSlotTime(slot) : 0;
                    std::tm tm{};
                    localtime_r(&mtime, &tm);
                    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tm);
                    dl->AddText(font, 16.0f * scale, ImVec2(textX, y + cellH - 42.0f * scale),
                                cyan, timeBuf);
                }
                else
                {
                    dl->AddText(font, 16.0f * scale, ImVec2(textX, y + cellH - 42.0f * scale),
                                muted, tr("空存档槽").c_str());
                }
            }
        }
        // Scroll position hint above the footer (右下角提示文字上方).
        const float hintY = viewBottom - 26.0f * scale;
        const std::string scrollHint = std::to_string(selectedRow + 1) + " / " + std::to_string(gridH);
        const ImVec2 hintSize = font->CalcTextSizeA(16.0f * scale, FLT_MAX, 0.0f, scrollHint.c_str());
        dl->AddText(font, 16.0f * scale,
                    ImVec2(contentX + contentW - hintSize.x, hintY),
                    IM_COL32(184, 204, 224, (int)(160.0f * ease)), scrollHint.c_str());
    }
    else if (m_currentMenu == OverlayMenu::Settings)
    {
        if (activeTab == 3)
        {
            const std::string enabled = m_host && m_host->GetCoreOption("reicast_widescreen_cheats", "disabled") == "enabled"
                                            ? tr("开启") : tr("关闭");
            char icon[8];
            EncodeUtf8(icon, 0xE3AE);
            drawRow(0, inContent && m_settingsSelection == 0, icon, tr("宽屏金手指"), enabled, false);
        }
        else if (activeTab == 4)
        {
            // 画面设置: 渲染相关 + 显示模式/比例。
            const std::string labels[] = {tr("渲染分辨率"), tr("纹理过滤"), tr("各向异性过滤"), tr("Mip 贴图"), tr("自动跳帧"), tr("帧跳过"), tr("宽屏修正"), tr("显示模式"), tr("画面比例")};
            const char *keys[] = {"reicast_internal_resolution", "reicast_texture_filtering", "reicast_anisotropic_filtering", "reicast_mipmapping", "reicast_auto_skip_frame", "reicast_frame_skipping", "reicast_widescreen_hack", "", ""};
            const int rowIcons[] = {0xE333, 0xE3F4, 0xE3F4, 0xE873, 0xE8E5, 0xE8E5, 0xE3B6, 0xE8F1, 0xE3F4};
            const int total = 9;
            const int visible = std::min(8, total);
            const int first = std::clamp(m_settingsSelection - visible / 2, 0, std::max(0, total - visible));
            const std::string mode = m_displayMode == FlycastDisplayMode::Integer ? tr("整数缩放") : tr("比例显示");
            std::string size = tr("原始比例");
            if (m_displayMode == FlycastDisplayMode::Integer)
                size = m_displaySize == FlycastDisplaySize::_1x ? "1x" : m_displaySize == FlycastDisplaySize::_2x ? "2x" : tr("自动");
            else if (m_displaySize == FlycastDisplaySize::Stretch) size = tr("拉伸");
            else if (m_displaySize == FlycastDisplaySize::_4_3) size = "4:3";
            else if (m_displaySize == FlycastDisplaySize::_16_9) size = "16:9";
            for (int row = 0; row < visible; ++row)
            {
                const int option = first + row;
                std::string value;
                if (option == 7)
                {
                    value = mode;
                }
                else if (option == 8)
                {
                    value = size;
                }
                else
                {
                    value = m_host ? m_host->GetCoreOption(keys[option], tr("自动")) : tr("自动");
                    // These are documented "only apply after restarting" in the
                    // core's option metadata (texture/anisotropic filtering).
                    if (option == 1 || option == 2)
                        value += tr("（重启后生效）");
                }
                char icon[8];
                EncodeUtf8(icon, rowIcons[option]);
                drawRow(row, inContent && option == m_settingsSelection, icon, labels[option], value,
                        true);
            }
        }
        else if (activeTab == 5)
        {
            // 功能设置: 线程/快进等非画面项目。
            const std::string labels[] = {tr("多线程渲染"), tr("快进倍率"), tr("快进模式")};
            const char *keys[] = {"reicast_threaded_rendering", "", ""};
            const int rowIcons[] = {0xE8B8, 0xE8B2, 0xE8B8};
            const int total = 3;
            const int visible = std::min(8, total);
            const int first = std::clamp(m_settingsSelection - visible / 2, 0, std::max(0, total - visible));
            for (int row = 0; row < visible; ++row)
            {
                const int option = first + row;
                std::string value;
                if (option == 1)
                {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%dx", static_cast<int>(m_host ? m_host->GetFastForwardMultiplier() : 2.0f));
                    value = buf;
                }
                else if (option == 2)
                {
                    value = m_host && m_host->GetFastForwardToggleMode() ? tr("切换") : tr("按住");
                }
                else
                {
                    value = m_host ? m_host->GetCoreOption(keys[option], tr("自动")) : tr("自动");
                    value += tr("（重启后生效）");
                }
                char icon[8];
                EncodeUtf8(icon, rowIcons[option]);
                drawRow(row, inContent && option == m_settingsSelection, icon, labels[option], value,
                        true);
            }
        }
        else
        {
            const std::string mode = m_displayMode == FlycastDisplayMode::Integer ? tr("整数缩放") : tr("比例显示");
            std::string size = tr("原始比例");
            if (m_displayMode == FlycastDisplayMode::Integer)
                size = m_displaySize == FlycastDisplaySize::_1x ? "1x" : m_displaySize == FlycastDisplaySize::_2x ? "2x" : tr("自动");
            else if (m_displaySize == FlycastDisplaySize::Stretch) size = tr("拉伸");
            else if (m_displaySize == FlycastDisplaySize::_4_3) size = "4:3";
            else if (m_displaySize == FlycastDisplaySize::_16_9) size = "16:9";
            char icon[8];
            EncodeUtf8(icon, 0xE8F1);
            drawRow(0, inContent && m_settingsSelection == 0, icon, tr("显示模式"), mode, true);
            EncodeUtf8(icon, 0xE3F4);
            drawRow(1, inContent && m_settingsSelection == 1, icon, tr("画面比例"), size, true);
        }
    }
    else if (m_currentMenu == OverlayMenu::DiscSelect)
    {
        RenderDiscBrowser(dl, displaySize);
    }
    else
    {
        dl->AddText(font, 20.0f * scale, ImVec2(contentX, 310.0f * scale),
                    IM_COL32(204, 230, 250, (int)(219.0f * ease)), descriptions[activeTab].c_str());
    }
}

void GBAStationOverlay::RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const std::string menuItems[] = {
        tr("emulator_resume"),
        tr("emulator_save_state"),
        tr("emulator_load_state"),
        tr("emulator_cheats"),
        tr("emulator_video_settings"),
        tr("emulator_core_settings"),
        tr("emulator_swap_disc"),
        tr("emulator_reset"),
        tr("emulator_exit_game")};
    const std::string descriptions[] = {
        "Return to the current game.",
        "Create a restore point for the current game.",
        "Resume from an existing restore point.",
        "Manage game cheat settings.",
        "Configure display scaling and aspect ratio.",
        "Configure Flycast core options.",
        "Swap the current disc for another image.",
        "Restart the current game.",
        "Return to GBAStation."};
    constexpr int numItems = 9;

    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    ImFont *font = ImGui::GetFont();
    const float labelSize = ImGui::GetFontSize() * 0.78f;
    const float titleSize = ImGui::GetFontSize() * 1.12f;
    const float width = std::min(displaySize.x - 100.0f * scale, 1030.0f * scale);
    const float height = std::min(displaySize.y - 150.0f * scale, 530.0f * scale);
    const ImVec2 origin((displaySize.x - width) * 0.5f,
                        (displaySize.y - height) * 0.5f + (1.0f - easeOut) * 90.0f * scale);
    const ImVec2 end(origin.x + width, origin.y + height);
    const float sidebarWidth = std::min(300.0f * scale, width * 0.32f);
    const float itemHeight = (height - 32.0f * scale) / numItems;

    dl->AddRectFilled(origin, end, IM_COL32(14, 23, 28, (int)(248 * easeOut)), 6.0f * scale);
    dl->AddRectFilled(origin, ImVec2(origin.x + sidebarWidth, end.y),
                      IM_COL32(18, 48, 52, (int)(255 * easeOut)), 6.0f * scale,
                      ImDrawFlags_RoundCornersLeft);
    for (int i = 0; i < numItems; i++)
    {
        bool isSelected = (m_quickMenuSelection == i);
        ImVec2 itemMin(origin.x + 12.0f * scale, origin.y + 16.0f * scale + i * itemHeight);
        ImVec2 itemMax(origin.x + sidebarWidth - 12.0f * scale, itemMin.y + itemHeight - 4.0f * scale);
        if (isSelected)
            dl->AddRectFilled(itemMin, itemMax, IM_COL32(15, 142, 122, (int)(235 * easeOut)), 4.0f * scale);
        const ImVec2 textSize = font->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, menuItems[i].c_str());
        dl->AddText(font, labelSize, ImVec2(itemMin.x + 18.0f * scale, itemMin.y + (itemHeight - textSize.y) * 0.5f),
                    isSelected ? IM_COL32(255, 255, 255, (int)(255 * easeOut))
                               : IM_COL32(205, 212, 220, (int)(235 * easeOut)), menuItems[i].c_str());
    }

    const float contentX = origin.x + sidebarWidth + 48.0f * scale;
    dl->AddText(font, titleSize, ImVec2(contentX, origin.y + 72.0f * scale),
                IM_COL32(255, 255, 255, (int)(255 * easeOut)), menuItems[m_quickMenuSelection].c_str());
    dl->AddLine(ImVec2(contentX, origin.y + 118.0f * scale),
                ImVec2(end.x - 48.0f * scale, origin.y + 118.0f * scale),
                IM_COL32(42, 116, 108, (int)(180 * easeOut)), 1.0f * scale);
    dl->AddText(font, labelSize, ImVec2(contentX, origin.y + 150.0f * scale),
                IM_COL32(196, 222, 216, (int)(235 * easeOut)), descriptions[m_quickMenuSelection].c_str());
}

void GBAStationOverlay::RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const float menuWidth = 400.0f * scale;
    const int numSlots = 4;
    const float itemHeight = 64.0f * scale;
    const float contentHeight = numSlots * itemHeight; // Flush

    ImVec2 menuSize(menuWidth, contentHeight);

    // Animation: Slide from bottom
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float targetY = (displaySize.y - menuSize.y) / 2.0f;
    float startY = displaySize.y + (100.0f * scale);
    float currentY = startY + (targetY - startY) * easeOut;

    ImVec2 menuPos((displaySize.x - menuSize.x) / 2, currentY);
    ImVec2 p0 = menuPos;
    ImVec2 p1 = ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y);

    const float cornerRadius = 16.0f * scale;

    // Opaque Background
    ImU32 containerColor;
    if (m_isDarkMode)
    {
        containerColor = IM_COL32(45, 45, 45, (int)(255 * easeOut));
    }
    else
    {
        containerColor = IM_COL32(242, 245, 248, (int)(255 * easeOut));
    }
    dl->AddRectFilled(p0, p1, containerColor, cornerRadius);

    ImFont *font = ImGui::GetFont();
    float baseFontSize = ImGui::GetFontSize();
    float smallFontSize = baseFontSize * 0.85f;

    for (int i = 0; i < numSlots; i++)
    {
        bool isSelected = (m_saveStateSlot == i);
        float itemY = menuPos.y + i * itemHeight;
        float inset = 0.0f;
        ImVec2 itemMin(menuPos.x + inset, itemY);
        ImVec2 itemMax(menuPos.x + menuSize.x - inset, itemY + itemHeight);

        if (isSelected)
        {
            ImDrawFlags corners = 0;
            float itemRadius = 0.0f;
            if (i == 0)
            {
                corners = ImDrawFlags_RoundCornersTop;
                itemRadius = cornerRadius;
            }
            else if (i == numSlots - 1)
            {
                corners = ImDrawFlags_RoundCornersBottom;
                itemRadius = cornerRadius;
            }

            ImU32 selectedColor;
            if (m_isDarkMode)
            {
                selectedColor = IM_COL32(60, 60, 60, (int)(255 * easeOut));
            }
            else
            {
                // Light Mode Selected: Darker Grey/White (220, 224, 228) for visibility
                selectedColor = IM_COL32(190, 195, 205, (int)(255 * easeOut));
            }
            dl->AddRectFilled(itemMin, itemMax, selectedColor, itemRadius, corners);
        }

        // No Dividers

        bool exists = false;
        if (m_host && m_host->IsGameLoaded())
            exists = m_host->StateSlotExists(i);

        char slotText[64];
        snprintf(slotText, sizeof(slotText), tr("emulator_slot").c_str(), i + 1, exists ? tr("emulator_in_use").c_str() : tr("emulator_empty").c_str());

        ImVec2 sz = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, slotText);
        float textX = itemMin.x + 20.0f * scale;
        float textY = itemMin.y + (itemHeight - sz.y) / 2;

        ImU32 textColor;
        if (m_isDarkMode)
        {
            if (isSelected)
                textColor = IM_COL32(255, 255, 255, (int)(255 * easeOut));
            else
                textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));
        }
        else
        {
            if (isSelected)
                textColor = IM_COL32(60, 60, 70, (int)(255 * easeOut));
            else
                textColor = IM_COL32(90, 90, 100, (int)(255 * easeOut));
        }

        dl->AddText(font, smallFontSize, ImVec2(textX, textY), textColor, slotText);
    }
}

// Copied logic from EmulatorScreen
void GBAStationOverlay::RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const float menuWidth = 400.0f * scale;
    int numItems = 2; // Display Mode + Size

    const float itemHeight = 64.0f * scale;
    const float contentHeight = numItems * itemHeight;

    ImVec2 menuSize(menuWidth, contentHeight);

    // Animation: Slide from bottom
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float targetY = (displaySize.y - menuSize.y) / 2.0f;
    float startY = displaySize.y + (100.0f * scale);
    float currentY = startY + (targetY - startY) * easeOut;

    ImVec2 menuPos((displaySize.x - menuSize.x) / 2, currentY);
    ImVec2 p0 = menuPos;
    ImVec2 p1 = ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y);

    const float cornerRadius = 16.0f * scale;

    // Opaque Background
    ImU32 containerColor;
    if (m_isDarkMode)
    {
        containerColor = IM_COL32(45, 45, 45, (int)(255 * easeOut));
    }
    else
    {
        containerColor = IM_COL32(242, 245, 248, (int)(255 * easeOut));
    }
    dl->AddRectFilled(p0, p1, containerColor, cornerRadius);

    ImFont *font = ImGui::GetFont();
    float baseFontSize = ImGui::GetFontSize();
    float smallFontSize = baseFontSize * 0.85f;

    for (int i = 0; i < numItems; i++)
    {
        bool isSelected = (m_settingsSelection == i);
        float itemY = menuPos.y + i * itemHeight;
        float inset = 0.0f;
        ImVec2 itemMin(menuPos.x + inset, itemY);
        ImVec2 itemMax(menuPos.x + menuSize.x - inset, itemY + itemHeight);

        // Selection Highlight
        if (isSelected)
        {
            ImDrawFlags corners = 0;
            float itemRadius = 0.0f;
            if (i == 0)
            {
                corners = ImDrawFlags_RoundCornersTop;
                itemRadius = cornerRadius;
            }
            else if (i == numItems - 1)
            {
                corners = ImDrawFlags_RoundCornersBottom;
                itemRadius = cornerRadius;
            }

            ImU32 selectedColor;
            if (m_isDarkMode)
            {
                selectedColor = IM_COL32(60, 60, 60, (int)(255 * easeOut));
            }
            else
            {
                selectedColor = IM_COL32(190, 195, 205, (int)(255 * easeOut));
            }
            dl->AddRectFilled(itemMin, itemMax, selectedColor, itemRadius, corners);
        }

        // Label + Value
        std::string label;
        std::string value;

        if (i == 0)
        {
            label = tr("emulator_display_mode");
            value = (m_displayMode == FlycastDisplayMode::Integer) ? tr("emulator_integer") : tr("emulator_display");
        }
        else if (i == 1)
        {
            label = tr("emulator_size");
            if (m_displayMode == FlycastDisplayMode::Integer)
            {
                switch (m_displaySize)
                {
                case FlycastDisplaySize::_1x:
                    value = "1x";
                    break;
                case FlycastDisplaySize::_2x:
                    value = "2x";
                    break;
                case FlycastDisplaySize::Auto:
                    value = tr("emulator_auto");
                    break;
                default:
                    value = tr("emulator_auto");
                    break;
                }
            }
            else
            {
                switch (m_displaySize)
                {
                case FlycastDisplaySize::Stretch:
                    value = tr("emulator_stretch");
                    break;
                case FlycastDisplaySize::_4_3:
                    value = "4:3";
                    break;
                case FlycastDisplaySize::_16_9:
                    value = "16:9";
                    break;
                case FlycastDisplaySize::Original:
                    value = tr("emulator_original");
                    break;
                default:
                    value = "4:3";
                    break;
                }
            }
        }

        ImU32 textColor;
        if (m_isDarkMode)
        {
            if (isSelected)
                textColor = IM_COL32(255, 255, 255, (int)(255 * easeOut));
            else
                textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));
        }
        else
        {
            if (isSelected)
                textColor = IM_COL32(60, 60, 70, (int)(255 * easeOut));
            else
                textColor = IM_COL32(90, 90, 100, (int)(255 * easeOut));
        }

        float textX = itemMin.x + 20.0f * scale;
        ImVec2 labelSize = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, label.c_str());
        float textY = itemMin.y + (itemHeight - labelSize.y) / 2;
        dl->AddText(font, smallFontSize, ImVec2(textX, textY), textColor, label.c_str());

        ImVec2 valueSize = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, value.c_str());
        float valueX = itemMax.x - valueSize.x - 40.0f * scale;
        dl->AddText(font, smallFontSize, ImVec2(valueX, textY), textColor, value.c_str());

        // Draw Triangle Arrows
        if (isSelected)
        {
            float arrowSize = 12.0f * scale;
            float arrowY = itemMin.y + (itemHeight - arrowSize) / 2;

            // Left Arrow
            float leftArrowX = valueX - arrowSize - 12.0f * scale;
            ImVec2 lp1 = ImVec2(leftArrowX, arrowY + arrowSize / 2);
            ImVec2 lp2 = ImVec2(leftArrowX + arrowSize, arrowY);
            ImVec2 lp3 = ImVec2(leftArrowX + arrowSize, arrowY + arrowSize);
            dl->AddTriangleFilled(lp1, lp2, lp3, textColor);

            // Right Arrow
            float rightArrowX = valueX + valueSize.x + 12.0f * scale;
            ImVec2 rp1 = ImVec2(rightArrowX + arrowSize, arrowY + arrowSize / 2);
            ImVec2 rp2 = ImVec2(rightArrowX, arrowY);
            ImVec2 rp3 = ImVec2(rightArrowX, arrowY + arrowSize);
            dl->AddTriangleFilled(rp1, rp2, rp3, textColor);
        }
    }
}

void GBAStationOverlay::RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize)
{
    const float scale = OverlayScale();
    const float easeOut = 1.0f - std::pow(1.0f - std::min(m_animTimer / 0.4f, 1.0f), 3.0f);

    // 3DS footer: B and A button hints pinned to the bottom right.
    const std::string bLabel = (m_sidebarFocused || m_currentMenu == OverlayMenu::QuickMenu) ? tr("返回") : tr("返回列表");
    std::string aLabel;
    if (m_currentMenu == OverlayMenu::SaveStates)
        aLabel = m_isSaveMode ? tr("保存") : tr("读取");
    else if (m_currentMenu == OverlayMenu::Settings)
        aLabel = tr("调整");
    else if (m_currentMenu == OverlayMenu::DiscSelect)
        aLabel = tr("选择");
    else
        aLabel = tr("确定");

    ImFont *font = ImGui::GetFont();
    const ImU32 hintColor = IM_COL32(184, 204, 224, (int)(199.0f * easeOut));
    char iconB[8], iconA[8];
    EncodeUtf8(iconB, 0xE0E1);
    EncodeUtf8(iconA, 0xE0E0);
const float baseY = displaySize.y - 42.0f * scale;
dl->AddText(font, 27.0f * scale, ImVec2(1020.0f * scale, baseY - 27.0f * scale * 0.5f), hintColor, iconB);
dl->AddText(font, 19.0f * scale, ImVec2(1042.0f * scale, baseY - 19.0f * scale * 0.5f), hintColor, bLabel.c_str());
dl->AddText(font, 27.0f * scale, ImVec2(1152.0f * scale, baseY - 27.0f * scale * 0.5f), hintColor, iconA);
dl->AddText(font, 19.0f * scale, ImVec2(1174.0f * scale, baseY - 19.0f * scale * 0.5f), hintColor, aLabel.c_str());
}

//==============================================================================
// Input Handling
//==============================================================================

bool GBAStationOverlay::HandleInput(const GBAStation::FrameInput &input)
{
    using namespace GBAStation;

    const uint64_t buttons = input.buttons;
    [[maybe_unused]] const uint64_t pressed = input.pressed;
#ifdef __SWITCH__
    const uint64_t navButtons = input.rawButtons;
    const uint64_t navPressed = input.rawPressed;
#else
    const uint64_t navButtons = buttons;
    const uint64_t navPressed = pressed;
#endif

    // Pad_Guide is synthesized by GBAStation::Main from dc.hotkey.menu.pad.  Do not
    // infer it from Start+Select here: those buttons belong to the emulated DC
    // pad and may have been remapped by the user.
    const bool comboDown = (buttons & Pad_Guide) != 0;
    if (comboDown)
    {
        if (m_currentMenu == OverlayMenu::None)
            Show();
        return true;
    }

    // If overlay not visible, don't consume input
    if (m_currentMenu == OverlayMenu::None)
        return false;

    // Directional navigation: merge D-pad + left stick into a held bitmask,
    // then derive edge + hold-repeat without any time API.  The physical L / R
    // shoulders double as Left / Right so the LR selectors work like the 3DS.
    uint64_t dirHeld = 0;
    if ((navButtons & HidNpadButton_Up) || input.leftStickY < -16000) dirHeld |= Pad_Up;
    if ((navButtons & HidNpadButton_Down) || input.leftStickY > 16000) dirHeld |= Pad_Down;
    if ((navButtons & (HidNpadButton_Left | HidNpadButton_L)) || input.leftStickX < -16000) dirHeld |= Pad_Left;
    if ((navButtons & (HidNpadButton_Right | HidNpadButton_R)) || input.leftStickX > 16000) dirHeld |= Pad_Right;

    uint64_t dirFire = dirHeld & ~m_navHeldPrev; // new presses fire instantly
    if (dirFire != 0)
    {
        m_navStartMs = SDL_GetTicks();
        m_navFireAtMs = m_navStartMs + NAV_INITIAL_DELAY_MS;
    }
    else if (dirHeld != 0 && dirHeld == m_navHeldPrev && m_navFireAtMs != 0)
    {
        const uint64_t nowMs = SDL_GetTicks();
        if (nowMs >= m_navFireAtMs)
        {
            dirFire |= dirHeld;
            // Accelerate while held: 128 ms -> 48 ms over ~1 second.
            const uint64_t heldMs = nowMs - m_navStartMs;
            uint64_t interval = NAV_START_REPEAT_MS - heldMs * 12 / 100;
            if (interval < NAV_MIN_REPEAT_MS)
                interval = NAV_MIN_REPEAT_MS;
            m_navFireAtMs = nowMs + interval;
        }
    }
    m_navHeldPrev = dirHeld;

    bool upPressed = (dirFire & Pad_Up) != 0;
    bool downPressed = (dirFire & Pad_Down) != 0;
    bool leftPressed = (dirFire & Pad_Left) != 0;
    bool rightPressed = (dirFire & Pad_Right) != 0;

    // Confirm = B, Back = A, disc select = Y (edge-triggered).
    bool confirmPressed = (navPressed & HidNpadButton_A) != 0;
    bool backPressed = (navPressed & HidNpadButton_B) != 0;
    bool xPressed = (navPressed & HidNpadButton_X) != 0;

    // The sidebar owns navigation until Right explicitly enters the active
    // page. Switching a tab is immediate: the right panel changes before any
    // confirmation is required.
    if (m_sidebarFocused)
    {
        if (upPressed)
        {
            ActivateTab((m_quickMenuSelection + 8) % 9);
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        if (downPressed)
        {
            ActivateTab((m_quickMenuSelection + 1) % 9);
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        if (rightPressed && (m_currentMenu == OverlayMenu::SaveStates || m_currentMenu == OverlayMenu::Settings))
        {
            m_sidebarFocused = false;
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        if (confirmPressed)
        {
            if (m_quickMenuSelection == 0)
            {
                Hide();
                GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm);
            }
            else if (m_quickMenuSelection == 6)
            {
                OpenDiscBrowser();
                m_currentMenu = OverlayMenu::DiscSelect;
                m_sidebarFocused = false; // browser list owns the d-pad now
                m_animTimer = 0.4f;
                GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm);
            }
            else if (m_quickMenuSelection == 7)
            {
                m_shouldReset = true;
                Hide();
                GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm);
            }
            else if (m_quickMenuSelection == 8)
            {
                m_shouldExit = true;
                Hide();
                GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm);
            }
            else if (m_currentMenu == OverlayMenu::SaveStates || m_currentMenu == OverlayMenu::Settings)
            {
                m_sidebarFocused = false;
                GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
            }
        }
        if (backPressed)
        {
            Hide();
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Cancel);
        }
        return true;
    }

    if (leftPressed)
    {
        // Left adjusts the focused setting or moves between save-slot columns;
        // it never returns to the sidebar (B does).  Without this the LR
        // selectors could not be changed — the first Left would pop the focus
        // back to the tabs.
        if (m_currentMenu != OverlayMenu::Settings && m_currentMenu != OverlayMenu::SaveStates)
        {
            m_sidebarFocused = true;
            return true;
        }
    }

    // Minus = reset, but only after the menu's been open a moment (else the
    // Start+Select that opened it would reset immediately).
    constexpr float kResetThreshold = 0.6f;
    if (m_currentMenu == OverlayMenu::QuickMenu &&
        (navPressed & HidNpadButton_Minus) &&
        m_quickMenuOpenTime > kResetThreshold)
    {
        m_shouldReset = true;
        Hide();
        return true;
    }

    // X button for disc swap browser (always available).
    if (xPressed && m_currentMenu == OverlayMenu::QuickMenu)
    {
        OpenDiscBrowser();
        m_currentMenu = OverlayMenu::DiscSelect;
        m_sidebarFocused = false; // browser list owns the d-pad now
        m_animTimer = 0.4f;
        return true;
    }

    // Up
    if (upPressed)
    {
        if (m_currentMenu == OverlayMenu::QuickMenu)
        {
            m_quickMenuSelection = (m_quickMenuSelection + 8) % 9;
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        else if (m_currentMenu == OverlayMenu::SaveStates)
        {
            m_saveStateSlot = (m_saveStateSlot + 8) % 10; // up = previous row (2 columns)
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        else if (m_currentMenu == OverlayMenu::Settings)
        {
            const int settingCount = m_quickMenuSelection == 4 ? 9 : (m_quickMenuSelection == 5 ? 3 : (m_quickMenuSelection == 3 ? 1 : 2));
            m_settingsSelection = (m_settingsSelection + settingCount - 1) % settingCount;
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        else if (m_currentMenu == OverlayMenu::DiscSelect)
        {
            if (!m_discBrowserEntries.empty())
            {
                m_discBrowserSelection--;
                if (m_discBrowserSelection < 0)
                    m_discBrowserSelection = (int)m_discBrowserEntries.size() - 1;
                const float rowH = 52.0f * OverlayScale();
                const float contentH = 470.0f * OverlayScale();
                m_discBrowserTargetScrollY = m_discBrowserSelection * rowH - contentH * 0.5f + rowH * 0.5f;
                const float maxScroll = (float)m_discBrowserEntries.size() * rowH - contentH;
                m_discBrowserTargetScrollY = maxScroll > 0.0f ? std::clamp(m_discBrowserTargetScrollY, 0.0f, maxScroll) : 0.0f;
            }
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
    }
    // Down
    if (downPressed)
    {
        if (m_currentMenu == OverlayMenu::QuickMenu)
        {
            m_quickMenuSelection = (m_quickMenuSelection + 1) % 9;
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        else if (m_currentMenu == OverlayMenu::SaveStates)
        {
            m_saveStateSlot = (m_saveStateSlot + 2) % 10; // down = next row (2 columns)
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        else if (m_currentMenu == OverlayMenu::Settings)
        {
            const int settingCount = m_quickMenuSelection == 4 ? 9 : (m_quickMenuSelection == 5 ? 3 : (m_quickMenuSelection == 3 ? 1 : 2));
            m_settingsSelection = (m_settingsSelection + 1) % settingCount;
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
        else if (m_currentMenu == OverlayMenu::DiscSelect)
        {
            if (!m_discBrowserEntries.empty())
            {
                m_discBrowserSelection++;
                if (m_discBrowserSelection >= (int)m_discBrowserEntries.size())
                    m_discBrowserSelection = 0;
                const float rowH = 52.0f * OverlayScale();
                const float contentH = 470.0f * OverlayScale();
                m_discBrowserTargetScrollY = m_discBrowserSelection * rowH - contentH * 0.5f + rowH * 0.5f;
                const float maxScroll = (float)m_discBrowserEntries.size() * rowH - contentH;
                m_discBrowserTargetScrollY = maxScroll > 0.0f ? std::clamp(m_discBrowserTargetScrollY, 0.0f, maxScroll) : 0.0f;
            }
            GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        }
    }
    // Left/Right (Settings: adjust value; SaveStates: move between columns)
    bool directionChanged = false;
    int direction = 0;

    if (leftPressed)
    {
        direction = -1;
        directionChanged = true;
    }
    if (rightPressed)
    {
        direction = 1;
        directionChanged = true;
    }

    if (directionChanged && m_currentMenu == OverlayMenu::SaveStates)
    {
        m_saveStateSlot = (m_saveStateSlot + direction + 10) % 10;
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
    }

    if (directionChanged && m_currentMenu == OverlayMenu::Settings)
    {
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Focus);
        if (m_quickMenuSelection == 3)
        {
            CycleFlycastOption(m_host, "reicast_widescreen_cheats", {"disabled", "enabled"}, direction);
        }
        else if (m_quickMenuSelection == 4)
        {
            switch (m_settingsSelection)
            {
            case 0: CycleFlycastOption(m_host, "reicast_internal_resolution", {"320x240", "640x480", "960x720", "1280x960", "1920x1440"}, direction); break;
            case 1: CycleFlycastOption(m_host, "reicast_texture_filtering", {"0", "1", "2"}, direction); break;
            case 2: CycleFlycastOption(m_host, "reicast_anisotropic_filtering", {"off", "2", "4", "8", "16"}, direction); break;
            case 3: CycleFlycastOption(m_host, "reicast_mipmapping", {"disabled", "enabled"}, direction); break;
            case 4: CycleFlycastOption(m_host, "reicast_auto_skip_frame", {"disabled", "some", "more"}, direction); break;
            case 5: CycleFlycastOption(m_host, "reicast_frame_skipping", {"disabled", "1", "2", "3", "4", "5", "6"}, direction); break;
            case 6: CycleFlycastOption(m_host, "reicast_widescreen_hack", {"disabled", "enabled"}, direction); break;
            case 7:
            {
                // Display Mode: toggle Integer ↔ Display
                if (m_displayMode == FlycastDisplayMode::Display)
                    m_displayMode = FlycastDisplayMode::Integer;
                else
                    m_displayMode = FlycastDisplayMode::Display;
                if (m_displayMode == FlycastDisplayMode::Integer)
                    m_displaySize = FlycastDisplaySize::Auto;
                else
                    m_displaySize = FlycastDisplaySize::_4_3;
                ApplyScalingSettings(true);
                break;
            }
            case 8:
            {
                // Size: cycle through context-dependent options
                if (m_displayMode == FlycastDisplayMode::Integer)
                {
                    int s = (int)m_displaySize;
                    s += direction;
                    if (s < 4) s = 6;
                    if (s > 6) s = 4;
                    m_displaySize = (FlycastDisplaySize)s;
                }
                else
                {
                    int s = (int)m_displaySize;
                    s += direction;
                    if (s < 0) s = 3;
                    if (s > 3) s = 0;
                    m_displaySize = (FlycastDisplaySize)s;
                }
                ApplyScalingSettings(true);
                break;
            }
            }
        }
        else if (m_quickMenuSelection == 5)
        {
            switch (m_settingsSelection)
            {
            case 0: CycleFlycastOption(m_host, "reicast_threaded_rendering", {"disabled", "enabled"}, direction); break;
            case 1: {
                static const float kMultipliers[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
                constexpr int kCount = 5;
                float cur = m_host ? m_host->GetFastForwardMultiplier() : 2.0f;
                int idx = 2;
                for (int i = 0; i < kCount; ++i)
                {
                    if (cur <= kMultipliers[i] + 0.01f) { idx = i; break; }
                }
                idx = (idx + direction + kCount) % kCount;
                if (m_host)
                    m_host->SetFastForwardMultiplier(kMultipliers[idx]);
                break;
            }
            case 2:
                if (m_host)
                    m_host->SetFastForwardToggleMode(!m_host->GetFastForwardToggleMode());
                break;
            }
        }
        else if (m_settingsSelection == 0)
        {
            // Display Mode: toggle Integer ↔ Display
            if (m_displayMode == FlycastDisplayMode::Display)
                m_displayMode = FlycastDisplayMode::Integer;
            else
                m_displayMode = FlycastDisplayMode::Display;
            // Reset Size to first valid option for new mode
            if (m_displayMode == FlycastDisplayMode::Integer)
                m_displaySize = FlycastDisplaySize::Auto;
            else
                m_displaySize = FlycastDisplaySize::_4_3;
            ApplyScalingSettings(true);
        }
        else if (m_settingsSelection == 1)
        {
            // Size: cycle through context-dependent options
            if (m_displayMode == FlycastDisplayMode::Integer)
            {
                // Integer sizes: _1x(4), _2x(5), Auto(6)
                int s = (int)m_displaySize;
                s += direction;
                if (s < 4)
                    s = 6; // wrap to Auto
                if (s > 6)
                    s = 4; // wrap to _1x
                m_displaySize = (FlycastDisplaySize)s;
            }
            else
            {
                // Display sizes: Stretch(0), 4:3(1), 16:9(2), Original(3)
                int s = (int)m_displaySize;
                s += direction;
                if (s < 0)
                    s = 3; // wrap to Original
                if (s > 3)
                    s = 0; // wrap to Stretch
                m_displaySize = (FlycastDisplaySize)s;
            }
            ApplyScalingSettings(true);
        }
    }

    // Confirm (Logic for entering submenus or toggling)
    if (confirmPressed)
    {
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Confirm);
        if (m_currentMenu == OverlayMenu::QuickMenu)
        {
            switch (m_quickMenuSelection)
            {
            case 0: // Resume game
                Hide();
                return true;
            case 1: // Save State
                m_isSaveMode = true;
                m_currentMenu = OverlayMenu::SaveStates;
                m_animTimer = 0.4f;
                break;
            case 2: // Load State
                m_isSaveMode = false;
                m_currentMenu = OverlayMenu::SaveStates;
                m_animTimer = 0.4f;
                break;
            case 3: // Runtime widescreen cheat option.
            case 4: // Video settings
            case 5: // Core settings
                m_currentMenu = OverlayMenu::Settings;
                m_settingsSelection = 0;
                m_animTimer = 0.4f;
                break;
            case 6: // Swap disc
                OpenDiscBrowser();
                m_currentMenu = OverlayMenu::DiscSelect;
                m_sidebarFocused = false; // browser list owns the d-pad now
                m_animTimer = 0.4f;
                break;
            case 7: // Reset
                m_shouldReset = true;
                Hide();
                return true;
            case 8: // Exit
                m_shouldExit = true;
                break;
            }
        }
        else if (m_currentMenu == OverlayMenu::SaveStates)
        {
            if (m_host)
            {
                if (m_isSaveMode)
                {
                    m_host->SaveStateSlot(m_saveStateSlot);
                    m_currentMenu = OverlayMenu::QuickMenu;
                    m_sidebarFocused = true;
                }
                else
                {
                    m_host->LoadStateSlot(m_saveStateSlot);
                    Hide();
                    m_animTimer = 0.4f;
                    return true;
                }
            }
            else
            {
                m_currentMenu = OverlayMenu::QuickMenu;
            }
            m_animTimer = 0.4f;
        }
        else if (m_currentMenu == OverlayMenu::Settings)
        {
            if (m_quickMenuSelection == 3)
            {
                CycleFlycastOption(m_host, "reicast_widescreen_cheats", {"disabled", "enabled"}, 1);
            }
            else if (m_quickMenuSelection == 4)
            {
                switch (m_settingsSelection)
                {
                case 0: CycleFlycastOption(m_host, "reicast_internal_resolution", {"320x240", "640x480", "960x720", "1280x960", "1920x1440"}, 1); break;
                case 1: CycleFlycastOption(m_host, "reicast_texture_filtering", {"0", "1", "2"}, 1); break;
                case 2: CycleFlycastOption(m_host, "reicast_anisotropic_filtering", {"off", "2", "4", "8", "16"}, 1); break;
                case 3: CycleFlycastOption(m_host, "reicast_mipmapping", {"disabled", "enabled"}, 1); break;
                case 4: CycleFlycastOption(m_host, "reicast_auto_skip_frame", {"disabled", "some", "more"}, 1); break;
                case 5: CycleFlycastOption(m_host, "reicast_frame_skipping", {"disabled", "1", "2", "3", "4", "5", "6"}, 1); break;
                case 6: CycleFlycastOption(m_host, "reicast_widescreen_hack", {"disabled", "enabled"}, 1); break;
                }
            }
            else if (m_quickMenuSelection == 5)
            {
                switch (m_settingsSelection)
                {
                case 0: CycleFlycastOption(m_host, "reicast_threaded_rendering", {"disabled", "enabled"}, 1); break;
                case 1: {
                    static const float kMultipliers[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
                    constexpr int kCount = 5;
                    float cur = m_host ? m_host->GetFastForwardMultiplier() : 2.0f;
                    int idx = 2;
                    for (int i = 0; i < kCount; ++i)
                    {
                        if (cur <= kMultipliers[i] + 0.01f) { idx = i; break; }
                    }
                    idx = (idx + 1 + kCount) % kCount;
                    if (m_host)
                        m_host->SetFastForwardMultiplier(kMultipliers[idx]);
                    break;
                }
                case 2:
                    if (m_host)
                        m_host->SetFastForwardToggleMode(!m_host->GetFastForwardToggleMode());
                    break;
                }
            }
            // Confirm acts like Right.
            else if (m_settingsSelection == 0)
            {
                if (m_displayMode == FlycastDisplayMode::Display)
                    m_displayMode = FlycastDisplayMode::Integer;
                else
                    m_displayMode = FlycastDisplayMode::Display;
                if (m_displayMode == FlycastDisplayMode::Integer)
                    m_displaySize = FlycastDisplaySize::Auto;
                else
                    m_displaySize = FlycastDisplaySize::_4_3;
                ApplyScalingSettings(true);
            }
            else if (m_settingsSelection == 1)
            {
                if (m_displayMode == FlycastDisplayMode::Integer)
                {
                    int s = (int)m_displaySize;
                    s = (s >= 6) ? 4 : s + 1;
                    m_displaySize = (FlycastDisplaySize)s;
                }
                else
                {
                    int s = (int)m_displaySize;
                    s = (s >= 3) ? 0 : s + 1;
                    m_displaySize = (FlycastDisplaySize)s;
                }
                ApplyScalingSettings(true);
            }
        }
        else if (m_currentMenu == OverlayMenu::DiscSelect)
        {
            if (m_host && !m_discBrowserEntries.empty())
            {
                const DiscBrowserEntry &entry = m_discBrowserEntries[m_discBrowserSelection];
                if (entry.isDir)
                {
                    m_discBrowserDir = entry.path;
                    RefreshDiscBrowser();
                }
                else
                {
                    m_host->SwapDisc(entry.path);
                    m_host->UpdateGamePath(entry.path);
                    Hide();
                    m_animTimer = 0.4f;
                }
                return true;
            }
        }
    }
    // Back
    if (backPressed)
    {
        if (m_currentMenu == OverlayMenu::DiscSelect)
        {
            // Browse up one directory, or back to the quick menu at the root.
            const size_t slash = m_discBrowserDir.find_last_of("/\\");
            if (slash != std::string::npos && slash > 0)
            {
                m_discBrowserDir = m_discBrowserDir.substr(0, slash);
                RefreshDiscBrowser();
            }
            else
            {
                m_currentMenu = OverlayMenu::QuickMenu;
                m_sidebarFocused = true;
            }
        }
        else
        {
            m_sidebarFocused = true;
        }
        GBAStationAudio::PlayUiSoundGlobal(GBAStationAudio::UiSound::Cancel);
    }

    return true;
}

//==============================================================================
// Disc Scanning & Menu
//==============================================================================

static std::string NormalizeDiscPath(const std::string &path)
{
    std::string result = path;
    for (char &c : result)
    {
        if (c == '\\')
            c = '/';
    }
    return result;
}

//==============================================================================
// Disc Browser (manual disc swap file picker)
//==============================================================================

static bool IsDiscFile(const std::string &path)
{
    const std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : std::string();
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == ".chd" || lower == ".cue" || lower == ".gdi" ||
           lower == ".cdi" || lower == ".iso" || lower == ".m3u";
}

void GBAStationOverlay::OpenDiscBrowser()
{
    m_discBrowserDir.clear();
    if (m_host)
    {
        std::string current = m_host->GetGamePath();
        if (current.size() >= 2 && current.front() == '"' && current.back() == '"')
            current = current.substr(1, current.size() - 2);
        current = NormalizeDiscPath(current);
        const size_t slash = current.find_last_of("/\\");
        if (slash != std::string::npos)
            m_discBrowserDir = current.substr(0, slash);
    }
    if (m_discBrowserDir.empty())
        m_discBrowserDir = "sdmc:/GBAStation/roms";
    RefreshDiscBrowser();
}

void GBAStationOverlay::RefreshDiscBrowser()
{
    m_discBrowserEntries.clear();
    m_discBrowserSelection = 0;
    m_discBrowserScrollY = 0.0f;
    m_discBrowserTargetScrollY = 0.0f;

    DIR *dir = opendir(m_discBrowserDir.c_str());
    if (!dir)
        return;

    std::vector<DiscBrowserEntry> dirs;
    std::vector<DiscBrowserEntry> files;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        const std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string full = m_discBrowserDir + "/" + name;
        struct stat st;
        const bool isDir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        DiscBrowserEntry entry;
        entry.name = name;
        entry.path = full;
        entry.isDir = isDir;
        if (isDir)
            dirs.push_back(entry);
        else if (IsDiscFile(full))
            files.push_back(entry);
    }
    closedir(dir);

    std::sort(dirs.begin(), dirs.end(), [](const DiscBrowserEntry &a, const DiscBrowserEntry &b) {
        return a.name < b.name;
    });
    std::sort(files.begin(), files.end(), [](const DiscBrowserEntry &a, const DiscBrowserEntry &b) {
        return a.name < b.name;
    });

    for (const auto &e : dirs)
        m_discBrowserEntries.push_back(e);
    for (const auto &e : files)
        m_discBrowserEntries.push_back(e);
}

void GBAStationOverlay::RenderDiscBrowser(ImDrawList *dl, ImVec2 displaySize)
{
    const float scale = OverlayScale();
    ImFont *font = ImGui::GetFont();
    if (!font)
        return;

    const float contentX = 268.0f * scale;
    const float contentW = displaySize.x - contentX - 60.0f * scale;
    const float viewTop = 150.0f * scale;
    const float rowH = 52.0f * scale;
    const float contentH = 470.0f * scale;
    const int visible = std::min((int)(contentH / rowH), (int)m_discBrowserEntries.size());

    // Path header.
    const float ease = 1.0f;
    dl->AddText(font, 21.0f * scale, ImVec2(contentX, viewTop - 40.0f * scale),
                IM_COL32(184, 204, 224, 219), m_discBrowserDir.c_str());

    // Smooth scroll.
    m_discBrowserScrollY += (m_discBrowserTargetScrollY - m_discBrowserScrollY) * 0.25f;
    int first = 0;
    if (!m_discBrowserEntries.empty())
    {
        const float maxScroll = (float)m_discBrowserEntries.size() * rowH - contentH;
        if (maxScroll > 0.0f)
            first = (int)(m_discBrowserScrollY / rowH);
        first = std::clamp(first, 0, (int)m_discBrowserEntries.size() - visible);
    }

    for (int row = 0; row < visible; ++row)
    {
        const int index = first + row;
        if (index < 0 || index >= (int)m_discBrowserEntries.size())
            break;
        const bool selected = index == m_discBrowserSelection;
        const float y = viewTop + row * rowH;
        if (selected)
        {
            dl->AddRectFilled(ImVec2(contentX, y), ImVec2(contentX + contentW, y + rowH - 6.0f * scale),
                              IM_COL32(38, 62, 92, 170), 10.0f * scale);
            dl->AddRect(ImVec2(contentX, y), ImVec2(contentX + contentW, y + rowH - 6.0f * scale),
                        IM_COL32(90, 190, 255, 200), 10.0f * scale, 0, 2.0f);
        }
        char icon[8];
        EncodeUtf8(icon, m_discBrowserEntries[index].isDir ? 0xE2C8 : 0xE161);
        dl->AddText(font, 26.0f * scale, ImVec2(contentX + 12.0f * scale, y + 10.0f * scale),
                    IM_COL32(130, 190, 255, 235), icon);
        dl->AddText(font, 22.0f * scale,
                    ImVec2(contentX + 52.0f * scale, y + (rowH - 22.0f * scale) * 0.5f),
                    selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(204, 224, 244, 225),
                    m_discBrowserEntries[index].name.c_str());
    }
}

static int GetDiscExtensionPriority(const std::string &ext)
{
    if (ext == ".chd") return 1;
    if (ext == ".cue") return 2;
    if (ext == ".gdi") return 3;
    if (ext == ".cdi") return 4;
    if (ext == ".iso") return 5;
    return 99;
}

void GBAStationOverlay::ScanForDiscs()
{
    m_discs.clear();
    m_discSelection = 0;
    m_discScrollY = 0.0f;
    m_discTargetScrollY = 0.0f;

    if (!m_host) return;
    std::string currentPath = m_host->GetGamePath();
    if (currentPath.empty()) return;

    // Strip quotes if present
    if (currentPath.front() == '"' && currentPath.back() == '"')
    {
        currentPath = currentPath.substr(1, currentPath.size() - 2);
    }

    currentPath = NormalizeDiscPath(currentPath);

    std::string dirname;
    std::string basename;
    size_t lastSlash = currentPath.find_last_of("/\\");
    if (lastSlash != std::string::npos)
    {
        dirname = currentPath.substr(0, lastSlash);
        basename = currentPath.substr(lastSlash + 1);
    }
    else
    {
        dirname = ".";
        basename = currentPath;
    }

    std::string lowerBase = basename;
    std::transform(lowerBase.begin(), lowerBase.end(), lowerBase.begin(), ::tolower);

    // M3U Parsing
    if (lowerBase.length() >= 4 && lowerBase.substr(lowerBase.length() - 4) == ".m3u")
    {
        FILE *fp = fopen(currentPath.c_str(), "r");
        if (fp)
        {
            char line[1024];
            int discIndex = 1;
            while (fgets(line, sizeof(line), fp))
            {
                std::string strLine(line);
                strLine.erase(strLine.find_last_not_of(" \n\r\t") + 1);
                size_t startpos = strLine.find_first_not_of(" \n\r\t");
                if (std::string::npos != startpos)
                    strLine = strLine.substr(startpos);
                else
                    strLine.clear();

                if (strLine.empty() || strLine[0] == '#') continue;

                std::string discRelPath = dirname + "/" + strLine;
                std::string normalizedPath = NormalizeDiscPath(discRelPath);

                DiscEntry entry;
                entry.displayName = tr("emulator_disc") + " " + std::to_string(discIndex);
                entry.romPath = normalizedPath;
                m_discs.push_back(entry);
                discIndex++;
            }
            fclose(fp);
            return;
        }
    }

    // Check for disc pattern in current filename
    const char *keywords[] = {"disc", "disk", "cd"};
    bool foundPat = false;
    for (const char *kw : keywords)
    {
        for (int n = 1; n <= 10; n++)
        {
            std::string pattern = std::string("(") + kw + " " + std::to_string(n) + ")";
            if (lowerBase.find(pattern) != std::string::npos)
            {
                foundPat = true;
                break;
            }
        }
        if (foundPat) break;
    }

    if (!foundPat) return;

    // Extract prefix (everything before first parenthesis)
    size_t firstParen = lowerBase.find('(');
    std::string prefix = lowerBase;
    if (firstParen != std::string::npos)
        prefix = lowerBase.substr(0, firstParen);
    prefix.erase(prefix.find_last_not_of(" \n\r\t") + 1);

    // Determine directories to scan
    std::vector<std::string> scanDirs;
    scanDirs.push_back(dirname);

    std::string lowerDir = dirname;
    std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(), ::tolower);
    bool isNested = false;
    for (const char *kw : keywords)
    {
        if (lowerDir.find(kw) != std::string::npos)
        {
            isNested = true;
            break;
        }
    }
    if (isNested)
        scanDirs.push_back(dirname + "/..");

    // Scan directories
    std::map<std::string, DiscEntry> bestDiscs;

    for (const auto &scanDir : scanDirs)
    {
        DIR *dir;
        struct dirent *ent;
        if ((dir = opendir(scanDir.c_str())) != NULL)
        {
            while ((ent = readdir(dir)) != NULL)
            {
                std::string filename = ent->d_name;
                if (filename == "." || filename == "..") continue;

                std::string currentFileDir = scanDir;
                bool isSubDirItem = false;

                if (ent->d_type == DT_DIR)
                {
                    currentFileDir = scanDir + "/" + filename;
                    isSubDirItem = true;
                }

                DIR *subDir = NULL;
                struct dirent *subEnt = NULL;
                bool hasSubDir = false;

                if (isSubDirItem)
                {
                    subDir = opendir(currentFileDir.c_str());
                    if (subDir)
                    {
                        hasSubDir = true;
                        subEnt = readdir(subDir);
                    }
                    else
                        continue;
                }
                else
                {
                    subEnt = ent;
                }

                while (subEnt != NULL)
                {
                    std::string actualFilename = subEnt->d_name;
                    if (actualFilename == "." || actualFilename == "..")
                    {
                        if (hasSubDir) { subEnt = readdir(subDir); continue; }
                        else break;
                    }

                    std::string lowerFilename = actualFilename;
                    std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);

                    std::string extFound = "";
                    size_t lastDot = lowerFilename.find_last_of('.');
                    if (lastDot != std::string::npos)
                        extFound = lowerFilename.substr(lastDot);

                    int priority = GetDiscExtensionPriority(extFound);

                    // Strip extension for prefix comparison
                    std::string lowerNameNoExt = lowerFilename;
                    if (lastDot != std::string::npos) lowerNameNoExt = lowerFilename.substr(0, lastDot);

                    // Must start with prefix (not just contain it)
                    if (priority <= 5 && lowerNameNoExt.find(prefix) == 0)
                    {
                        // Reject extra title words between prefix and first '('
                        std::string afterPrefix = lowerNameNoExt.substr(prefix.size());

                        // Remove anything inside [] entirely
                        int bracketDepth = 0;
                        std::string cleanAfterPrefix;
                        for (size_t i = 0; i < afterPrefix.length(); i++)
                        {
                            if (afterPrefix[i] == '[') bracketDepth++;
                            else if (afterPrefix[i] == ']') bracketDepth = std::max(0, bracketDepth - 1);
                            else if (bracketDepth == 0) cleanAfterPrefix += afterPrefix[i];
                        }

                        size_t parenPos = cleanAfterPrefix.find('(');
                        std::string beforeParen = (parenPos != std::string::npos)
                                                      ? cleanAfterPrefix.substr(0, parenPos)
                                                      : cleanAfterPrefix;
                        beforeParen.erase(beforeParen.find_last_not_of(" \t") + 1);
                        beforeParen.erase(0, beforeParen.find_first_not_of(" \t"));

                        if (beforeParen.empty())
                        {
                            bool foundDiscForFile = false;
                            for (const char *kw : keywords)
                            {
                                for (int n = 1; n <= 10; n++)
                                {
                                    std::string pattern = std::string("(") + kw + " " + std::to_string(n) + ")";
                                    if (lowerFilename.find(pattern) != std::string::npos)
                                    {
                                        DiscEntry entry;
                                        entry.displayName = tr("emulator_disc") + " " + std::to_string(n);
                                        entry.romPath = NormalizeDiscPath(currentFileDir + "/" + actualFilename);

                                        if (bestDiscs.find(entry.displayName) == bestDiscs.end())
                                        {
                                            bestDiscs[entry.displayName] = entry;
                                        }
                                        else
                                        {
                                            std::string existingExt = "";
                                            std::string existingLower = bestDiscs[entry.displayName].romPath;
                                            std::transform(existingLower.begin(), existingLower.end(), existingLower.begin(), ::tolower);
                                            size_t extDot = existingLower.find_last_of('.');
                                            if (extDot != std::string::npos) existingExt = existingLower.substr(extDot);

                                            if (priority < GetDiscExtensionPriority(existingExt))
                                                bestDiscs[entry.displayName] = entry;
                                        }
                                        foundDiscForFile = true;
                                        break;
                                    }
                                }
                                if (foundDiscForFile) break;
                            }
                        }
                    }

                    if (hasSubDir)
                        subEnt = readdir(subDir);
                    else
                        break;
                }

                if (hasSubDir && subDir)
                    closedir(subDir);
            }
            closedir(dir);
        }
    }

    for (const auto &pair : bestDiscs)
        m_discs.push_back(pair.second);

    // Sort by display name
    std::sort(m_discs.begin(), m_discs.end(), [](const DiscEntry &a, const DiscEntry &b) {
        return a.displayName < b.displayName;
    });

    // Set selection to current disc
    for (int i = 0; i < (int)m_discs.size(); i++)
    {
        if (m_discs[i].romPath == currentPath)
        {
            m_discSelection = i;
            break;
        }
    }
}

void GBAStationOverlay::RenderDiscMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const float menuWidth = 400.0f * scale;
    const float itemHeight = 64.0f * scale;
    const int numItems = m_discs.empty() ? 1 : (int)m_discs.size();

    const int maxVisible = 4;
    const int visibleItems = std::min(numItems, maxVisible);
    const float paddingY = 16.0f * scale;
    const float contentHeight = visibleItems * itemHeight + (paddingY * 2.0f);

    ImVec2 menuSize(menuWidth, contentHeight);

    float t = m_animTimer / 0.4f;
    if (t > 1.0f) t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float targetY = (displaySize.y - menuSize.y) / 2.0f;
    float startY = displaySize.y + (100.0f * scale);
    float currentY = startY + (targetY - startY) * easeOut;

    ImVec2 menuPos((displaySize.x - menuSize.x) / 2.0f, currentY);
    ImVec2 p0 = menuPos;
    ImVec2 p1 = ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y);

    const float cornerRadius = 16.0f * scale;

    ImU32 containerColor;
    if (m_isDarkMode)
        containerColor = IM_COL32(45, 45, 45, (int)(255 * easeOut));
    else
        containerColor = IM_COL32(242, 245, 248, (int)(255 * easeOut));

    dl->AddRectFilled(p0, p1, containerColor, cornerRadius);

    ImFont *font = ImGui::GetFont();
    float baseFontSize = ImGui::GetFontSize();
    float smallFontSize = baseFontSize * 0.85f;

    ImVec2 clipP0(p0.x, p0.y + paddingY);
    ImVec2 clipP1(p1.x, p1.y - paddingY);
    dl->PushClipRect(clipP0, clipP1, true);

    for (int i = 0; i < numItems; i++)
    {
        bool isSelected = m_discs.empty() ? true : (m_discSelection == i);
        float itemY = clipP0.y + i * itemHeight - m_discScrollY;

        if (itemY + itemHeight < clipP0.y || itemY > clipP1.y)
            continue;

        ImVec2 itemMin(menuPos.x, itemY);
        ImVec2 itemMax(menuPos.x + menuSize.x, itemY + itemHeight);

        if (isSelected)
        {
            ImU32 selectedColor;
            if (m_isDarkMode)
                selectedColor = IM_COL32(60, 60, 60, (int)(255 * easeOut));
            else
                selectedColor = IM_COL32(190, 195, 205, (int)(255 * easeOut));

            dl->AddRectFilled(itemMin, itemMax, selectedColor);
        }

        std::string displayName = m_discs.empty() ? tr("emulator_no_discs") : m_discs[i].displayName;
        ImVec2 textSize = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, displayName.c_str());
        float textX = itemMin.x + 20.0f * scale;
        float textY = itemMin.y + (itemHeight - textSize.y) / 2;

        ImU32 textColor;
        if (m_isDarkMode)
        {
            if (isSelected)
                textColor = IM_COL32(255, 255, 255, (int)(255 * easeOut));
            else
                textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));
        }
        else
        {
            if (isSelected)
                textColor = IM_COL32(60, 60, 70, (int)(255 * easeOut));
            else
                textColor = IM_COL32(90, 90, 100, (int)(255 * easeOut));
        }

        dl->AddText(font, smallFontSize, ImVec2(textX, textY), textColor, displayName.c_str());
    }

    dl->PopClipRect();

    // Scroll shadow indicators
    bool needsScroll = numItems > maxVisible;
    if (needsScroll)
    {
        float maxScroll = (float)m_discs.size() * itemHeight - contentHeight + (paddingY * 2.0f);
        float shadowH = 20.0f * scale;
        ImU32 shadowStart = IM_COL32(0, 0, 0, (int)(40 * easeOut));
        ImU32 shadowEnd = IM_COL32(0, 0, 0, 0);

        if (m_discScrollY > 1.0f)
        {
            float y1 = clipP0.y;
            float y2 = clipP0.y + shadowH;
            dl->AddRectFilledMultiColor(ImVec2(menuPos.x, y1), ImVec2(menuPos.x + menuSize.x, y2),
                                        shadowStart, shadowStart, shadowEnd, shadowEnd);
        }

        if (m_discScrollY < maxScroll - 1.0f)
        {
            float y1 = clipP1.y - shadowH;
            float y2 = clipP1.y;
            dl->AddRectFilledMultiColor(ImVec2(menuPos.x, y1), ImVec2(menuPos.x + menuSize.x, y2),
                                        shadowEnd, shadowEnd, shadowStart, shadowStart);
        }
    }
}

//==============================================================================
// Settings Persistence
//==============================================================================

void GBAStationOverlay::LoadCoreSettings()
{
    // Read from the shared core config (same file the launcher settings uses)
#ifdef __SWITCH__
    std::string configPath = "sdmc:/GBAStation/config/cores/flycast.jsonc";
#else
    std::string configPath = "GBAStation/config/cores/flycast.jsonc";
#endif

    std::ifstream file(configPath);
    if (file.is_open())
    {
        try
        {
            auto j = nlohmann::json::parse(file, nullptr, false, true);
            file.close();

            if (!j.is_discarded())
            {
                // display_mode: "Integer" or "Display"
                if (j.contains("display_mode") && j["display_mode"].is_string())
                {
                    std::string v = j["display_mode"].get<std::string>();
                    if (v == "Integer")
                        m_displayMode = FlycastDisplayMode::Integer;
                    else
                        m_displayMode = FlycastDisplayMode::Display;
                }
                else
                {
                    m_displayMode = FlycastDisplayMode::Display;
                }

                // display_size: context-dependent string
                if (j.contains("display_size") && j["display_size"].is_string())
                {
                    std::string v = j["display_size"].get<std::string>();
                    if (v == "Stretch")
                        m_displaySize = FlycastDisplaySize::Stretch;
                    else if (v == "16:9")
                        m_displaySize = FlycastDisplaySize::_16_9;
                    else if (v == "Original")
                        m_displaySize = FlycastDisplaySize::Original;
                    else if (v == "1x")
                        m_displaySize = FlycastDisplaySize::_1x;
                    else if (v == "2x")
                        m_displaySize = FlycastDisplaySize::_2x;
                    else if (v == "Auto")
                        m_displaySize = FlycastDisplaySize::Auto;
                    else
                        m_displaySize = FlycastDisplaySize::_4_3;
                }
                else
                {
                    m_displaySize = FlycastDisplaySize::_4_3;
                }
            }
            else
            {
                m_displayMode = FlycastDisplayMode::Display;
                m_displaySize = FlycastDisplaySize::_4_3;
            }
        }
        catch (...)
        {
            m_displayMode = FlycastDisplayMode::Display;
            m_displaySize = FlycastDisplaySize::_4_3;
        }
    }
    else
    {
        m_displayMode = FlycastDisplayMode::Display;
        m_displaySize = FlycastDisplaySize::_4_3;
    }

    const std::string modeOverride = ReadGBAStationConfigValue("core.flycast.display_mode");
    if (!modeOverride.empty())
        m_displayMode = modeOverride == "Integer" ? FlycastDisplayMode::Integer
                                                  : FlycastDisplayMode::Display;
    const std::string sizeOverride = ReadGBAStationConfigValue("core.flycast.display_size");
    if (!sizeOverride.empty())
    {
        if (sizeOverride == "Stretch")
            m_displaySize = FlycastDisplaySize::Stretch;
        else if (sizeOverride == "16:9")
            m_displaySize = FlycastDisplaySize::_16_9;
        else if (sizeOverride == "Original")
            m_displaySize = FlycastDisplaySize::Original;
        else if (sizeOverride == "1x")
            m_displaySize = FlycastDisplaySize::_1x;
        else if (sizeOverride == "2x")
            m_displaySize = FlycastDisplaySize::_2x;
        else if (sizeOverride == "Auto")
            m_displaySize = FlycastDisplaySize::Auto;
        else if (sizeOverride == "4:3")
            m_displaySize = FlycastDisplaySize::_4_3;
    }

    ApplyScalingSettings(false);
}

void GBAStationOverlay::SaveCoreSettings()
{
    // Merge into the shared core config (same file the launcher settings uses)
#ifdef __SWITCH__
    std::string configPath = "sdmc:/GBAStation/config/cores/flycast.jsonc";
#else
    std::string configPath = "GBAStation/config/cores/flycast.jsonc";
#endif

    try
    {
        // Read existing config first so we only update our keys
        nlohmann::json j = nlohmann::json::object();
        {
            std::ifstream in(configPath);
            if (in.is_open())
            {
                auto parsed = nlohmann::json::parse(in, nullptr, false, true);
                in.close();
                if (!parsed.is_discarded())
                    j = parsed;
            }
        }

        // display_mode → string
        j["display_mode"] = (m_displayMode == FlycastDisplayMode::Integer) ? "Integer" : "Display";

        // display_size → string
        const char *sizeStr = "4:3";
        switch (m_displaySize)
        {
        case FlycastDisplaySize::Stretch:
            sizeStr = "Stretch";
            break;
        case FlycastDisplaySize::_4_3:
            sizeStr = "4:3";
            break;
        case FlycastDisplaySize::_16_9:
            sizeStr = "16:9";
            break;
        case FlycastDisplaySize::Original:
            sizeStr = "Original";
            break;
        case FlycastDisplaySize::_1x:
            sizeStr = "1x";
            break;
        case FlycastDisplaySize::_2x:
            sizeStr = "2x";
            break;
        case FlycastDisplaySize::Auto:
            sizeStr = "Auto";
            break;
        default:
            break;
        }
        j["display_size"] = sizeStr;

        std::ofstream out(configPath);
        if (out.is_open())
        {
            out << j.dump(4);
            out.close();
        }
    }
    catch (...)
    {
    }
}

void GBAStationOverlay::ApplyScalingSettings(bool save)
{
    if (save)
    {
        SaveCoreSettings();
    }
}

// Helper to load SVG
void GBAStationOverlay::LoadSVGIcon()
{
    if (!m_host)
        return;

    // Embed SVG to avoid file path issues
    const char *svgContent = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 448 512"><path fill="#FFFFFF" d="M338.8-9.9c11.9 8.6 16.3 24.2 10.9 37.8L271.3 224 416 224c13.5 0 25.5 8.4 30.1 21.1s.7 26.9-9.6 35.5l-288 240c-11.3 9.4-27.4 9.9-39.3 1.3s-16.3-24.2-10.9-37.8L176.7 288 32 288c-13.5 0-25.5-8.4-30.1-21.1s-.7-26.9 9.6-35.5l288-240c11.3-9.4 27.4-9.9 39.3-1.3z"/></svg>)";

    char *input = strdup(svgContent);
    if (!input)
        return;

    NSVGimage *image = nsvgParse(input, "px", 96);
    free(input);
    if (!image)
        return;

    float scale = 64.0f / image->height;
    int w = (int)(image->width * scale);
    int h = (int)(image->height * scale);

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast)
    {
        nsvgDelete(image);
        return;
    }

    unsigned char *img = (unsigned char *)malloc(w * h * 4);
    if (!img)
    {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return;
    }

    nsvgRasterize(rast, image, 0, 0, scale, img, w, h, w * 4);

    if (m_boltTexture)
        m_host->DestroyTexture(m_boltTexture);
    m_boltTexture = m_host->CreateTextureRGBA(img, w, h);
    m_boltWidth = w;
    m_boltHeight = h;

    free(img);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
}

// ... existing RenderStatusBar ...

void GBAStationOverlay::RenderStatusBar(ImDrawList *dl, ImVec2 displaySize)
{
    if (m_animTimer <= 0.0f)
        return;

    const float scale = OverlayScale();

    // Animation: Fade in
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    float alpha = ease;

    // Config
    const float BAR_HEIGHT = 50.0f * scale;
    const float TOP_MARGIN = 32.0f * scale;
    const float SIDE_MARGIN = 32.0f * scale;
    const float ITEM_SPACING = 20.0f * scale;

    ImFont *font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    // FORCE DARK MODE per user request

    // Time
    std::time_t now = std::time(nullptr);
    std::tm *localTime = std::localtime(&now);
    char timeStr[16];
    char periodStr[16];

    bool is24h = (m_hourFormat == "24h");

    float timeW = 0.0f;
    float periodFontSize = fontSize * 0.55f;
    float periodW = 0.0f;

    if (is24h)
    {
        std::strftime(timeStr, sizeof(timeStr), "%H:%M", localTime);
        timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x;
        periodStr[0] = '\0';
    }
    else
    {
        std::strftime(timeStr, sizeof(timeStr), "%I:%M", localTime);
        std::strftime(periodStr, sizeof(periodStr), "%p", localTime);
        timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x;
        periodW = font->CalcTextSizeA(periodFontSize, FLT_MAX, 0.0f, periodStr).x;
    }

    float totalWidth = timeW;
    if (!is24h)
        totalWidth += 4.0f * scale + periodW;

    // Battery
    bool showBattery = true;
    if (showBattery)
    {
        totalWidth += ITEM_SPACING + 34.0f * scale; // Battery icon width + space
    }

    // Margins inside bar
    float PADDING = 20.0f * scale;
    totalWidth += PADDING * 2;

    // Position
    float offsetY = (1.0f - ease) * -20.0f * scale;
    float barX = displaySize.x - totalWidth - SIDE_MARGIN;
    float barY = TOP_MARGIN + offsetY;

    // Text Color
    ImU32 textColor = IM_COL32(200, 200, 200, (int)(255 * alpha)); // Light Grey for Dark Mode
    ImU32 iconColor = textColor;

    float cursorX = barX + PADDING;
    float centerY = barY + BAR_HEIGHT * 0.5f;

    // Draw Time
    ImVec2 timePos(cursorX, centerY - fontSize * 0.5f);
    dl->AddText(font, fontSize, timePos, textColor, timeStr);
    cursorX += timeW;

    if (!is24h)
    {
        cursorX += 4.0f * scale;
        ImVec2 periodPos(cursorX, centerY - fontSize * 0.5f + (fontSize - periodFontSize) * 0.9f);
        dl->AddText(font, periodFontSize, periodPos, textColor, periodStr);
        cursorX += periodW;
    }

    // Draw Battery
    if (showBattery)
    {
        cursorX += ITEM_SPACING;

        float bodyWidth = 32.0f * scale;
        float bodyHeight = 20.0f * scale;
        float tipWidth = 4.0f * scale;
        float tipHeight = 10.0f * scale;

        ImVec2 batteryPos(cursorX, centerY - bodyHeight * 0.5f);
        ImVec2 bodyMin = batteryPos;
        ImVec2 bodyMax = bodyMin + ImVec2(bodyWidth, bodyHeight);

        // Stroke
        dl->AddRect(bodyMin, bodyMax, iconColor, 3.0f * scale, 0, 2.0f * scale);

        // Tip
        ImVec2 tipMin = ImVec2(bodyMax.x, batteryPos.y + (bodyHeight - tipHeight) * 0.5f);
        ImVec2 tipMax = tipMin + ImVec2(tipWidth, tipHeight);
        dl->AddRectFilled(tipMin, tipMax, iconColor, 2.0f * scale, ImDrawFlags_RoundCornersRight);

        // Fill
        float pct = m_batteryLevel / 100.0f;
        if (pct > 1.0f)
            pct = 1.0f;
        if (pct < 0.0f)
            pct = 0.0f;

        float pad = 4.0f * scale;
        float fillMaxW = bodyWidth - pad * 2;
        float currentFillW = fillMaxW * pct;
        if (currentFillW < 2.0f * scale && pct > 0)
            currentFillW = 2.0f * scale;

        if (currentFillW > 0)
        {
            ImVec2 fillMin = bodyMin + ImVec2(pad, pad);
            ImVec2 fillMax = fillMin + ImVec2(currentFillW, bodyHeight - pad * 2);
            dl->AddRectFilled(fillMin, fillMax, iconColor, 1.0f * scale);
        }

        // Bolt if charging: TEXTURE BASED
        if (m_isCharging)
        {
            // Load texture if needed (Lazy load or init check)
            if (m_boltTexture == 0)
            {
                LoadSVGIcon();
            }

            if (m_boltTexture != 0)
            {
                float iconH = 16.0f * scale;
                float iconW = iconH * ((float)m_boltWidth / (float)m_boltHeight);

                ImVec2 iconPos = ImVec2(tipMax.x + 6.0f * scale, batteryPos.y + (bodyHeight - iconH) * 0.5f);

                // Fade in based on charging progress
                float fadeProgress = (m_chargingStateProgress - 0.5f) * 2.0f;
                if (fadeProgress < 0.0f)
                    fadeProgress = 0.0f;

                int alphaBolt = (int)(255 * fadeProgress * ease);

                if (alphaBolt > 0)
                {
                    ImVec2 p_min = iconPos;
                    ImVec2 p_max = p_min + ImVec2(iconW, iconH);

                    // Tint
                    ImU32 tint = IM_COL32(235, 235, 235, alphaBolt);

                    dl->AddImage(m_boltTexture,
                                 p_min, p_max,
                                 ImVec2(0, 0), ImVec2(1, 1),
                                 tint);
                }
            }
        }
    }
}

//==============================================================================
// RetroAchievements Overlay
//==============================================================================

void GBAStationOverlay::EnsureRAIconLoaded() {
    // Rasterize assets/ra.svg once and upload it through the host so RA toasts
    // (the "Playing:" session toast, mastered/leaderboard alerts) have an icon,
    // matching the other GBAStation cores. Badge textures for individual achievements
    // are still uploaded by the host on demand; this only owns the generic icon.
    if (!m_host)
        return;
    IOverlayRAHost *ra = m_host->RA();
    if (!ra)
        return;                       // standalone path: no RA host
    if (ra->IconTexture() != 0)
        return;                       // already uploaded

    // Retry every frame until the overlay backend is ready (CreateTextureRGBA
    // returns 0 before the Vulkan overlay is up); cheap until it succeeds.
#ifdef __SWITCH__
    const char *svgPath = "romfs:/assets/ra.svg";
#else
    const char *svgPath = "GBAStation/assets/ra.svg";
#endif
    NSVGimage *image = nsvgParseFromFile(svgPath, "px", 96);
    if (!image)
        return;

    float scale = 64.0f / image->height;
    int w = (int)(image->width * scale);
    int h = (int)(image->height * scale);
    if (w <= 0 || h <= 0) {
        nsvgDelete(image);
        return;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return;
    }

    unsigned char *img = (unsigned char *)malloc((size_t)w * h * 4);
    if (!img) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return;
    }

    nsvgRasterize(rast, image, 0, 0, scale, img, w, h, w * 4);
    ImTextureID tex = m_host->CreateTextureRGBA(img, w, h);
    if (tex != 0) {
        ra->SetIconTexture(tex);
        m_raIconLoadAttempted = true;
    }

    free(img);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
}

void GBAStationOverlay::ResolveNotificationTextures() {
    IOverlayRAHost *ra = m_host ? m_host->RA() : nullptr;
    if (!ra) return;
    std::lock_guard<std::mutex> lock(ra->Mutex());
    auto& notifications = ra->Notifications();
    if (notifications.empty()) return;

    for (auto& n : notifications) {
        if (n.textureId != 0) continue;
        if (n.badge_name.empty()) continue;

        if (n.badge_name == "ra_icon") {
            n.textureId = ra->IconTexture();
        } else {
            // Only check the in-memory texture cache — NO disk I/O, NO stbi_load.
            ImTextureID t = ra->BadgeTexture(n.badge_name);
            if (t != 0)
                n.textureId = t;
            // If not cached yet, leave textureId=0; the badge appears once the
            // host uploads it (next frame).
        }
    }
}

void GBAStationOverlay::RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime) {
    IOverlayRAHost *ra = m_host ? m_host->RA() : nullptr;
    if (!ra) return;
    std::lock_guard<std::mutex> lock(ra->Mutex());
    auto& notifications = ra->Notifications();
    if (notifications.empty()) return;

    float scale = OverlayScale();
    ImFont *font = ImGui::GetFont();
    ImFont *descFont = m_descFont ? m_descFont : font;
    if (!m_descFont && ImGui::GetIO().Fonts->Fonts.Size > 1) {
        descFont = ImGui::GetIO().Fonts->Fonts[1];
    }
    float descFontSize = ImGui::GetFontSize() * 0.65f;
    float titleFontSize = ImGui::GetFontSize() * 0.85f;

    // Alert dimensions
    float alertW = 420.0f * scale;
    float alertH = 100.0f * scale;
    float padding = 12.0f * scale;
    float margin = 16.0f * scale;
    float spacing = 8.0f * scale;
    float cornerRadius = 14.0f * scale;
    float badgeSize = 76.0f * scale;
    float badgeRadius = 4.0f * scale;
    float badgeMargin = 12.0f * scale;

    RAAlertPosition pos = ra->AlertPosition();
    bool isTop = (pos == RAAlertPosition::TopLeft || pos == RAAlertPosition::TopRight);
    bool isRight = (pos == RAAlertPosition::TopRight || pos == RAAlertPosition::BottomRight);

    // Update timers and remove expired
    for (auto& n : notifications) {
        n.timer += deltaTime;
    }
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [](const RANotification& n) { return n.timer >= n.duration; }),
        notifications.end());

    // Render each notification
    for (size_t i = 0; i < notifications.size(); i++) {
        auto& n = notifications[i];

        // Calculate slide animation
        float slideProgress;
        if (n.timer < n.slideIn) {
            float t = n.timer / n.slideIn;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else if (n.timer > n.duration - n.slideOut) {
            float t = (n.duration - n.timer) / n.slideOut;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else {
            slideProgress = 1.0f;
        }

        // Calculate position
        float stackOffset = (float)i * (alertH + spacing);
        float anchorX = isRight ? (displaySize.x - alertW - margin) : margin;
        float anchorY = isTop ? (margin + stackOffset) : (displaySize.y - margin - alertH - stackOffset);
        float slideOffsetY = isTop
            ? -(alertH + margin + stackOffset) * (1.0f - slideProgress)
            : (alertH + margin + stackOffset) * (1.0f - slideProgress);

        float drawY = anchorY + slideOffsetY;
        int alpha = (int)(230 * slideProgress);
        if (alpha <= 0) continue;

        ImVec2 rectMin(anchorX, drawY);
        ImVec2 rectMax(anchorX + alertW, drawY + alertH);

        // Background — glassmorphic rounded rectangle
        ImU32 bgColor = m_isDarkMode
            ? IM_COL32(35, 35, 40, alpha)
            : IM_COL32(245, 248, 252, alpha);
        ImU32 borderColor = m_isDarkMode
            ? IM_COL32(70, 70, 80, (int)(180 * slideProgress))
            : IM_COL32(200, 205, 215, (int)(200 * slideProgress));

        dl->AddRectFilled(rectMin, rectMax, bgColor, cornerRadius);
        dl->AddRect(rectMin, rectMax, borderColor, cornerRadius, 0, 1.5f * scale);

        // Badge image (left side)
        float textX = rectMin.x + padding;
        if (n.textureId != 0) {
            float badgeX = rectMin.x + badgeMargin;
            float badgeY = rectMin.y + (alertH - badgeSize) * 0.5f;

            float drawBadgeSize = badgeSize;
            float drawBadgeX = badgeX;
            float drawBadgeY = badgeY;

            // Make the general RA icon a bit smaller to fit visually better
            if (n.badge_name == "ra_icon") {
                drawBadgeSize = badgeSize * 0.70f;
                drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
                drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
            }

            ImVec2 bMin(drawBadgeX, drawBadgeY);
            ImVec2 bMax(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize);
            ImU32 imgCol = IM_COL32(255, 255, 255, alpha);
            dl->AddImageRounded(n.textureId,
                bMin, bMax, ImVec2(0,0), ImVec2(1,1), imgCol, badgeRadius);
            
            textX = badgeX + badgeSize + badgeMargin;
        }

        // Text colors
        ImU32 descColor = m_isDarkMode
            ? IM_COL32(185, 185, 195, alpha)
            : IM_COL32(80, 80, 95, alpha);
        float maxDescW = rectMax.x - textX - padding;

        ImU32 titleColor = m_isDarkMode
            ? IM_COL32(255, 255, 255, alpha)
            : IM_COL32(30, 30, 40, alpha);

        std::string desc = n.description;
        float maxDescH = descFontSize * 2.5f;
        ImVec2 fullSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        // If content goes through 2 lines, slice and add '...'
        if (fullSize.y > maxDescH) {
            desc += "...";
            while (desc.length() > 4) {
                ImVec2 testSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
                if (testSize.y <= maxDescH) break;
                desc.erase(desc.length() - 4, 1);
            }
        }

        std::string titleStr = n.title;
        ImVec2 titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        if (titleSize.x > maxDescW) {
            titleStr += "...";
            while (titleStr.length() > 4) {
                ImVec2 testSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
                if (testSize.x <= maxDescW) break;
                titleStr.erase(titleStr.length() - 4, 1);
            }
            titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        }

        ImVec2 descSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        float textSpacing = 4.0f * scale;
        float totalTextH = titleSize.y + textSpacing + descSize.y;
        float titleY = rectMin.y + (alertH - totalTextH) * 0.5f;
        float descY = titleY + titleSize.y + textSpacing;

        // Title shadow + text
        dl->AddText(font, titleFontSize,
            ImVec2(textX + 1.0f, titleY + 1.0f),
            IM_COL32(0, 0, 0, (int)(80 * slideProgress)),
            titleStr.c_str());
        dl->AddText(font, titleFontSize,
            ImVec2(textX, titleY), titleColor, titleStr.c_str());

        // Description
        dl->AddText(descFont, descFontSize, ImVec2(textX, descY), descColor, desc.c_str(), nullptr, maxDescW);
    }
}
