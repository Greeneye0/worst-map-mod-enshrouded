#include "../src/filter_notice.h"
#include <cstdio>

static int g_failed = 0;

static void Check(const std::vector<std::pair<std::string, bool>>& changes, const char* want) {
    const std::string got = filter_notice::Format(changes, "ON", "OFF");
    if (got == want) return;
    std::printf("FAIL: got \"%s\", wanted \"%s\"\n", got.c_str(), want);
    ++g_failed;
}

int main() {
    Check({}, "");
    Check({{"Wood Chest", false}}, "Wood Chest: OFF");
    Check({{"Wood Chest", true}}, "Wood Chest: ON");
    Check({{"Gold", true}, {"Silver", true}, {"Wood", true}}, "Gold, Silver, Wood: ON");
    // More than the line names: the rest are counted
    Check({{"A", false}, {"B", false}, {"C", false}, {"D", false}, {"E", false}}, "A, B, C +2: OFF");
    // Both ways in one step: each state keeps its own names, whatever the row order
    Check({{"A", false}, {"B", true}, {"C", false}}, "B: ON   A, C: OFF");
    // The Settings line's words, and a Ctrl + click's line
    const std::string line = "Looted / Read / Beaten";
    if (filter_notice::WordOf(line, 0) != "Looted" || filter_notice::WordOf(line, 1) != "Read" ||
        filter_notice::WordOf(line, 2) != "Beaten" || filter_notice::WordOf("One", 2) != "One") {
        std::printf("FAIL: WordOf\n");
        ++g_failed;
    }
    if (filter_notice::Mark("Silver Chest", "Looted", "ON") != "Silver Chest - Looted: ON" ||
        filter_notice::Mark("Silver Chest", "Refilled") != "Silver Chest - Refilled") {
        std::printf("FAIL: Mark\n");
        ++g_failed;
    }
    if (g_failed) return 1;
    std::printf("Filter notice tests passed\n");
    return 0;
}
