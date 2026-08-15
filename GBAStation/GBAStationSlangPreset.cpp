#include "GBAStationSlangPreset.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <set>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace GBAStationSlang
{
namespace
{
std::string Trim(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string Directory(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string ReadFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string JoinPath(const std::string &directory, const std::string &path)
{
    if (path.empty() || (path.size() > 1 && path[1] == ':') || path[0] == '/' || path[0] == '\\')
        return path;
    return directory + path;
}

bool ExpandIncludes(const std::string &path, std::set<std::string> &active,
                    std::string &out, std::string &error)
{
    if (!active.insert(path).second)
    {
        error = "cyclic #include: " + path;
        return false;
    }
    const std::string text = ReadFile(path);
    if (text.empty())
    {
        active.erase(path);
        error = "cannot read shader source: " + path;
        return false;
    }
    static const std::regex include(R"(^\s*#\s*include\s+\"([^\"]+)\"\s*$)");
    std::istringstream lines(text);
    std::string line;
    std::smatch match;
    while (std::getline(lines, line))
    {
        if (std::regex_match(line, match, include))
        {
            if (!ExpandIncludes(JoinPath(Directory(path), match[1].str()), active, out, error))
            {
                active.erase(path);
                return false;
            }
        }
        else
            out += line + '\n';
    }
    active.erase(path);
    return true;
}

std::map<std::string, std::string> ParseAssignments(const std::string &text)
{
    std::map<std::string, std::string> values;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equals));
        std::string value = Trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (!key.empty()) values[key] = value;
    }
    return values;
}

float Number(const std::map<std::string, std::string> &values, const std::string &key, float fallback)
{
    const auto it = values.find(key);
    return it == values.end() ? fallback : std::strtof(it->second.c_str(), nullptr);
}

bool Bool(const std::map<std::string, std::string> &values, const std::string &key, bool fallback)
{
    const auto it = values.find(key);
    if (it == values.end()) return fallback;
    return it->second == "true" || it->second == "1";
}

ScaleType ParseScale(const std::string &value)
{
    if (value == "viewport") return ScaleType::Viewport;
    if (value == "absolute") return ScaleType::Absolute;
    return ScaleType::Source;
}

bool SplitStages(const std::string &source, std::string &vertex, std::string &fragment)
{
    std::istringstream lines(source);
    std::string common, vertexBody, fragmentBody, line;
    enum class Stage { Common, Vertex, Fragment } stage = Stage::Common;
    while (std::getline(lines, line))
    {
        const std::string trimmed = Trim(line);
        if (trimmed == "#pragma stage vertex") { stage = Stage::Vertex; continue; }
        if (trimmed == "#pragma stage fragment") { stage = Stage::Fragment; continue; }
        if (trimmed.rfind("#pragma parameter", 0) == 0 || trimmed.rfind("#pragma name", 0) == 0)
            continue;
        if (stage == Stage::Common) common += line + '\n';
        else if (stage == Stage::Vertex) vertexBody += line + '\n';
        else fragmentBody += line + '\n';
    }
    vertex = common + vertexBody;
    fragment = common + fragmentBody;
    return !vertexBody.empty() && !fragmentBody.empty();
}

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void TypeLayout(const std::string &type, uint32_t &alignment, uint32_t &size)
{
    alignment = 4;
    size = 4;
    if (type == "vec2" || type == "ivec2" || type == "uvec2") { alignment = 8; size = 8; }
    else if (type == "vec3" || type == "ivec3" || type == "uvec3") { alignment = 16; size = 12; }
    else if (type == "vec4" || type == "ivec4" || type == "uvec4") { alignment = 16; size = 16; }
    else if (type == "mat2") { alignment = 8; size = 16; }
    else if (type == "mat3") { alignment = 16; size = 48; }
    else if (type == "mat4") { alignment = 16; size = 64; }
}

void ReflectPass(Pass &pass)
{
    static const std::regex pushBlock(R"(layout\s*\(\s*push_constant\s*\)\s*uniform\s+\w+\s*\{([\s\S]*?)\}\s*\w*\s*;)");
    static const std::regex member(R"(\b(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;)");
    static const std::regex sampler(R"(layout\s*\([^)]*binding\s*=\s*(\d+)[^)]*\)\s*uniform\s+sampler\w*\s+(\w+))");
    std::smatch block;
    if (std::regex_search(pass.vertexSource, block, pushBlock))
    {
        uint32_t offset = 0;
        std::string declarations = block[1].str();
        for (std::sregex_iterator it(declarations.begin(), declarations.end(), member), end; it != end; ++it)
        {
            uint32_t alignment = 4, size = 4;
            TypeLayout((*it)[1].str(), alignment, size);
            const uint32_t count = (*it)[3].matched ? static_cast<uint32_t>(std::strtoul((*it)[3].str().c_str(), nullptr, 10)) : 1;
            if (count > 1) { alignment = std::max(16u, alignment); size = AlignUp(size, alignment) * count; }
            offset = AlignUp(offset, alignment);
            pass.pushConstants.push_back({(*it)[2].str(), offset, size});
            offset += size;
        }
    }
    std::set<std::string> seen;
    for (std::sregex_iterator it(pass.fragmentSource.begin(), pass.fragmentSource.end(), sampler), end; it != end; ++it)
        if (seen.insert((*it)[2].str()).second)
            pass.samplers.push_back({(*it)[2].str(), static_cast<uint32_t>(std::strtoul((*it)[1].str().c_str(), nullptr, 10))});
}

EShLanguage LanguageForStage(bool vertex)
{
    return vertex ? EShLangVertex : EShLangFragment;
}

bool CompileStage(const std::string &source, bool vertex, std::vector<uint32_t> &spirv, std::string &error)
{
    const char *text = source.c_str();
    glslang::TShader shader(LanguageForStage(vertex));
    shader.setStrings(&text, 1);
    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 100, false, messages))
    {
        error = shader.getInfoLog();
        if (error.empty()) error = shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
    {
        error = program.getInfoLog();
        if (error.empty()) error = program.getInfoDebugLog();
        return false;
    }
    glslang::GlslangToSpv(*program.getIntermediate(LanguageForStage(vertex)), spirv);
    return !spirv.empty();
}

