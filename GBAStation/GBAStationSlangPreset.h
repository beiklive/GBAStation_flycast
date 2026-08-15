/// @file GBAStationSlangPreset.h
/// @brief Small, deterministic parser for RetroArch SlangP presets.
///
/// The parser deliberately keeps the data model independent of Vulkan.  The
/// renderer consumes the resolved pass order while the overlay consumes the
/// same parameter metadata, so a user can never edit a value the pipeline did
/// not parse.
#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace GBAStationSlang
{
enum class ScaleType { Source, Viewport, Absolute };

struct Parameter {
    std::string id;
    // The identifier used in a pass' push-constant block.  Most presets use
    // the same name as id, but a few published presets use a friendly preset
    // id (for example MASK_SIZE) and a different shader member (ss).
    std::string runtimeId;
    std::string label;
    float initial = 0.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
    float step = 0.0f;
    float value = 0.0f;
    // Headers and malformed/obsolete preset entries are deliberately kept out
    // of the editable list.  Keeping this bit lets the UI explain why instead
    // of silently exposing a slider that can never affect the image.
    bool editable = true;
};

struct PushConstantMember {
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct Sampler {
    std::string name;
    uint32_t binding = 0;
};

struct Pass {
    std::string path;
    std::string alias;
    ScaleType scaleX = ScaleType::Source;
    ScaleType scaleY = ScaleType::Source;
    float scaleXValue = 1.0f;
    float scaleYValue = 1.0f;
    bool linear = false;
    bool mipmapInput = false;
    bool srgbFramebuffer = false;
    bool floatFramebuffer = false;
    // Source files in this project use both stages in one .slang file.
    std::string vertexSource;
    std::string fragmentSource;
    std::vector<uint32_t> vertexSpirv;
    std::vector<uint32_t> fragmentSpirv;
    std::vector<PushConstantMember> pushConstants;
    // std140 UBO reflection is kept separately from push constants. Published
    // Slang shaders do not all put SourceSize/OutputSize at the same offset.
    std::vector<PushConstantMember> uniformMembers;
    std::vector<Sampler> samplers;
};

struct Preset {
    std::string path;
    std::vector<Pass> passes;
    // Parameters listed by the preset are the user-facing controls.  They do
    // not necessarily enumerate every #pragma parameter used by its passes:
    // authors commonly omit fixed controls while relying on their declared
    // defaults.  Keep the complete runtime set separately so omitted defaults
    // are still uploaded to Vulkan instead of silently becoming zero.
    std::vector<Parameter> runtimeParameters;
    std::vector<Parameter> parameters;
    // Non-fatal preset quality issues. They are logged when selected and are
    // particularly useful for hand-edited .slangp files.
    std::vector<std::string> warnings;
};

/// Parses a .slangp and all directly referenced .slang files. It validates
/// every pass has a vertex and fragment stage, resolves relative paths, and
/// applies the preset's per-parameter overrides to Parameter::value. Every
/// pass is then compiled to SPIR-V with the same Vulkan GLSL rules used by the
/// renderer; this makes a selected preset safe to hand to the Vulkan chain.
bool Load(const std::string &path, Preset &out, std::string &error);
}
