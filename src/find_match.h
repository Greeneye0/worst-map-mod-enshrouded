#pragma once
// Copied from FindMod/src/match.h (2026-11-12): keep the two in step.
// How typed text matches a name (and a skill's description). Everything is already lower case.
// Header only, so tools/tests.cpp checks it offline.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace match {

// 0 exact, 1 the name starts with it, 2 a word in the name starts with it, 3 anywhere in the
// name; -1 no match.
inline int Rank(const std::string& name, const std::string& q)
{
    if (q.empty())
        return -1;
    if (name == q)
        return 0;
    size_t at = name.find(q);
    if (at == std::string::npos)
        return -1;
    if (at == 0)
        return 1;
    for (; at != std::string::npos; at = name.find(q, at + 1)) {
        char before = name[at - 1];
        if (!isalnum(static_cast<unsigned char>(before)))
            return 2;
    }
    return 3;
}

// The words of the text, each once.
inline std::vector<std::string> Words(const std::string& q)
{
    std::vector<std::string> words;
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && isspace(static_cast<unsigned char>(q[i])))
            ++i;
        size_t start = i;
        while (i < q.size() && !isspace(static_cast<unsigned char>(q[i])))
            ++i;
        if (i > start) {
            std::string w = q.substr(start, i - start);
            if (std::find(words.begin(), words.end(), w) == words.end())
                words.push_back(w);
        }
    }
    return words;
}

// Ranks below Rank's (and below 4, a skill whose description has the whole text):
constexpr int kAllWordStarts = 5; // every word starts a word of the name ("ring mana": Ring of Mana)
constexpr int kAllInName = 6;     // every word is somewhere in the name
constexpr int kAllInText = 7;     // every word is in the name or the description (skills)
constexpr int kSome = 10;         // kSome + missing words: some words start a word of the name or description

inline bool IsAll(int r) { return r >= kAllWordStarts && r <= kAllInText; }

// Text of two or more words that doesn't match as a whole: first names with all the words (in
// any order), then names with some of them, more of them first. Words under 3 bytes ("of", "a")
// count for all-words but never make a match on their own. -1 no match.
inline int WordsRank(const std::string& name, const std::string& desc, const std::vector<std::string>& words)
{
    if (words.size() < 2)
        return -1;
    bool starts = true, inName = true, inText = true;
    size_t some = 0, counted = 0;
    for (const std::string& w : words) {
        int r = Rank(name, w);
        bool start = r >= 0 && r <= 2;
        starts = starts && start;
        inName = inName && r >= 0;
        bool inDesc = !desc.empty() && desc.find(w) != std::string::npos;
        inText = inText && (r >= 0 || inDesc);
        if (w.size() >= 3) {
            ++counted;
            int d = desc.empty() ? -1 : Rank(desc, w);
            some += start || (d >= 0 && d <= 2) ? 1 : 0;
        }
    }
    if (starts)
        return kAllWordStarts;
    if (inName)
        return kAllInName;
    if (inText)
        return kAllInText;
    return some ? kSome + static_cast<int>(counted - some) : -1;
}

} // namespace match
