#pragma once

#include <string>
#include <vector>

namespace archive_catalog {

inline constexpr long long final_version_rank = 1LL << 40;

// "0.7.0-beta.1" < "0.7.0"; unknown text compares as the oldest version.
inline std::vector<long long> version_parts(const std::string& text) {
    std::vector<long long> parts;
    std::size_t position = 0;
    for (int index = 0; index < 3; ++index) {
        std::size_t used = 0;
        try { parts.push_back(std::stoll(text.substr(position), &used)); } catch (...) { return {-1}; }
        position += used;
        if (index < 2) { if (position >= text.size() || text[position] != '.') return {-1}; ++position; }
    }
    const std::string beta = "-beta.";
    if (position == text.size()) parts.push_back(final_version_rank);
    else if (text.compare(position, beta.size(), beta) == 0) {
        try { parts.push_back(std::stoll(text.substr(position + beta.size()))); } catch (...) { return {-1}; }
    } else return {-1};
    return parts;
}

inline int compare_versions(const std::string& left, const std::string& right) {
    const auto a = version_parts(left), b = version_parts(right);
    return a < b ? -1 : b < a ? 1 : 0;
}

inline bool canonical_version(const std::string& text) {
    if (text.size() > 80) return false;
    const auto parts = version_parts(text);
    if (parts.size() != 4 || parts[0] < 0 || parts[1] < 0 || parts[2] < 0 ||
        parts[3] <= 0 || parts[3] > final_version_rank) return false;
    auto canonical = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]);
    if (parts[3] != final_version_rank) canonical += "-beta." + std::to_string(parts[3]);
    return canonical == text;
}

}
