#pragma once
// The game's own controls, shared by the mods that follow them (Town Portals, the map mod).
// The keys the game uses for its interact action (GameplayAction ContextualAction, the "E" of
// every interaction prompt), so a portal answers to the key the player chose in the game's
// controls. The game keeps only changed bindings, in Steam's cloud folder:
// Steam\userdata\<account>\1203620\remote\user_settings.json, under
// input.keyboardMouse.bindings.gameplay, as entries
//   { "id": "ContextualAction", "action": { "primary": { "operation": { "type": "Key", "id": "F" }, ... },
//                                           "secondary": { ... }, ... } }
// (keen::input_mapping::serialize::ActionInputBinding; key names as in the exe's key table:
// A-Z, Number0-9, F1-F24, Space, Return, KeyPad_0-9, ...; mouse buttons are type MouseButton).
// No entry: the default, E.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#pragma comment(lib, "advapi32.lib") // RegGetValueW

namespace greeneye::bindings {

// A small JSON reader: enough for the settings file (objects, arrays, strings, plain values).
struct Json {
    enum Kind { null, value, string, array, object } kind = null;
    std::string text; // string or plain value
    std::vector<std::pair<std::string, Json>> members; // object members, or array items (empty keys)
    const Json* Get(std::string_view key) const {
        if (kind != object) return nullptr;
        for (const auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}
    bool Parse(Json& out) { return Value(out, 0); }
private:
    std::string_view s_;
    size_t i_ = 0;
    void Space() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_; }
    bool String(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        for (++i_; i_ < s_.size(); ++i_) {
            const char c = s_[i_];
            if (c == '"') { ++i_; return true; }
            if (c == '\\' && i_ + 1 < s_.size()) {
                const char e = s_[++i_];
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'u' ? '?' : e;
                if (e == 'u') i_ += 4;
            } else out += c;
        }
        return false;
    }
    bool Value(Json& out, int depth) {
        if (depth > 64) return false;
        Space();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '"') { out.kind = Json::string; return String(out.text); }
        if (c == '{' || c == '[') {
            const bool isObject = c == '{';
            out.kind = isObject ? Json::object : Json::array;
            ++i_;
            Space();
            if (i_ < s_.size() && s_[i_] == (isObject ? '}' : ']')) { ++i_; return true; }
            for (;;) {
                std::pair<std::string, Json> member;
                Space();
                if (isObject) {
                    if (!String(member.first)) return false;
                    Space();
                    if (i_ >= s_.size() || s_[i_] != ':') return false;
                    ++i_;
                }
                if (!Value(member.second, depth + 1)) return false;
                out.members.push_back(std::move(member));
                Space();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == (isObject ? '}' : ']')) { ++i_; return true; }
                return false;
            }
        }
        out.kind = Json::value;
        while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' && s_[i_] != ' ' && s_[i_] != '\r' && s_[i_] != '\n' && s_[i_] != '\t')
            out.text += s_[i_++];
        return !out.text.empty();
    }
};

