#pragma once
// Rebindable keys and controller input, shared by the mods (see README.md).
//
// A binding is one line of text in a mod's settings, comma separated:
//     Shift+F, Pad:LS+X
//     Tab, NumPlus, Pad:View
// Keys: A-Z, 0-9, F1-F24, Num0-Num9, named keys (KeyNames below), Mouse1-Mouse5, 0xNN. Modifiers:
// Shift, Ctrl, Alt; alone they make a chord of their own (Shift+Alt). A chord matches its modifiers
// exactly, so Shift+F does not fire on Ctrl+Shift+F.
// Pad: A B X Y LB RB LT RT LS RS View Menu Up Down Left Right, joined with +.
//
// A value with no key part keeps the default keys and one with no Pad: part keeps the default pad
// buttons, so a settings file written before the mod had controller support gets it anyway.
// NoKeys and NoPad turn one side off, None (or Off) both.
#include <windows.h>
#include <xinput.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include "greeneye.h"

namespace greeneye::input {

constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadMenu = 0x0010,
                   kPadView = 0x0020, kPadLS = 0x0040, kPadRS = 0x0080, kPadLB = 0x0100, kPadRB = 0x0200,
                   kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;
constexpr uint16_t kPadDpad = kPadUp | kPadDown | kPadLeft | kPadRight;
constexpr uint16_t kPadFace = kPadA | kPadB | kPadX | kPadY;

struct PadState {
    uint16_t buttons = 0;
    bool lt = false, rt = false;
};

struct KeyChord {
    uint8_t vk = 0; // 0: modifiers alone
    bool shift = false, ctrl = false, alt = false;
    bool Empty() const { return !vk && !shift && !ctrl && !alt; }
};

struct PadChord {
    uint16_t buttons = 0;
    bool lt = false, rt = false;
    bool Empty() const { return !buttons && !lt && !rt; }
};

struct Binding {
    static constexpr int kMaxKeys = 4, kMaxPads = 2;
    KeyChord keys[kMaxKeys]{};
    PadChord pads[kMaxPads]{};
    int keyCount = 0, padCount = 0;
    bool Unbound() const { return !keyCount && !padCount; }
};

// ---- Text ------------------------------------------------------------------------------------

inline std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

inline bool Same(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

struct KeyName { const char* name; uint8_t vk; };
inline constexpr KeyName KeyNames[] = {
    // The first name of a key is the one written back.
    {"Enter", VK_RETURN}, {"Return", VK_RETURN}, {"Esc", VK_ESCAPE}, {"Escape", VK_ESCAPE}, {"Tab", VK_TAB},
    {"Space", VK_SPACE}, {"Backspace", VK_BACK}, {"CapsLock", VK_CAPITAL}, {"Insert", VK_INSERT}, {"Delete", VK_DELETE},
    {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Up", VK_UP}, {"Down", VK_DOWN},
    {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"NumPlus", VK_ADD}, {"NumMinus", VK_SUBTRACT}, {"NumMultiply", VK_MULTIPLY},
    {"NumDivide", VK_DIVIDE}, {"NumPeriod", VK_DECIMAL}, {"Minus", VK_OEM_MINUS}, {"Plus", VK_OEM_PLUS},
    {"Equals", VK_OEM_PLUS}, {"LeftBracket", VK_OEM_4}, {"RightBracket", VK_OEM_6}, {"Backslash", VK_OEM_5},
    {"Semicolon", VK_OEM_1}, {"Apostrophe", VK_OEM_7}, {"Backquote", VK_OEM_3}, {"Grave", VK_OEM_3}, {"Comma", VK_OEM_COMMA},
    {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2}, {"Mouse1", VK_LBUTTON}, {"Mouse2", VK_RBUTTON}, {"Mouse3", VK_MBUTTON},
    {"Mouse4", VK_XBUTTON1}, {"Mouse5", VK_XBUTTON2}, {"LeftMouse", VK_LBUTTON}, {"RightMouse", VK_RBUTTON},
    {"MiddleMouse", VK_MBUTTON}, {"[", VK_OEM_4}, {"]", VK_OEM_6}, {";", VK_OEM_1}, {"'", VK_OEM_7}, {"`", VK_OEM_3},
    {".", VK_OEM_PERIOD}, {"/", VK_OEM_2}, {"\\", VK_OEM_5}, {"-", VK_OEM_MINUS}, {"=", VK_OEM_PLUS},
};

struct PadName { const char* name; uint16_t bits; };
inline constexpr PadName PadNames[] = {
    // In the order written back: what is held first.
    {"LB", kPadLB}, {"RB", kPadRB}, {"LS", kPadLS}, {"RS", kPadRS}, {"View", kPadView}, {"Back", kPadView}, {"Menu", kPadMenu},
    {"Start", kPadMenu}, {"A", kPadA}, {"B", kPadB}, {"X", kPadX}, {"Y", kPadY}, {"Up", kPadUp}, {"Down", kPadDown},
    {"Left", kPadLeft}, {"Right", kPadRight},
};

// A key's name to its virtual key; 0 when unknown.
inline int ParseKeyName(std::string_view name)
{
    name = Trim(name);
    if (name.empty()) return 0;
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        const std::string text(name.substr(2));
        char* end = nullptr;
        const long v = std::strtol(text.c_str(), &end, 16);
        return *end == 0 && v > 0 && v < 256 ? int(v) : 0;
    }
    if ((name[0] == 'F' || name[0] == 'f') && name.size() >= 2 && name.size() <= 3 && name[1] >= '1' && name[1] <= '9' &&
        (name.size() == 2 || (name[2] >= '0' && name[2] <= '9'))) {
        const int n = name.size() == 2 ? name[1] - '0' : (name[1] - '0') * 10 + (name[2] - '0');
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
    }
    if (name.size() == 4 && _strnicmp(name.data(), "Num", 3) == 0 && name[3] >= '0' && name[3] <= '9')
        return VK_NUMPAD0 + (name[3] - '0');
    for (const KeyName& k : KeyNames)
        if (Same(name, k.name)) return k.vk;
    return 0;
}

inline std::string KeyToText(int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num" + std::to_string(vk - VK_NUMPAD0);
    for (const KeyName& k : KeyNames)
        if (k.vk == vk) return k.name;
    char hex[8];
    snprintf(hex, sizeof(hex), "0x%02X", unsigned(vk));
    return hex;
}

template <typename Fn>
inline bool SplitPlus(std::string_view text, Fn&& part)
{
    for (;;) {
        const size_t plus = text.find('+');
        if (!part(Trim(text.substr(0, plus)))) return false;
        if (plus == std::string_view::npos) return true;
        text.remove_prefix(plus + 1);
    }
}

inline bool ParseKeyChord(std::string_view text, KeyChord& out)
{
    KeyChord chord;
    const bool ok = SplitPlus(text, [&](std::string_view part) {
        if (Same(part, "Shift")) return chord.shift = true;
        if (Same(part, "Ctrl") || Same(part, "Control")) return chord.ctrl = true;
        if (Same(part, "Alt")) return chord.alt = true;
        const int vk = ParseKeyName(part);
        if (!vk || chord.vk) return false;
        chord.vk = uint8_t(vk);
        return true;
    });
    if (!ok || chord.Empty()) return false;
    out = chord;
    return true;
}

inline bool ParsePadChord(std::string_view text, PadChord& out)
{
    PadChord chord;
    const bool ok = SplitPlus(text, [&](std::string_view part) {
        if (Same(part, "LT")) return chord.lt = true;
        if (Same(part, "RT")) return chord.rt = true;
        for (const PadName& p : PadNames)
            if (Same(part, p.name)) {
                chord.buttons |= p.bits;
                return true;
            }
        return false;
    });
    if (!ok || chord.Empty()) return false;
    out = chord;
    return true;
}

// Text to a binding. False (and out = fallback, why = the part not understood) for a bad value.
inline bool ParseBinding(std::string_view text, const Binding& fallback, Binding& out, std::string* why = nullptr)
{
    out = fallback;
    text = Trim(text);
    if (text.empty()) return true;
    Binding parsed;
    bool noKeys = false, noPad = false, none = false;
    for (;;) {
        const size_t comma = text.find(',');
        const std::string_view part = Trim(text.substr(0, comma));
        bool ok = !part.empty();
        if (Same(part, "None") || Same(part, "Off")) none = true;
        else if (Same(part, "NoKeys")) noKeys = true;
        else if (Same(part, "NoPad")) noPad = true;
        else if (part.size() > 4 && _strnicmp(part.data(), "Pad:", 4) == 0) {
            PadChord chord;
            ok = parsed.padCount < Binding::kMaxPads && ParsePadChord(part.substr(4), chord);
            if (ok) parsed.pads[parsed.padCount++] = chord;
        } else if (ok) {
            KeyChord chord;
            ok = parsed.keyCount < Binding::kMaxKeys && ParseKeyChord(part, chord);
            if (ok) parsed.keys[parsed.keyCount++] = chord;
        }
        if (!ok) {
            if (why) *why = std::string(part);
            return false;
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (none) {
        out = Binding{};
        return true;
    }
    if (noKeys) { parsed.keyCount = 0; }
    else if (!parsed.keyCount) {
        for (int i = 0; i < fallback.keyCount; ++i) parsed.keys[i] = fallback.keys[i];
        parsed.keyCount = fallback.keyCount;
    }
    if (noPad) { parsed.padCount = 0; }
    else if (!parsed.padCount) {
        for (int i = 0; i < fallback.padCount; ++i) parsed.pads[i] = fallback.pads[i];
        parsed.padCount = fallback.padCount;
    }
    out = parsed;
    return true;
}

// A default written in code; a typo in one is a programming error caught by the mod's tests.
inline Binding MakeBinding(std::string_view text)
{
    Binding out;
    ParseBinding(text, Binding{}, out);
    return out;
}

inline std::string BindingToText(const Binding& b)
{
    if (b.Unbound()) return "None";
    std::string out;
    auto add = [&](const std::string& part) {
        if (!out.empty()) out += ", ";
        out += part;
    };
    for (int i = 0; i < b.keyCount; ++i) {
        std::string chord;
        if (b.keys[i].ctrl) chord += "Ctrl+";
        if (b.keys[i].shift) chord += "Shift+";
        if (b.keys[i].alt) chord += "Alt+";
        if (b.keys[i].vk) chord += KeyToText(b.keys[i].vk);
        else chord.pop_back();
        add(chord);
    }
    if (!b.keyCount) add("NoKeys");
    for (int i = 0; i < b.padCount; ++i) {
        std::string chord = "Pad:";
        const char* sep = "";
        if (b.pads[i].lt) { chord += "LT"; sep = "+"; }
        if (b.pads[i].rt) { chord += sep; chord += "RT"; sep = "+"; }
        for (const PadName& p : PadNames) {
            if (Same(p.name, "Back") || Same(p.name, "Start") || !(b.pads[i].buttons & p.bits)) continue;
            chord += sep;
            chord += p.name;
            sep = "+";
        }
        add(chord);
    }
    if (!b.padCount) add("NoPad");
    return out;
}

// ---- What is held (pure) ----------------------------------------------------------------------

template <typename Held>
inline bool ChordHeld(const KeyChord& c, Held&& held)
{
    if (c.Empty() || (c.vk && !held(int(c.vk)))) return false;
    return held(VK_SHIFT) == c.shift && held(VK_CONTROL) == c.ctrl && held(VK_MENU) == c.alt;
}

inline bool ChordHeld(const PadChord& c, const PadState& pad)
{
    return !c.Empty() && (pad.buttons & c.buttons) == c.buttons && (!c.lt || pad.lt) && (!c.rt || pad.rt);
}

template <typename Held>
inline bool KeysHeld(const Binding& b, Held&& held)
{
    for (int i = 0; i < b.keyCount; ++i)
        if (ChordHeld(b.keys[i], held)) return true;
    return false;
}

inline bool PadHeld(const Binding& b, const PadState& pad)
{
    for (int i = 0; i < b.padCount; ++i)
        if (ChordHeld(b.pads[i], pad)) return true;
    return false;
}

// The buttons of b's pad chords to keep from the game right now: a chord's face and D-pad buttons
// while the rest of the chord is held (hold LS, X is the mod's). A single button is never hidden.
inline uint16_t PadHideMask(const Binding& b, const PadState& pad)
{
    uint16_t hide = 0;
    for (int i = 0; i < b.padCount; ++i) {
        const PadChord& c = b.pads[i];
        const uint16_t action = c.buttons & (kPadFace | kPadDpad);
        const uint16_t modifier = c.buttons & uint16_t(~action);
        if (!action || (!modifier && !c.lt && !c.rt)) continue;
        if ((pad.buttons & modifier) == modifier && (!c.lt || pad.lt) && (!c.rt || pad.rt)) hide |= action;
    }
    return hide;
}

// Keeps buttons from the game until they come up, so a button still held when a mod lets go of it
// does not reach the game as a new press.
struct PadHide {
    uint16_t latched[4]{};
    uint16_t Apply(uint16_t buttons, uint32_t user, uint16_t hideNow)
    {
        if (user >= 4) return buttons;
        latched[user] = uint16_t((latched[user] | hideNow) & buttons);
        return uint16_t(buttons & ~latched[user]);
    }
};

// Any part of the binding still down, modifiers aside: wait for this before opening a text field, so
// the key's letter has gone to the game and a chord's button is not read by the field.
template <typename Held>
inline bool PartsHeld(const Binding& b, Held&& held, const PadState& pad)
{
    for (int i = 0; i < b.keyCount; ++i)
        if (b.keys[i].vk && held(int(b.keys[i].vk))) return true;
    for (int i = 0; i < b.padCount; ++i)
        if (pad.buttons & b.pads[i].buttons) return true;
    return false;
}

// One of b's keys is down whatever modifiers are held (Esc during Shift + hold still cancels).
// skipMouse leaves out the mouse buttons.
template <typename Held>
inline bool KeyOfHeld(const Binding& b, Held&& held, bool skipMouse = false)
{
    for (int i = 0; i < b.keyCount; ++i) {
        const int vk = b.keys[i].vk;
        if (vk && !(skipMouse && vk <= VK_XBUTTON2) && held(vk)) return true;
    }
    return false;
}

// A pad chord of b held with a button of it newly in pressed (PadPresses: repeats on the D-pad).
inline bool PadPressed(const Binding& b, uint16_t pressed, const PadState& pad)
{
    for (int i = 0; i < b.padCount; ++i)
        if ((pressed & b.pads[i].buttons) && ChordHeld(b.pads[i], pad)) return true;
    return false;
}

// Every button of b's pad chords.
inline uint16_t PadButtons(const Binding& b)
{
    uint16_t buttons = 0;
    for (int i = 0; i < b.padCount; ++i) buttons |= b.pads[i].buttons;
    return buttons;
}

// A binding the game thread sets and the pad filter reads, which may be on another thread.
struct SharedBinding {
    SRWLOCK lock = SRWLOCK_INIT;
    Binding binding;
    void Set(const Binding& b)
    {
        AcquireSRWLockExclusive(&lock);
        binding = b;
        ReleaseSRWLockExclusive(&lock);
    }
    Binding Get()
    {
        AcquireSRWLockShared(&lock);
        const Binding copy = binding;
        ReleaseSRWLockShared(&lock);
        return copy;
    }
};

// New presses, the D-pad repeating while held; for moving through a mod's panel.
struct PadPresses {
    uint16_t last = 0;
    uint64_t repeatAt = 0;
    uint16_t Update(uint16_t buttons, uint64_t now)
    {
        uint16_t pressed = uint16_t(buttons & ~last);
        last = buttons;
        if (pressed & kPadDpad) repeatAt = now + 380;
        else if ((buttons & kPadDpad) && now >= repeatAt) {
            pressed |= buttons & kPadDpad;
            repeatAt = now + 110;
        }
        return pressed;
    }
};

// A press's edges and how long it has been held.
struct Press {
    bool was = false;
    uint64_t since = 0;
    bool pressed = false, released = false;
    void Update(bool down, uint64_t now)
    {
        pressed = down && !was;
        released = !down && was;
        if (pressed) since = now;
        was = down;
    }
    uint64_t HeldMs(uint64_t now) const { return was ? now - since : 0; }
};

// ---- The machine -------------------------------------------------------------------------------
// Dev builds only (GREENEYE_INPUT_TEST): tools/inputtest.py publishes held keys and pad buttons in
// shared memory, so bindings can be checked in game with no focus and no real pad. Never in release.
#ifdef GREENEYE_INPUT_TEST
struct TestOverlay {
    uint32_t magic;        // 'GIPT'
    uint16_t buttons;
    uint8_t lt, rt;
    uint8_t keys[256];     // non-zero: held. 0xE0-0xE3: the emulated left stick, x and y as int16
};
inline const TestOverlay* Overlay()
{
    static const TestOverlay* view = nullptr;
    static uint64_t retryAt = 0;
    if (!view && GetTickCount64() >= retryAt) {
        retryAt = GetTickCount64() + 500;
        if (HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\GreeneyeInputTest"))
            view = static_cast<const TestOverlay*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(TestOverlay)));
    }
    return view && view->magic == 0x54504947 ? view : nullptr;
}
#endif

inline bool KeyDown(int vk)
{
#ifdef GREENEYE_INPUT_TEST
    if (const TestOverlay* o = Overlay(); o && vk >= 0 && vk < 256 && o->keys[vk]) return true;
#endif
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

inline bool GameHasFocus()
{
#ifdef GREENEYE_INPUT_TEST
    if (const TestOverlay* o = Overlay()) { (void)o; return true; }
#endif
    DWORD pid = 0;
    HWND window = GetForegroundWindow();
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
#ifdef GREENEYE_INPUT_TEST
inline void AskGameToScanPads();
#endif

inline XInputGetStateFn& PadReadFn()
{
    static XInputGetStateFn fn = nullptr;
    return fn;
}

// What every connected pad holds, read past this mod's own filter. Asking about an empty slot can
// stall, so one empty slot is tried every half second.
inline PadState ReadPad()
{
    static XInputGetStateFn fallback = [] {
        HMODULE module = GetModuleHandleW(L"xinput1_4.dll");
        if (!module) module = LoadLibraryW(L"xinput1_4.dll");
        if (!module) module = LoadLibraryW(L"xinput9_1_0.dll");
        return module ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState")) : nullptr;
    }();
    static bool connected[XUSER_MAX_COUNT]{};
    static DWORD nextEmpty = 0;
    static uint64_t probeAt = 0;
    static PadState cached{};
    static uint64_t cachedAt = 0;
    const uint64_t now = GetTickCount64();
    if (now == cachedAt) return cached;
    cachedAt = now;
    PadState out;
    const XInputGetStateFn get = PadReadFn() ? PadReadFn() : fallback;
    DWORD probe = XUSER_MAX_COUNT;
    if (now >= probeAt) {
        probeAt = now + 500;
        for (DWORD tries = 0; tries < XUSER_MAX_COUNT && probe == XUSER_MAX_COUNT; ++tries) {
            const DWORD slot = (nextEmpty + tries) % XUSER_MAX_COUNT;
            if (!connected[slot]) probe = slot;
        }
        if (probe != XUSER_MAX_COUNT) nextEmpty = (probe + 1) % XUSER_MAX_COUNT;
    }
    for (DWORD pad = 0; get && pad < XUSER_MAX_COUNT; ++pad) {
        if (!connected[pad] && pad != probe) continue;
        XINPUT_STATE state{};
        connected[pad] = get(pad, &state) == ERROR_SUCCESS;
        if (!connected[pad]) continue;
        out.buttons |= state.Gamepad.wButtons;
        out.lt |= state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        out.rt |= state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    }
#ifdef GREENEYE_INPUT_TEST
    AskGameToScanPads();
    if (const TestOverlay* o = Overlay()) {
        out.buttons |= o->buttons;
        out.lt |= o->lt != 0;
        out.rt |= o->rt != 0;
    }
#endif
    cached = out;
    return out;
}

// The binding held now, with this game in front.
inline bool Held(const Binding& b)
{
    if (!GameHasFocus()) return false;
    return KeysHeld(b, [](int vk) { return KeyDown(vk); }) || (b.padCount && PadHeld(b, ReadPad()));
}

// The filter: what the game reads from a pad passes through fn first (hide buttons, or everything
// while a mod's panel has the pad). Through the loader, so it chains with other mods and hot reloads.
using PadFilter = void (*)(XINPUT_GAMEPAD& pad, DWORD user);
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
struct PadHookState {
    const GreeneyeHost* host = nullptr;
    PadFilter filter = nullptr;
    XInputGetStateFn next = nullptr, nextEx = nullptr;
    XInputGetCapabilitiesFn nextCaps = nullptr;
};
inline PadHookState& PadHook()
{
    static PadHookState state;
    return state;
}
#ifdef GREENEYE_INPUT_TEST
// tools/inputtest.py --emulate: a pad on slot 0 as the game itself reads it, at its XInput imports.
inline bool EmulatingPad(DWORD user)
{
    const TestOverlay* o = user == 0 ? Overlay() : nullptr;
    return o && o->keys[0xfe];
}
inline void EmulatedState(XINPUT_STATE* st)
{
    static DWORD packet = 1;
    static uint32_t last = 0;
    const TestOverlay* o = Overlay();
    int16_t stick[2]{};
    std::memcpy(stick, o->keys + 0xE0, sizeof(stick));
    const uint32_t now = uint32_t(o->buttons) ^ uint32_t(o->lt) << 16 ^ uint32_t(o->rt) << 17 ^ (uint32_t(uint16_t(stick[0])) << 3) ^ (uint32_t(uint16_t(stick[1])) << 15);
    if (now != last) { last = now; ++packet; }
    std::memset(st, 0, sizeof(*st));
    st->dwPacketNumber = packet;
    st->Gamepad.wButtons = o->buttons;
    st->Gamepad.bLeftTrigger = o->lt ? 255 : 0;
    st->Gamepad.bRightTrigger = o->rt ? 255 : 0;
    st->Gamepad.sThumbLX = stick[0];
    st->Gamepad.sThumbLY = stick[1];
}
// The game looks at its four XInput slots only when a device was announced: its XInput object (found by
// what it holds: +8 the module, +0x18 XInputSetState, +0x20 XInputGetCapabilities) has a byte at +0x28
// saying "scan". Setting it is what a pad arriving does; the hooks then answer for slot 0.
inline void AskGameToScanPads()
{
    static bool emulatingBefore = false;
    static uint8_t* object = nullptr;
    const TestOverlay* o = Overlay();
    const bool emulating = o && o->keys[0xfe];
    const bool rising = emulating && !emulatingBefore;
    emulatingBefore = emulating;
    if (!rising) return;
    if (!object) {
        HMODULE module = GetModuleHandleW(L"xinput1_4.dll");
        const uint64_t wantModule = reinterpret_cast<uint64_t>(module);
        const uint64_t wantSet = reinterpret_cast<uint64_t>(module ? GetProcAddress(module, "XInputSetState") : nullptr);
        const uint64_t wantCaps = reinterpret_cast<uint64_t>(module ? GetProcAddress(module, "XInputGetCapabilities") : nullptr);
        MEMORY_BASIC_INFORMATION info{};
        for (uint8_t* at = nullptr; !object && VirtualQuery(at, &info, sizeof(info)); at = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize) {
            if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type == MEM_IMAGE) continue;
            auto* words = static_cast<uint64_t*>(info.BaseAddress);
            const size_t count = info.RegionSize / 8;
            for (size_t i = 0; i + 5 < count; ++i)
                if (words[i + 1] == wantModule && words[i + 3] == wantSet && words[i + 4] == wantCaps) {
                    object = reinterpret_cast<uint8_t*>(words + i);
                    break;
                }
        }
        if (const GreeneyeHost* host = PadHook().host; host && host->log) {
            char line[100];
            snprintf(line, sizeof(line), "input test: the game's XInput object is at %p", object);
            host->log(line);
        }
    }
    if (object) object[0x28] = 1;
}

// Who reads the pad, in the log while emulating: the game's calls come back into the exe.
inline void NoteRead(const char* what, DWORD user, void* from)
{
    static uint64_t nextLog = 0;
    static uint32_t count = 0;
    const GreeneyeHost* host = PadHook().host;
    if (!EmulatingPad(0)) return; // only while tools/inputtest.py --emulate runs
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(from), &module);
    if (module != GetModuleHandleW(nullptr)) return;
    ++count;
    const uint64_t now = GetTickCount64();
    if (host && host->log && now >= nextLog) {
        nextLog = now + 1000;
        char line[120];
        snprintf(line, sizeof(line), "input test: the game reads %s slot %lu from %p (%u calls so far)", what, user, from, count);
        host->log(line);
    }
}
inline DWORD WINAPI PadGetCapabilitiesHook(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    NoteRead("capabilities", user, _ReturnAddress());
    if (caps && EmulatingPad(user)) {
        std::memset(caps, 0, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
        return ERROR_SUCCESS;
    }
    return PadHook().nextCaps(user, flags, caps);
}
#endif
inline DWORD PadFiltered(XInputGetStateFn next, DWORD user, XINPUT_STATE* st)
{
    PadHookState& hook = PadHook();
    const bool active = !hook.host || hook.host->enter(hook.host->token);
    DWORD result = next(user, st);
#ifdef GREENEYE_INPUT_TEST
    if (st && result != ERROR_SUCCESS && EmulatingPad(user)) { // the lowest hook: a filter above it stays in force
        EmulatedState(st);
        result = ERROR_SUCCESS;
    }
#endif
    if (active && result == ERROR_SUCCESS && st && hook.filter) hook.filter(st->Gamepad, user);
    if (hook.host) hook.host->leave();
    return result;
}
inline DWORD WINAPI PadGetStateHook(DWORD user, XINPUT_STATE* st)
{
#ifdef GREENEYE_INPUT_TEST
    NoteRead("state", user, _ReturnAddress());
#endif
    return PadFiltered(PadHook().next, user, st);
}
inline DWORD WINAPI PadGetStateExHook(DWORD user, XINPUT_STATE* st)
{
#ifdef GREENEYE_INPUT_TEST
    NoteRead("stateEx", user, _ReturnAddress());
#endif
    return PadFiltered(PadHook().nextEx, user, st);
}

// Another hook's jump at the start of a function (Steam's overlay, Creative Mode, our other mods), in
// the forms common/jumppatch.h knows: jmp rel32, jmp [rip+disp32], mov rax, imm64; jmp rax.
inline bool StartsWithHookJump(const uint8_t* code)
{
    return code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25) ||
           (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0);
}

#ifdef GREENEYE_INPUT_TEST
// The game's pad update reads its window's record and does nothing while byte +0x1b, the window having
// focus, is 0. While emulating, only that update's own lookups say focused: the rest of the game (cursor
// capture, mouse look) still sees the truth. Addresses of the 2026-11-04 build, checked against their call
// targets: a test tool, not for release.
using WindowRecordFn = bool (*)(uint8_t* out, void* windows, uint32_t id);
inline WindowRecordFn& NextWindowRecord()
{
    static WindowRecordFn fn = nullptr;
    return fn;
}
inline bool WindowRecordForPads(uint8_t* out, void* windows, uint32_t id)
{
    PadHookState& hook = PadHook();
    const bool active = hook.host && hook.host->enter(hook.host->token);
    const bool found = NextWindowRecord()(out, windows, id);
    if (active && found && EmulatingPad(0)) out[0x1b] = 1;
    if (hook.host) hook.host->leave();
    return found;
}
inline void InstallPadFocusForTest(const GreeneyeHost* host)
{
    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    constexpr size_t kTarget = 0xfa3360;
    int done = 0;
    for (size_t site : {size_t(0xfbbd0f)}) {
        uint8_t code[5]{};
        host->read(exe + site, code, sizeof(code));
        int32_t rel = 0;
        std::memcpy(&rel, code + 1, sizeof(rel));
        if (code[0] != 0xE8 || size_t(site + 5 + rel) != kTarget) continue;
        auto* next = reinterpret_cast<WindowRecordFn>(host->redirectCall(exe + site, reinterpret_cast<void*>(&WindowRecordForPads)));
        if (next) { NextWindowRecord() = next; ++done; }
    }
    char line[100];
    snprintf(line, sizeof(line), "input test: pad update focus redirect: %d site", done);
    host->log(line);
}
#endif

// False when xinput1_4 starts with neither its usual code nor a hook: bindings still work through
// ReadPad, but the game also sees the buttons.
inline bool InstallPadFilter(const GreeneyeHost* host, PadFilter filter)
{
    if (!host || !host->hook || !host->read) return false;
    HMODULE module = LoadLibraryW(L"xinput1_4.dll");
    if (!module) return false;
    auto starts = [&](uint8_t* fn, const uint8_t* bytes, size_t n) {
        uint8_t have[32]{};
        host->read(fn, have, n);
        return std::memcmp(have, bytes, n) == 0 || StartsWithHookJump(have); // the loader chains onto a jump
    };
    // "mov [rsp+8], rbx; push rsi; push rdi; push r14; sub rsp, 30h"
    static const uint8_t kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
    // Ordinal 100: "mov [rsp+8], rbx; mov [rsp+10h], rbp; push rdi; sub rsp, 30h"
    static const uint8_t kPrologueEx[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30};
    auto* fn = reinterpret_cast<uint8_t*>(GetProcAddress(module, "XInputGetState"));
    auto* fnEx = reinterpret_cast<uint8_t*>(GetProcAddress(module, reinterpret_cast<LPCSTR>(100)));
    if (!fn || !starts(fn, kPrologue, sizeof(kPrologue))) return false;
    PadHookState& hook = PadHook();
    hook.host = host;
    hook.filter = filter;
    hook.next = reinterpret_cast<XInputGetStateFn>(host->hook(fn, reinterpret_cast<void*>(&PadGetStateHook), sizeof(kPrologue), nullptr, 0));
    if (!hook.next) return false;
    if (fnEx && starts(fnEx, kPrologueEx, sizeof(kPrologueEx)))
        hook.nextEx = reinterpret_cast<XInputGetStateFn>(host->hook(fnEx, reinterpret_cast<void*>(&PadGetStateExHook), sizeof(kPrologueEx), nullptr, 0));
    PadReadFn() = hook.next;
#ifdef GREENEYE_INPUT_FOCUS_SITE
    InstallPadFocusForTest(host);
#endif
#ifdef GREENEYE_INPUT_TEST
    // "mov [rsp+20h], rbx; push rsi; push rdi; push r14; sub rsp, 60h"
    static const uint8_t kPrologueCaps[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60};
    auto* fnCaps = reinterpret_cast<uint8_t*>(GetProcAddress(module, "XInputGetCapabilities"));
    if (fnCaps && starts(fnCaps, kPrologueCaps, sizeof(kPrologueCaps)))
        hook.nextCaps = reinterpret_cast<XInputGetCapabilitiesFn>(host->hook(fnCaps, reinterpret_cast<void*>(&PadGetCapabilitiesHook), sizeof(kPrologueCaps), nullptr, 0));
#endif
    return true;
}

// ---- Settings ------------------------------------------------------------------------------------

// True at most every 2 s, when the file's time changed; the first call says true.
struct FileWatch {
    uint64_t checkedAt = 0;
    FILETIME seen{};
    bool Changed(const char* path)
    {
        const uint64_t now = GetTickCount64();
        if (checkedAt && now - checkedAt < 2000) return false;
        const bool first = !checkedAt;
        checkedAt = now;
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return first;
        if (!first && CompareFileTime(&info.ftLastWriteTime, &seen) == 0) return false;
        seen = info.ftLastWriteTime;
        return true;
    }
};

// One setting of an ini file, written with its default text when missing so it can be found there.
// A bad value keeps the default; log (optional) gets the line.
inline Binding ReadIniBinding(const char* path, const char* section, const char* name, std::string_view defaultText,
                              void (*log)(const char* line) = nullptr)
{
    const Binding fallback = MakeBinding(defaultText);
    char text[128]{};
    if (!GetPrivateProfileStringA(section, name, "", text, sizeof(text), path)) {
        WritePrivateProfileStringA(section, name, std::string(defaultText).c_str(), path);
        return fallback;
    }
    Binding out;
    std::string why;
    if (!ParseBinding(text, fallback, out, &why) && log) {
        char line[220];
        snprintf(line, sizeof(line), "[%s] %s: '%s' is not a key or button this mod knows; using %s", section, name, why.c_str(),
                 BindingToText(fallback).c_str());
        log(line);
    }
    return out;
}

} // namespace greeneye::input
