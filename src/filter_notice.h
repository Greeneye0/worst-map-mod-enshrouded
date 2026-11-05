#pragma once
// The line the World Map shows when a hide click or an undo / redo changes filters, such as "Wood Chest: OFF".
// No game code here (tests/test_filter_notice.cpp).
#include <string>
#include <utility>
#include <vector>

namespace filter_notice {

constexpr size_t kMaxNames = 3; // a longer change (undoing Show All) names these and counts the rest

// One side's names: "A, B, C +9: OFF"
inline void AddSide(std::string& out, const std::vector<std::string>& names, const char* word) {
    if (names.empty()) return;
    if (!out.empty()) out += "   ";
    for (size_t i = 0; i < names.size() && i < kMaxNames; ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    if (names.size() > kMaxNames) out += " +" + std::to_string(names.size() - kMaxNames);
    out += ": ";
    out += word;
}

// changes: each changed row's name and its new state, in row order. Empty when nothing changed.
inline std::string Format(const std::vector<std::pair<std::string, bool>>& changes, const char* onWord,
                          const char* offWord) {
    std::vector<std::string> on, off;
    for (const auto& [name, enabled] : changes) (enabled ? on : off).push_back(name);
    std::string out;
    AddSide(out, on, onWord);
    AddSide(out, off, offWord);
    return out;
}

// The word at index of a line of words split by " / " (the Settings line "Looted / Read / Beaten", so a message
// uses the same words). The whole line when it has no such word.
inline std::string WordOf(const std::string& line, size_t index) {
    size_t from = 0;
    for (size_t i = 0; i < index; ++i) {
        const size_t cut = line.find(" / ", from);
        if (cut == std::string::npos) return line;
        from = cut + 3;
    }
    return line.substr(from, line.find(" / ", from) - from);
}

// A Ctrl + click's line: "Silver Chest - Looted: ON", or with no word for its state "Silver Chest - Refilled".
inline std::string Mark(const std::string& name, const std::string& state, const char* stateWord = nullptr) {
    std::string out = name + " - " + state;
    if (stateWord) {
        out += ": ";
        out += stateWord;
    }
    return out;
}

} // namespace filter_notice
