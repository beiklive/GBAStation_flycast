#pragma once

#include <string>
#include <unordered_map>

class GBAStationTranslationManager {
public:
    static GBAStationTranslationManager& Instance();

    bool Init();
    std::string GetString(const std::string& key) const;

private:
    GBAStationTranslationManager() = default;
    
    std::string m_currentLanguage;
    std::unordered_map<std::string, std::string> m_translations;
};

// Global helper
std::string tr(const std::string& key);