bool CompilePass(Pass &pass, std::string &error)
{
    static bool initialized = false;
    if (!initialized)
        initialized = glslang::InitializeProcess();
    if (!initialized)
    {
        error = "glslang initialization failed";
        return false;
    }
    if (!CompileStage(pass.vertexSource, true, pass.vertexSpirv, error))
    {
        error = "vertex compile failed in " + pass.path + ": " + error;
        return false;
    }
    if (!CompileStage(pass.fragmentSource, false, pass.fragmentSpirv, error))
    {
        error = "fragment compile failed in " + pass.path + ": " + error;
        return false;
    }
    return true;
}
}

bool Load(const std::string &path, Preset &out, std::string &error)
{
    out = {};
    error.clear();
    const std::string presetText = ReadFile(path);
    if (presetText.empty()) { error = "cannot read preset: " + path; return false; }
    const auto values = ParseAssignments(presetText);
    const int passCount = static_cast<int>(Number(values, "shaders", 0));
    if (passCount <= 0) { error = "preset has no shader passes"; return false; }
    out.path = path;
    const std::string base = Directory(path);
    std::map<std::string, Parameter> definitions;
    const std::regex parameter(R"(^\s*#pragma\s+parameter\s+([^\s]+)\s+\"([^\"]*)\"\s+([^\s]+)\s+([^\s]+)\s+([^\s]+)\s+([^\s]+))");
    for (int i = 0; i < passCount; ++i)
    {
        const auto shader = values.find("shader" + std::to_string(i));
        if (shader == values.end()) { error = "missing shader" + std::to_string(i); return false; }
        Pass pass;
        pass.path = JoinPath(base, shader->second);
        pass.alias = values.count("alias" + std::to_string(i)) ? values.at("alias" + std::to_string(i)) : "";
        pass.scaleX = ParseScale(values.count("scale_type_x" + std::to_string(i)) ? values.at("scale_type_x" + std::to_string(i)) :
                                 (values.count("scale_type" + std::to_string(i)) ? values.at("scale_type" + std::to_string(i)) : "source"));
        pass.scaleY = ParseScale(values.count("scale_type_y" + std::to_string(i)) ? values.at("scale_type_y" + std::to_string(i)) :
                                 (values.count("scale_type" + std::to_string(i)) ? values.at("scale_type" + std::to_string(i)) : "source"));
        pass.scaleXValue = Number(values, "scale_x" + std::to_string(i), Number(values, "scale" + std::to_string(i), 1.0f));
        pass.scaleYValue = Number(values, "scale_y" + std::to_string(i), Number(values, "scale" + std::to_string(i), 1.0f));
        pass.linear = Bool(values, "filter_linear" + std::to_string(i), false);
        pass.mipmapInput = Bool(values, "mipmap_input" + std::to_string(i), false);
        pass.srgbFramebuffer = Bool(values, "srgb_framebuffer" + std::to_string(i), false);
        pass.floatFramebuffer = Bool(values, "float_framebuffer" + std::to_string(i), false);
        std::string source;
        std::set<std::string> activeIncludes;
        if (!ExpandIncludes(pass.path, activeIncludes, source, error) ||
            !SplitStages(source, pass.vertexSource, pass.fragmentSource)) {
            if (error.empty()) error = "invalid Slang stages: " + pass.path;
            return false;
        }
        std::istringstream lines(source);
        std::string line;
        std::smatch match;
        while (std::getline(lines, line))
            if (std::regex_search(line, match, parameter))
                definitions.emplace(match[1], Parameter{match[1], match[1], match[2], std::strtof(match[3].str().c_str(), nullptr), std::strtof(match[4].str().c_str(), nullptr), std::strtof(match[5].str().c_str(), nullptr), std::strtof(match[6].str().c_str(), nullptr), std::strtof(match[3].str().c_str(), nullptr), true});
        ReflectPass(pass);
        if (!CompilePass(pass, error))
            return false;
        out.passes.push_back(std::move(pass));
    }
    const auto parameterList = values.find("parameters");
    if (parameterList != values.end()) {
        std::istringstream names(parameterList->second);
        std::string id;
        while (std::getline(names, id, ';')) {
            id = Trim(id);
            if (id.empty()) continue;
            // F10_PhosphorLineReflex.slangp intentionally repeats several
            // ids in its parameters list. A parameter is a shared push
            // constant across passes, so expose it exactly once in the UI.
            const auto alreadyAdded = std::find_if(out.parameters.begin(), out.parameters.end(),
                [&id](const Parameter &parameter) { return parameter.id == id; });
            if (alreadyAdded != out.parameters.end()) continue;
            auto definition = definitions.find(id);
            // Some released presets use a public preset id that differs from
            // the actual #pragma/member identifier. Resolve by the stable
            // human label, rather than assuming a particular author's alias.
            if (definition == definitions.end())
            {
                static const std::map<std::string, std::string> knownLabels = {
                    {"MASK_SIZE", "Shadow Mask Size"}, {"LINEAR_X", "Horizontal Blending"}
                };
                const auto label = knownLabels.find(id);
                if (label != knownLabels.end())
                    definition = std::find_if(definitions.begin(), definitions.end(),
                        [&label](const auto &entry) { return entry.second.label == label->second; });
            }
            if (definition == definitions.end())
            {
                out.warnings.push_back("preset parameter '" + id + "' is not declared by an active pass");
                continue;
            }
            Parameter parameter = definition->second;
            parameter.id = id;
            parameter.value = Number(values, id, parameter.initial);
            parameter.editable = parameter.step > 0.0f && parameter.maximum > parameter.minimum;
            out.parameters.push_back(std::move(parameter));
        }
    }
    return true;
}
}