// A key or mouse button name of the game's to a Windows virtual-key code; 0 when unknown.
inline int GameKeyToVk(std::string_view type, std::string_view id) {
    auto is = [&](const char* name) { return id == name; };
    if (type == "MouseButton") {
        if (is("Left") || is("MouseLeft") || is("Button1")) return VK_LBUTTON;
        if (is("Right") || is("MouseRight") || is("Button2")) return VK_RBUTTON;
        if (is("Middle") || is("MouseMiddle") || is("Button3")) return VK_MBUTTON;
        if (is("Button4") || is("X1")) return VK_XBUTTON1;
        if (is("Button5") || is("X2")) return VK_XBUTTON2;
        return 0;
    }
    if (type != "Key") return 0;
    if (id.size() == 1 && id[0] >= 'A' && id[0] <= 'Z') return id[0];
    if (id.size() == 7 && id.starts_with("Number") && id[6] >= '0' && id[6] <= '9') return id[6];
    if (id.size() == 8 && id.starts_with("KeyPad_") && id[7] >= '0' && id[7] <= '9') return VK_NUMPAD0 + (id[7] - '0');
    if (id.size() >= 2 && id[0] == 'F' && id[1] >= '1' && id[1] <= '9') {
        int n = 0;
        for (size_t k = 1; k < id.size(); ++k) {
            if (id[k] < '0' || id[k] > '9') return 0;
            n = n * 10 + (id[k] - '0');
        }
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
    }
    struct Named { const char* name; int vk; };
    static constexpr Named kNames[] = {
        {"Return", VK_RETURN}, {"Escape", VK_ESCAPE}, {"Backspace", VK_BACK}, {"Tab", VK_TAB}, {"Space", VK_SPACE},
        {"Minus", VK_OEM_MINUS}, {"Equals", VK_OEM_PLUS}, {"LeftBracket", VK_OEM_4}, {"RightBracket", VK_OEM_6},
        {"Backslash", VK_OEM_5}, {"NonUsHash", VK_OEM_5}, {"Semicolon", VK_OEM_1}, {"Apostrophe", VK_OEM_7},
        {"Grave", VK_OEM_3}, {"Comma", VK_OEM_COMMA}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2},
        {"Capslock", VK_CAPITAL}, {"Insert", VK_INSERT}, {"Home", VK_HOME}, {"PageUp", VK_PRIOR},
        {"Delete", VK_DELETE}, {"End", VK_END}, {"PageDown", VK_NEXT}, {"Right", VK_RIGHT}, {"Left", VK_LEFT},
        {"Down", VK_DOWN}, {"Up", VK_UP}, {"LeftControl", VK_LCONTROL}, {"LeftShift", VK_LSHIFT},
        {"LeftAlt", VK_LMENU}, {"RightControl", VK_RCONTROL}, {"RightShift", VK_RSHIFT}, {"RightAlt", VK_RMENU},
        {"KeyPad_Enter", VK_RETURN}, {"KeyPad_Plus", VK_ADD}, {"KeyPad_Minus", VK_SUBTRACT},
        {"KeyPad_Multiply", VK_MULTIPLY}, {"KeyPad_Divide", VK_DIVIDE}, {"KeyPad_Period", VK_DECIMAL}};
    for (const auto& n : kNames)
        if (is(n.name)) return n.vk;
    return 0;
}

// The interact keys in a user_settings.json text: up to two (primary, secondary). {'E'} when the
// file has no binding for it (the game's default), or cannot be read; {} when the player bound
// interact to something the mod cannot read as a key (it then keeps only the gamepad's Y).
struct Keys { int vk[2]{}; int count = 0; bool custom = false; struct Pad { uint16_t buttons = 0x8000; bool leftTrigger = false, rightTrigger = false; } pad{}; };
inline Keys InteractKeysFromSettings(std::string_view text) {
    Keys keys{{'E', 0}, 1, false};
    Json root;
    if (text.empty() || !Parser(text).Parse(root)) return keys;
    const Json* input = root.Get("input");
    const Json* km = input ? input->Get("keyboardMouse") : nullptr;
    const Json* bindings = km ? km->Get("bindings") : nullptr;
    const Json* gameplay = bindings ? bindings->Get("gameplay") : nullptr;
    if (!gameplay || gameplay->kind != Json::array) return keys;
    const Json* entry = nullptr;
    for (const char* name : {"ContextualAction", "ContextualAction_Tap"}) {
        for (const auto& item : gameplay->members) {
            const Json* id = item.second.Get("id");
            if (id && id->text == name) { entry = &item.second; break; }
        }
        if (entry) break;
    }
    const Json* action = entry ? entry->Get("action") : nullptr;
    if (!action) return keys;
    Keys found{};
    found.custom = true;
    for (const char* slot : {"primary", "secondary"}) {
        const Json* combo = action->Get(slot);
        const Json* op = combo ? combo->Get("operation") : nullptr;
        const Json* type = op ? op->Get("type") : nullptr;
        const Json* id = op ? op->Get("id") : nullptr;
        if (!type || !id) continue;
        if (const int vk = GameKeyToVk(type->text, id->text); vk && found.count < 2) found.vk[found.count++] = vk;
    }
    return found;
}

