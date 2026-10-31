// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wolf_shader_compat_detail {

inline constexpr std::string_view kFragmentHeader =
    "#define uhi uniform highp\n"
    "#define umed uniform mediump\n"
    "#define ulow uniform lowp\n"
    "#define vhi varying highp\n"
    "#define vmed varying mediump\n"
    "#define vlow varying lowp\n"
    "precision lowp float;\n";

inline constexpr std::string_view kTintFragmentHeader =
    "#define uhi uniform highp\n"
    "#define umed uniform mediump\n"
    "#define ulow uniform mediump\n"
    "#define vhi varying highp\n"
    "#define vmed varying mediump\n"
    "#define vlow varying mediump\n"
    "precision mediump float;\n";

inline constexpr std::string_view kDerivativeExtension =
    "#extension GL_OES_standard_derivatives : enable\n";

inline std::size_t Count(std::string_view text, std::string_view word) {
    std::size_t count = 0;
    for (std::size_t pos = 0; (pos = text.find(word, pos)) != text.npos;
         pos += word.size()) {
        ++count;
    }
    return count;
}

inline bool Hex8(std::string_view text, std::uint32_t& value) {
    if (text.size() != 8) return false;
    value = 0;
    for (const char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}

inline bool Comment(std::string_view text, std::string_view& effect,
                    std::uint32_t& features, std::uint32_t& variance) {
    constexpr std::string_view separator = ", features: ";
    constexpr std::string_view variance_separator = ", variance: ";
    if (text.substr(0, 2) != "//") return false;
    const auto end = text.find(separator, 2);
    if (end == text.npos) return false;
    effect = text.substr(2, end - 2);
    text.remove_prefix(end + separator.size());
    if (text.size() != 8 + variance_separator.size() + 8 + 1 ||
        text.substr(8, variance_separator.size()) != variance_separator ||
        text.back() != '\n') return false;
    return Hex8(text.substr(0, 8), features) &&
           Hex8(text.substr(8 + variance_separator.size(), 8), variance);
}

inline bool NumberedName(std::string_view name, std::string_view prefix) {
    if (name.substr(0, prefix.size()) != prefix || name.size() == prefix.size())
        return false;
    for (const char c : name.substr(prefix.size())) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Match each declaration exactly once in either original or repaired form.
// Fail closed on duplicates or partial identifiers. Mutation is local to the
// returned source; a rejected source never changes the caller's strings.
inline bool Promote(std::string_view original, std::string& edited,
                    std::string_view before, std::string_view after,
                    bool& changed) {
    const auto old_count = Count(original, before);
    const auto new_count = Count(original, after);
    if (old_count == 0 && new_count == 1) return true;
    if (old_count != 1 || new_count != 0) return false;
    if (edited.empty()) edited.assign(original);
    edited.replace(edited.find(before), before.size(), after);
    changed = true;
    return true;
}

inline bool SmoothDetail(std::string_view body, std::string& edited,
                         bool& changed) {
    if (body.find('#') != body.npos) return false;
    constexpr std::string_view declaration = "  highp float detailAlpha_";
    const auto pos = body.find(declaration);
    if (pos == body.npos || Count(body, declaration) != 1) return false;
    const auto name_start = pos + std::string_view("  highp float ").size();
    const auto name_end = body.find(';', name_start);
    if (name_end == body.npos) return false;
    const auto name = body.substr(name_start, name_end - name_start);
    if (!NumberedName(name, "detailAlpha_")) return false;

    const std::string assignment = "\n  " + std::string(name) + " = ";
    const auto assignment_pos = body.find(assignment);
    if (assignment_pos == body.npos || Count(body, assignment) != 1) return false;
    const auto temp_start = assignment_pos + assignment.size();
    const auto temp_end = body.find(';', temp_start);
    if (temp_end == body.npos) return false;
    const auto temp = body.substr(temp_start, temp_end - temp_start);
    if (!NumberedName(temp, "t")) return false;

    const std::string lookup = "\n  " + std::string(temp) +
                               " = texture2D (F47, ";
    const auto lookup_pos = body.find(lookup);
    if (lookup_pos == body.npos || Count(body, lookup) != 1 ||
        Count(body, "texture2D (F47,") != 1) return false;
    const auto lookup_end = body.find('\n', lookup_pos + 1);
    if (lookup_end == body.npos || lookup_end < 4 ||
        body.substr(lookup_end - 4, 4) != ").w;") return false;
    if (Count(body, std::string(name) + " * T4.z") != 1 ||
        Count(body, "+ T4.w)") != 1) return false;

    return Promote(body, edited, "uniform sampler2D F47;",
                   "uniform highp sampler2D F47;", changed) &&
           Promote(body, edited, "  float " + std::string(temp) + ";",
                   "  highp float " + std::string(temp) + ";", changed);
}

inline bool MissingNormal(std::string_view body, std::string& edited,
                          bool& changed) {
    // Variants with neither diffuse nor specular light optimize F43 away.
    if (body.find("F43") == body.npos) return true;
    constexpr std::string_view lookup = " = texture2D (F43, T1);";
    const auto at = body.find(lookup);
    if (at == body.npos || Count(body, lookup) != 1 ||
        Count(body, "texture2D (F43,") != 1 ||
        Count(body, "uniform sampler2D F43;") != 1) return false;
    const auto line = body.rfind('\n', at);
    if (line == body.npos || body.substr(line + 1, 2) != "  ") return false;
    const auto sample = body.substr(line + 3, at - line - 3);
    if (!NumberedName(sample, "t") ||
        Count(body, "  vec4 " + std::string(sample) + ";") != 1) return false;

    const std::string alpha = ".w = " + std::string(sample) + ".x;";
    const auto alpha_at = body.find(alpha);
    if (alpha_at == body.npos || Count(body, alpha) != 1) return false;
    const auto alpha_line = body.rfind('\n', alpha_at);
    if (alpha_line == body.npos || body.substr(alpha_line + 1, 2) != "  ") return false;
    const auto normal = body.substr(alpha_line + 3, alpha_at - alpha_line - 3);
    if (!NumberedName(normal, "normalTexture_") ||
        Count(body, "  vec4 " + std::string(normal) + ";") != 1) return false;
    const std::string decoded = "((" + std::string(sample) + ".wzy * 2.0) - 1.0)";
    const std::string prefix = "  " + std::string(normal) + ".xyz = ";
    const std::string original = prefix + decoded + ";";
    const std::string repaired = prefix + "all(equal(" + std::string(sample) +
        ", vec4(0.0))) ? vec3(0.0, 0.0, 1.0) : " + decoded + ";";
    // shortcut: RGBA0 is the native missing-map sentinel; use an explicit flag if non-unit/padded normals are introduced.
    return Promote(body, edited, original, repaired, changed);
}

}  // namespace wolf_shader_compat_detail

// Original Engine supplies comment, precision header and Cg-generated body as
// three strings. Empty means forward those original strings unchanged. The
// caller owns the returned string until glShaderSource has copied its bytes.
inline std::string PrepareWolfShaderSource(std::string_view comment,
                                          std::string_view header,
                                          std::string_view body) {
    using namespace wolf_shader_compat_detail;
    std::string_view effect;
    std::uint32_t features = 0, variance = 0;
    if (!Comment(comment, effect, features, variance)) return {};
    const auto expected_header = (features & 0x40000u) ? kTintFragmentHeader
                                                     : kFragmentHeader;
    if (header != expected_header) return {};

    std::string edited;
    bool changed = false;
    std::size_t extension_size = 0;
    if (effect == "Mesh" &&
        ((features & 0x80080u) == 0x80080u || (features & 4u))) {
        // All original smooth-detail bodies start with varying declarations.
        if (body.substr(0, 5) != "vlow " && body.substr(0, 4) != "vhi " &&
            body.substr(0, 5) != "vmed ") return {};
        if ((features & 0x80080u) == 0x80080u &&
            !SmoothDetail(body, edited, changed)) return {};
        if ((features & 4u) && !MissingNormal(body, edited, changed)) return {};
    } else if ((effect == "DirectionalLightShadow" ||
                effect == "CascadeLightShadow" ||
                effect == "LastCascadeLightShadow") &&
               features == 0 && (variance & ~0x1400u) == 0) {
        if (body.substr(0, kDerivativeExtension.size()) != kDerivativeExtension ||
            Count(body, "#extension") != 1 ||
            Count(body, "  highp float projectedDepth_2;") != 1 ||
            Count(body, "  t3 = texture2D (F55, T0).x;") != 1) return {};
        const bool projected = (variance & 0x400u) != 0;
        const auto lookup = projected ? "  t10 = texture2DProj (F5D, t9);"
                                      : "  t10 = texture2D (F5D, t4.xy).x;";
        if (Count(body, lookup) != 1 ||
            !Promote(body, edited, "uniform sampler2D F55;",
                     "uniform highp sampler2D F55;", changed) ||
            !Promote(body, edited, "uniform sampler2D F5D;",
                     "uniform highp sampler2D F5D;", changed) ||
            !Promote(body, edited, "  float t3;", "  highp float t3;", changed) ||
            !Promote(body, edited, projected ? "  vec4 t10;" : "  float t10;",
                     projected ? "  highp vec4 t10;" : "  highp float t10;",
                     changed)) return {};
        extension_size = kDerivativeExtension.size();
    } else if (effect == "FX_CreateLinearDepth" && features == 0 && variance == 0) {
        if (body.substr(0, 13) != "vhi vec2 T0;\n" || body.find('#') != body.npos ||
            Count(body, "  highp float projectedDepth_1;") != 1 ||
            Count(body, "  t2 = texture2D (F55, T0).x;") != 1 ||
            !Promote(body, edited, "uniform sampler2D F55;",
                     "uniform highp sampler2D F55;", changed) ||
            !Promote(body, edited, "  float t2;", "  highp float t2;",
                     changed)) return {};
    } else {
        return {};
    }
    if (!changed && extension_size == 0) return {};
    const std::string_view source = edited.empty() ? body : std::string_view(edited);
    std::string merged;
    merged.reserve(comment.size() + header.size() + source.size());
    merged.append(comment);
    merged.append(source.substr(0, extension_size));
    merged.append(header);
    merged.append(source.substr(extension_size));
    return merged;
}
