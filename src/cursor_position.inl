// Settings > Advanced > Position, on the World Map: the X and Z under the cursor on a plate at the bottom, like the
// minimap's; its copy button or a middle click copies them ("X 3215, Z 2051"). Included by map_key_panel.cpp.

void CopyTextToClipboard(const char* text);

uint64_t g_mapPositionCopiedTick = 0;

void DrawMapCursorPosition(void* widgetRaw, const float* rootRect, uint8_t* mapState, bool haveCursor, float mx,
                           float my, bool overPanel) {
    if (!g_minimapShowPosition || !widgetRaw || !mapState) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1e0) + *reinterpret_cast<float*>(mapView + 0x17c) -
                          *reinterpret_cast<float*>(mapView + 0x1e8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1e4) + *reinterpret_cast<float*>(mapView + 0x180) -
                          *reinterpret_cast<float*>(mapView + 0x1ec);
    using namespace key_style;
    const float left = rootRect[0], top = rootRect[1];
    const float rootW = rootRect[2] > 100.0f ? rootRect[2] : 1920.0f;
    const float rootH = rootRect[3] > 100.0f ? rootRect[3] : 1080.0f;
    constexpr float kPlateH = 26.0f, kCellW = 92.0f, kCheckW = 26.0f, kLetterW = 13.0f;
    const float plateW = kCellW * 2.0f + 8.0f + kCheckW;
    const float plateX = left + rootW * 0.5f - plateW * 0.5f, plateY = top + rootH - 92.0f;
    const bool overPlate = haveCursor && mx >= plateX && mx <= plateX + plateW && my >= plateY && my <= plateY + kPlateH;
    const float copyX = plateX + plateW - 4.0f - kCheckW;
    const bool overCopy = overPlate && mx >= copyX;

    // The spot shown: the one under the cursor; on the plate, the last spot the cursor rested on (a third of a second
    // still), so a move from that spot to the copy button keeps it
    static int lastX = -1, lastZ = -1, restX = -1, restZ = -1;
    static float stillMx = -1.0f, stillMy = -1.0f;
    static uint64_t stillSince = 0;
    const uint64_t now = GetTickCount64();
    if (haveCursor && !overPanel && !overPlate) {
        const float wx = (mx - originX) / zoom, wz = mapWorldH - (my - originY) / zoom;
        if (wx >= 0.0f && wz >= 0.0f && wx <= 10240.0f && wz <= 10240.0f) {
            lastX = static_cast<int>(std::lround(wx));
            lastZ = static_cast<int>(std::lround(wz));
            if (std::fabs(mx - stillMx) > 3.0f || std::fabs(my - stillMy) > 3.0f) {
                stillMx = mx;
                stillMy = my;
                stillSince = now;
            } else if (now - stillSince >= 330) {
                restX = lastX;
                restZ = lastZ;
            }
        }
    }
    if (lastX < 0) { // no cursor on the map yet: the middle of the view
        const float cx = left + rootW * 0.5f, cy = top + rootH * 0.5f;
        lastX = std::max(0, std::min(10240, static_cast<int>(std::lround((cx - originX) / zoom))));
        lastZ = std::max(0, std::min(10240, static_cast<int>(std::lround(mapWorldH - (cy - originY) / zoom))));
    }
    const int shownX = overPlate && restX >= 0 ? restX : lastX, shownZ = overPlate && restX >= 0 ? restZ : lastZ;

    // Copy: a click on the copy button, or a middle click anywhere on the map
    static bool leftWasDown = false, middleWasDown = false;
    const bool leftDown = overCopy && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool middleDown = haveCursor && !overPanel && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    if ((leftDown && !leftWasDown) || (middleDown && !middleWasDown)) {
        char text[48]{};
        std::snprintf(text, sizeof(text), "X %d, Z %d", shownX, shownZ);
        CopyTextToClipboard(text);
        g_mapPositionCopiedTick = now;
        if (leftDown) ClearMapViewClickLatch(mapState); // no waypoint under the button
    }
    leftWasDown = leftDown;
    middleWasDown = middleDown;

    constexpr uint32_t kTraySlate = 0xFF6C5A58u, kTrayEdge = 0xFF2A2020u, kTrayGoldLine = 0xFF4986A0u;
    DrawFramedBoxAbs(widgetRaw, plateX, plateY, plateW, kPlateH, kTraySlate, 0.97f, kTrayEdge, 0.95f, 6.0f);
    DrawFramedBoxAbs(widgetRaw, plateX + 2.0f, plateY + 2.0f, plateW - 4.0f, kPlateH - 4.0f, kTraySlate, 1.0f,
                     kTrayGoldLine, 0.75f, 5.0f);
    DrawBoxAbs(widgetRaw, kBoxInnerGlowPrimitive, plateX + 3.0f, plateY + 3.0f, plateW - 6.0f, kPlateH - 6.0f,
               0xFF000000u, 0.25f, 6.0f);
    const char* const kAxis[2] = {"X", "Z"};
    const int value[2] = {shownX, shownZ};
    for (int axis = 0; axis < 2; ++axis) {
        const float cx = plateX + 4.0f + kCellW * axis;
        if (axis > 0) DrawSolidRectAbs(widgetRaw, cx - 0.5f, plateY + 5.0f, 1.0f, kPlateH - 10.0f, 0xFF62A8C4u, 0.7f);
        char num[16]{};
        std::snprintf(num, sizeof(num), "%d", value[axis]);
        DrawLabelAbs(cx + 9.0f, plateY + 1.0f, kLetterW, kPlateH, kAxis[axis], 0xFF7FD3EEu, 0.8f, TextAlignH::Left,
                     TextAlignV::Center, nullptr, 0.72f);
        DrawLabelAbs(cx + 9.0f + kLetterW, plateY, kCellW - kLetterW - 12.0f, kPlateH, num, 0xFFC6EFFCu, 0.97f,
                     TextAlignH::Left, TextAlignV::Center, nullptr, 0.8f);
    }
    // The copy mark: two sheets, a check for a moment once copied
    const float bx = copyX;
    DrawSolidRectAbs(widgetRaw, bx - 0.5f, plateY + 5.0f, 1.0f, kPlateH - 10.0f, 0xFF62A8C4u, 0.7f);
    const float bcx = bx + kCheckW * 0.5f + 1.0f, bcy = plateY + kPlateH * 0.5f;
    if (overCopy)
        DrawAssetRectAbs(widgetRaw, kSoftGlowPrimitive, bcx - 13.0f, bcy - 13.0f, 26.0f, 26.0f, 0xFFC6EFFCu, 0.4f);
    if (g_mapPositionCopiedTick && GetTickCount64() - g_mapPositionCopiedTick < 1500) {
        DrawAssetRectAbs(widgetRaw, kCheckmarkPrimitive, bcx - 8.0f, bcy - 8.0f, 16.0f, 16.0f, 0xFF8EE0B8u, 0.98f);
    } else {
        auto sheet = [&](float x, float y, float w, float h, bool fill) {
            if (fill) DrawSolidRectAbs(widgetRaw, x, y, w, h, kTraySlate, 1.0f);
            DrawSolidRectAbs(widgetRaw, x, y, w, 1.5f, 0xFFC6EFFCu, 0.9f);
            DrawSolidRectAbs(widgetRaw, x, y + h - 1.5f, w, 1.5f, 0xFFC6EFFCu, 0.9f);
            DrawSolidRectAbs(widgetRaw, x, y, 1.5f, h, 0xFFC6EFFCu, 0.9f);
            DrawSolidRectAbs(widgetRaw, x + w - 1.5f, y, 1.5f, h, 0xFFC6EFFCu, 0.9f);
        };
        sheet(bcx - 6.0f, bcy - 7.0f, 9.0f, 11.0f, false);
        sheet(bcx - 3.0f, bcy - 4.0f, 9.0f, 11.0f, true);
    }
}