// A gamepad button by the game's name for it (keen::GamepadButton, read from the exe's type tables) as
// XInput button bits; the lower shoulders are the analog triggers. 0x8000 is XINPUT_GAMEPAD_Y, the
// game's default for Interact (FaceTop).
inline bool GamePadButton(std::string_view id, Keys::Pad& pad) {
    static constexpr struct { const char* name; uint16_t bits; } kButtons[] = {
        {"FaceTop", 0x8000}, {"FaceBottom", 0x1000}, {"FaceLeft", 0x4000}, {"FaceRight", 0x2000},
        {"DpadUp", 0x0001}, {"DpadDown", 0x0002}, {"DpadLeft", 0x0004}, {"DpadRight", 0x0008},
        {"SystemRight", 0x0010}, {"SystemLeft", 0x0020}, {"LeftStickPress", 0x0040}, {"RightStickPress", 0x0080},
        {"ShoulderLeftUpper", 0x0100}, {"ShoulderRightUpper", 0x0200}};
    for (const auto& b : kButtons)
        if (id == b.name) { pad.buttons |= b.bits; return true; }
    if (id == "ShoulderLeftLower") { pad.leftTrigger = true; return true; }
    if (id == "ShoulderRightLower") { pad.rightTrigger = true; return true; }
    return false;
}
inline bool FindPadButtons(const Json& node, Keys::Pad& pad) {
    bool found = node.kind == Json::string && GamePadButton(node.text, pad);
    for (const auto& member : node.members) found |= FindPadButtons(member.second, pad);
    return found;
}
// The game's Interact on a gamepad from the same file: FaceTop (Y) unless it was rebound.
inline Keys::Pad PadInteractFromSettings(std::string_view text) {
    Keys::Pad fallback{};
    Json root;
    if (text.empty() || !Parser(text).Parse(root)) return fallback;
    const Json* input = root.Get("input");
    const Json* gamepad = input ? input->Get("gamepad") : nullptr;
    const Json* bindings = gamepad ? gamepad->Get("bindings") : nullptr;
    const Json* gameplay = bindings ? bindings->Get("gameplay") : nullptr;
    if (!gameplay || gameplay->kind != Json::array) return fallback;
    for (const char* name : {"ContextualAction", "ContextualAction_Tap"}) {
        for (const auto& item : gameplay->members) {
            const Json* id = item.second.Get("id");
            if (!id || id->text != name) continue;
            const Json* action = item.second.Get("action");
            Keys::Pad found{};
            found.buttons = 0;
            if (action && FindPadButtons(*action, found)) return found;
            return fallback;
        }
    }
    return fallback;
}

// The newest user_settings.json for Enshrouded in Steam's userdata (one per Steam account
// that played on this PC); empty when there is none.
inline std::wstring SettingsPath() {
    wchar_t steam[MAX_PATH]{};
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) != ERROR_SUCCESS)
        return {};
    const std::wstring userdata = std::wstring(steam) + L"\\userdata\\";
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((userdata + L"*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return {};
    std::wstring best;
    FILETIME bestTime{};
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        const std::wstring path = userdata + found.cFileName + L"\\1203620\\remote\\user_settings.json";
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && CompareFileTime(&info.ftLastWriteTime, &bestTime) > 0) {
            best = path;
            bestTime = info.ftLastWriteTime;
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return best;
}

// The game's interact keys, read again when the file changes (checked at most every 2 s).
inline Keys GameInteractKeys() {
    static Keys keys{{'E', 0}, 1, false};
    static uint64_t checkedAt = 0;
    static FILETIME seen{};
    static std::wstring path;
    const uint64_t now = GetTickCount64();
    if (checkedAt && now - checkedAt < 2000) return keys;
    checkedAt = now;
    if (path.empty()) path = SettingsPath();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return keys;
    if (CompareFileTime(&info.ftLastWriteTime, &seen) == 0) return keys;
    seen = info.ftLastWriteTime;
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buffer[16384];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) text.append(buffer, n);
        std::fclose(f);
    }
    keys = InteractKeysFromSettings(text);
    keys.pad = PadInteractFromSettings(text);
    return keys;
}

}
