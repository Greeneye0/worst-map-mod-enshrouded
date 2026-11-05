#pragma once
// The game's own key and button tips, showing a mod's current binding (input_binds.h).
//
// The game draws a tip from a 0xc4-byte action binding record, its keyboard half or its gamepad
// half depending on the device in use. Read from the game's own table, *(G + 0xd0) + 0xf27c:
//   +0x00, +0x30  keyboard and mouse, primary and secondary; +0x62, +0x92  gamepad, the same.
//   Each is an operation of 12 bytes, then up to three modifiers made the same way.
//   Keyboard: u8 type 1 at +0, u16 USB HID usage at +2. Mouse: type 2, u32 button at +4 (1 left, 2 right, 3 middle).
//   Gamepad: type 6, u8 button at +6 (kPadIds below).
//   +0x60 modality, +0x61 toggle, +0xc2: the action's own; left alone.
#include <cstdint>
#include <cstring>
#include "input_binds.h"

namespace greeneye::hints {

constexpr size_t kRecordSize = 0xc4;
constexpr size_t kPadPrimary = 0x62, kPadSecondary = 0x92, kKeySecondary = 0x30, kOpSize = 12;

// The USB HID usage the game names a key by; 0 for a key it has no code for here.
inline uint16_t VkToHid(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return uint16_t(0x04 + vk - 'A');
    if (vk >= '1' && vk <= '9') return uint16_t(0x1e + vk - '1');
    if (vk == '0') return 0x27;
    if (vk >= VK_F1 && vk <= VK_F12) return uint16_t(0x3a + vk - VK_F1);
    if (vk >= VK_F13 && vk <= VK_F24) return uint16_t(0x68 + vk - VK_F13);
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return uint16_t(0x59 + vk - VK_NUMPAD1);
    static constexpr struct { int vk; uint16_t hid; } kKeys[] = {
        {VK_RETURN, 0x28}, {VK_ESCAPE, 0x29}, {VK_BACK, 0x2a}, {VK_TAB, 0x2b}, {VK_SPACE, 0x2c}, {VK_OEM_MINUS, 0x2d},
        {VK_OEM_PLUS, 0x2e}, {VK_OEM_4, 0x2f}, {VK_OEM_6, 0x30}, {VK_OEM_5, 0x31}, {VK_OEM_1, 0x33}, {VK_OEM_7, 0x34},
        {VK_OEM_3, 0x35}, {VK_OEM_COMMA, 0x36}, {VK_OEM_PERIOD, 0x37}, {VK_OEM_2, 0x38}, {VK_CAPITAL, 0x39},
        {VK_INSERT, 0x49}, {VK_HOME, 0x4a}, {VK_PRIOR, 0x4b}, {VK_DELETE, 0x4c}, {VK_END, 0x4d}, {VK_NEXT, 0x4e},
        {VK_RIGHT, 0x4f}, {VK_LEFT, 0x50}, {VK_DOWN, 0x51}, {VK_UP, 0x52}, {VK_DIVIDE, 0x54}, {VK_MULTIPLY, 0x55},
        {VK_SUBTRACT, 0x56}, {VK_ADD, 0x57}, {VK_NUMPAD0, 0x62}, {VK_DECIMAL, 0x63},
    };
    for (const auto& k : kKeys)
        if (k.vk == vk) return k.hid;
    return 0;
}

// The game's number for a controller button; 0 for none.
inline uint8_t PadId(uint16_t bit)
{
    using namespace greeneye::input;
    static constexpr struct { uint16_t bit; uint8_t id; } kPadIds[] = {
        {kPadLeft, 1}, {kPadRight, 2}, {kPadUp, 3}, {kPadDown, 4}, {kPadA, 5}, {kPadB, 6}, {kPadY, 7}, {kPadX, 8},
        {kPadLB, 9}, {kPadRB, 11}, {kPadLS, 13}, {kPadRS, 14}, {kPadMenu, 15}, {kPadView, 16},
    };
    for (const auto& p : kPadIds)
        if (p.bit == bit) return p.id;
    return 0;
}
constexpr uint8_t kPadIdLT = 10, kPadIdRT = 12;

inline void PutKey(uint8_t* op, uint16_t hid)
{
    op[0] = 1;
    std::memcpy(op + 2, &hid, sizeof(hid));
}

inline void PutChord(uint8_t* combo, const greeneye::input::KeyChord& c)
{
    uint8_t* mod = combo + kOpSize;
    const int mods = int(c.ctrl) + int(c.shift) + int(c.alt);
    int left = mods;
    auto modifier = [&](bool on, uint16_t hid) {
        if (!on) return;
        --left;
        if (!c.vk && left == 0) { // modifiers alone: the last is the key
            PutKey(combo, hid);
            return;
        }
        PutKey(mod, hid);
        mod += kOpSize;
    };
    modifier(c.ctrl, 0xe0);
    modifier(c.shift, 0xe1);
    modifier(c.alt, 0xe2);
    if (!c.vk) return;
    if (c.vk >= VK_LBUTTON && c.vk <= VK_XBUTTON2 && c.vk != VK_CANCEL) {
        const uint32_t button = c.vk == VK_LBUTTON ? 1 : c.vk == VK_RBUTTON ? 2 : c.vk == VK_MBUTTON ? 3 : c.vk == VK_XBUTTON1 ? 4 : 5;
        combo[0] = 2;
        std::memcpy(combo + 4, &button, sizeof(button));
    } else {
        PutKey(combo, VkToHid(c.vk));
    }
}

inline void PutPadChord(uint8_t* combo, const greeneye::input::PadChord& c)
{
    using namespace greeneye::input;
    uint8_t ids[6]{};
    int count = 0;
    // What is held first, the button pressed with it last: the game's main button is the last.
    if (c.lt) ids[count++] = kPadIdLT;
    if (c.rt) ids[count++] = kPadIdRT;
    for (uint16_t bit : {kPadLB, kPadRB, kPadView, kPadMenu, kPadLS, kPadRS, kPadUp, kPadDown, kPadLeft, kPadRight, kPadA, kPadB, kPadX, kPadY})
        if ((c.buttons & bit) && count < 6) ids[count++] = PadId(bit);
    if (!count) return;
    combo[0] = 6;
    combo[6] = ids[count - 1];
    for (int i = 0; i < count - 1 && i < 3; ++i) {
        uint8_t* mod = combo + kOpSize * (i + 1);
        mod[0] = 6;
        mod[6] = ids[i];
    }
}

// The binding into a record, leaving the action's own fields (modality, toggle, +0xc2) alone.
inline void WriteRecord(uint8_t* record, const greeneye::input::Binding& b)
{
    std::memset(record, 0, 0x60);
    std::memset(record + kPadPrimary, 0, 0xc2 - kPadPrimary);
    if (b.keyCount > 0) PutChord(record, b.keys[0]);
    if (b.keyCount > 1) PutChord(record + kKeySecondary, b.keys[1]);
    if (b.padCount > 0) PutPadChord(record + kPadPrimary, b.pads[0]);
    if (b.padCount > 1) PutPadChord(record + kPadSecondary, b.pads[1]);
}

// A tip has something to show on this device.
inline bool ShowsOn(const greeneye::input::Binding& b, bool padInUse)
{
    return padInUse ? b.padCount > 0 : b.keyCount > 0;
}

// ---- The running game ---------------------------------------------------------------------------

struct Native {
    const GreeneyeHost* host = nullptr;
    uint8_t** deviceGlobal = nullptr; // [global + 0x10] + 1: the game draws gamepad tips
    uint8_t (*row)(const void* binding, void* style) = nullptr;
    void* (*styleInit)(void* style, bool enabled) = nullptr;
    void (*named)(void* widget, const void* name) = nullptr;
    void (*end)(void* widget) = nullptr;
    void* (*beginLayer)(void* layer, const void* name, uint8_t fullScreen, uint32_t order, int32_t a, uint32_t b) = nullptr;
    void (*endLayer)(void* layer) = nullptr;
};
inline Native& Game()
{
    static Native native;
    return native;
}

struct Text {
    const char* utf8;
    size_t len;
    uint64_t hash;
};
inline Text MakeText(const char* s)
{
    Text t{s, std::strlen(s), 0xcbf29ce484222325ull};
    for (size_t i = 0; i < t.len; ++i) t.hash = (t.hash ^ uint8_t(s[i])) * 0x100000001b3ull;
    return t;
}

// Finds what it can. PadInUse works with deviceGlobal alone; Draw needs the rest.
inline void Init(const GreeneyeHost* host)
{
    Native& n = Game();
    if (!host || !host->find || !host->read) return;
    n.host = host;
    // The function that picks a record's half: "test r9d, r9d; jne; mov rcx, [rip+device]; call"
    if (uint8_t* pick = host->find("48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 49 8B F8 48 8B F2 48 8B D9 45 85 C9 75 ?? 48 8B 0D", nullptr)) {
        int32_t disp = 0;
        host->read(pick + 0x20, &disp, sizeof(disp));
        n.deviceGlobal = reinterpret_cast<uint8_t**>(pick + 0x24 + disp);
    }
    n.row = reinterpret_cast<decltype(n.row)>(host->find("40 55 56 57 48 8D AC 24 E0 FC FF FF 48 81 EC 20 04 00 00 44 8B 0A 48 8B FA 4C 8B C1", nullptr));
    n.styleInit = reinterpret_cast<decltype(n.styleInit)>(host->find("48 83 EC 28 45 33 C0 48 C7 41 2D 00 00 01 00 44 88 41 06", nullptr));
    n.named = reinterpret_cast<decltype(n.named)>(host->find("48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B F9 48 8B DA 45 33 C0 48 8B 52 10 48 8B 48 18 48 89 0F 48 C7 47", nullptr));
    n.end = reinterpret_cast<decltype(n.end)>(host->find("48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 80 79 20 00", nullptr));
    n.beginLayer = reinterpret_cast<decltype(n.beginLayer)>(host->find("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 45 8B D9 44 8B 8C 24 80 00 00 00 41 0F B6 D8", nullptr));
    n.endLayer = reinterpret_cast<decltype(n.endLayer)>(host->find("40 53 48 83 EC 20 48 89 74 24 38 48 8B D9 48 8B 35 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10 48 8B 09 E8", nullptr));
}

// The game is drawing gamepad tips: its own idea of the device in use, so a mod's tips always match
// the ones beside them.
inline bool PadInUse()
{
    uint8_t** global = Game().deviceGlobal;
    if (!global) return false;
    __try {
        uint8_t* device = *global;
        uint8_t* state = device ? *reinterpret_cast<uint8_t**>(device + 0x10) : nullptr;
        if (!state) return false;
#ifdef GREENEYE_INPUT_TEST
        // tools/inputtest.py key:0xFF:ms: gamepad tips with no pad, to see them on screen; back after.
        static bool forced = false;
        const input::TestOverlay* o = input::Overlay();
        const bool force = o && o->keys[0xff];
        if (force) state[1] = 1;
        else if (forced) state[1] = 0;
        forced = force;
#endif
        return state[1] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game can draw a tip of its own on a layer of its own.
inline bool CanDraw()
{
    const Native& n = Game();
    return n.row && n.styleInit && n.named && n.end && n.beginLayer && n.endLayer;
}

// A tip of the game's at (x, y) in full-screen UI units, on a layer of its own over the game's dialogs:
// the binding's key cap or button icon, then the label. keySize as the game's prompt uses: 32. False
// if the game's drawing is missing.
inline bool Draw(float x, float y, const char* label, const greeneye::input::Binding& binding, const char* widgetName, float keySize = 32.0f)
{
    Native& n = Game();
    if (!CanDraw()) return false;
    __try {
        alignas(16) uint8_t layer[0x400]{};
        const Text name = MakeText(widgetName);
        n.beginLayer(layer, &name, 1, 0x17, -1, 0); // just over the game's text dialog (0x16)
        alignas(16) uint8_t root[0x80]{};
        n.named(root, &name);
        alignas(16) uint8_t widget[0x80]{};
        n.named(widget, &name);
        if (uint8_t* node = *reinterpret_cast<uint8_t**>(widget + 8)) {
            *reinterpret_cast<uint16_t*>(node + 0x6e) = 0x0101;
            const float position[2] = {x, y};
            std::memcpy(node + 0x70, position, sizeof(position));
        }
        alignas(16) uint8_t style[0x78]{};
        n.styleInit(style, true);
        style[0x2f] = 0;
        style[0x4c] = 1;
        *reinterpret_cast<float*>(style + 0x3c) = 1.0f;
        *reinterpret_cast<float*>(style + 0x54) = keySize;
        *reinterpret_cast<Text*>(style + 0x58) = MakeText(label);
        alignas(16) uint8_t record[kRecordSize]{};
        WriteRecord(record, binding);
        n.row(record, style);
        n.end(widget);
        n.end(root);
        n.endLayer(layer);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n.row = nullptr; // the mod's own text from now on
        return false;
    }
}

} // namespace greeneye::hints
