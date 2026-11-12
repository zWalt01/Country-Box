// Country-Box launcher: the drawn art (art.h). Plain shapes: gradients, polygons and curves, sized from the caller.
#include "art.h"

#include <cmath>
#include <initializer_list>

namespace art {

static constexpr float kPi = 3.14159265f;

static ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
static ImVec2 mul(ImVec2 a, float k) { return ImVec2(a.x * k, a.y * k); }
static ImVec2 norm(ImVec2 a) {
    float l = sqrtf(a.x * a.x + a.y * a.y);
    return l > 0 ? ImVec2(a.x / l, a.y / l) : a;
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

ImU32 alpha(ImU32 c, float a) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(c);
    x.w *= a;
    return ImGui::ColorConvertFloat4ToU32(x);
}

// ---- plants

void frond(ImDrawList *dl, ImVec2 base, float angle, float length, float width, float droop, ImU32 col) {
    ImVec2 dir(cosf(angle), sinf(angle));
    ImVec2 p1 = add(base, mul(dir, length * 0.5f));
    ImVec2 p2 = add(add(base, mul(dir, length)), ImVec2(0, droop * length));
    const int n = 18;
    ImVec2 prev = base;
    ImU32 dark = mix(col, IM_COL32(0, 0, 0, 255), 0.35f), light = mix(col, kCream, 0.18f);
    for (int i = 1; i <= n; i++) {
        float u = (float)i / n, v = 1 - u;
        ImVec2 p = ImVec2(v * v * base.x + 2 * v * u * p1.x + u * u * p2.x, v * v * base.y + 2 * v * u * p1.y + u * u * p2.y);
        ImVec2 tg = norm(ImVec2(p.x - prev.x, p.y - prev.y)), nn(-tg.y, tg.x);
        float seg = length / n;
        float len = width * powf(sinf(kPi * fminf(u * 1.15f, 1.0f)), 0.6f) * (1.0f - 0.25f * u) + width * 0.05f;
        if (i > 1)
            for (int side = -1; side <= 1; side += 2) {
                ImVec2 d = norm(add(add(mul(nn, 0.8f * side), mul(tg, 0.75f)), ImVec2(0, 0.35f)));
                ImVec2 a = add(p, mul(tg, -seg * 0.75f)), b = add(p, mul(tg, seg * 0.35f)), tip = add(p, mul(d, len));
                dl->AddTriangleFilled(a, tip, b, side < 0 ? col : mix(col, dark, 0.35f));
            }
        dl->AddLine(prev, p, i < n / 2 ? dark : light, fmaxf(1.0f, width * 0.07f * (1 - u)));
        prev = p;
    }
}

void palm(ImDrawList *dl, ImVec2 foot, float height, float lean, ImU32 trunk, ImU32 leaves) {
    const int n = 12;
    ImVec2 top = foot;
    for (int i = 0; i < n; i++) {
        float a = (float)i / n, b = (float)(i + 1) / n;
        ImVec2 qa(foot.x + lean * height * a * a, foot.y - height * a), qb(foot.x + lean * height * b * b, foot.y - height * b);
        float wa = height * 0.045f * (1 - 0.45f * a), wb = height * 0.045f * (1 - 0.45f * b);
        ImVec2 quad[4] = {ImVec2(qa.x - wa, qa.y), ImVec2(qa.x + wa, qa.y), ImVec2(qb.x + wb, qb.y + 1), ImVec2(qb.x - wb, qb.y + 1)};
        dl->AddConvexPolyFilled(quad, 4, (i & 1) ? trunk : mix(trunk, IM_COL32(0, 0, 0, 255), 0.18f));
        top = qb;
    }
    float L = height * 0.55f;
    static const float angles[] = {-3.05f, -2.55f, -2.0f, -1.45f, -0.95f, -0.45f, 0.05f};
    for (float a : angles) frond(dl, top, a, L, L * 0.2f, 0.45f, leaves);
    ImU32 nut = IM_COL32(92, 58, 28, 255);
    dl->AddCircleFilled(ImVec2(top.x - height * 0.03f, top.y + height * 0.04f), height * 0.035f, nut);
    dl->AddCircleFilled(ImVec2(top.x + height * 0.035f, top.y + height * 0.035f), height * 0.035f, mix(nut, IM_COL32(0, 0, 0, 255), 0.2f));
}

// ---- backdrop

void background(ImDrawList *dl, ImVec2 mn, ImVec2 mx, float s, float t) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    ImU32 top = IM_COL32(22, 64, 40, 255), mid = IM_COL32(14, 44, 28, 255), bot = IM_COL32(6, 22, 14, 255);
    dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + h * 0.5f), top, top, mid, mid);
    dl->AddRectFilledMultiColor(ImVec2(mn.x, mn.y + h * 0.5f), mx, mid, mid, bot, bot);
    // A warm glow behind the header, then light shafts through the canopy.
    ImVec2 glow(mn.x + w * 0.5f, mn.y + h * 0.05f);
    for (int i = 10; i > 0; i--) dl->AddCircleFilled(glow, h * 0.08f * i, IM_COL32(255, 214, 120, 5), 64);
    for (int i = 0; i < 5; i++) {
        float x = mn.x + w * (0.12f + 0.19f * i) + sinf(t * 0.15f + i) * 10 * s, sw = w * (0.025f + 0.012f * (i % 3));
        ImVec2 q[4] = {ImVec2(x, mn.y), ImVec2(x + sw, mn.y), ImVec2(x + sw * 3.5f + h * 0.35f, mx.y), ImVec2(x + h * 0.35f, mx.y)};
        dl->AddConvexPolyFilled(q, 4, IM_COL32(255, 240, 190, 7));
    }
    // Far bushes along the bottom.
    for (int layer = 0; layer < 2; layer++) {
        ImU32 c = layer ? IM_COL32(8, 30, 18, 255) : IM_COL32(12, 40, 24, 255);
        float base = mx.y - h * (layer ? 0.02f : 0.07f);
        for (int i = 0; i <= 16; i++) {
            float x = mn.x + w * i / 16.0f + (layer ? w / 32 : 0), r = h * (0.07f + 0.03f * ((i * 7 + layer * 3) % 4));
            dl->AddCircleFilled(ImVec2(x, base + r * 0.45f), r, c, 40);
        }
    }
    // Fronds hanging into both top corners and growing from the bottom corners.
    float sway = sinf(t * 0.7f) * 0.025f;
    struct F { float x, y, a, l, d; };
    static const F fr[] = {
        {-0.02f, -0.04f, 0.15f, 0.42f, 0.55f}, {-0.03f, -0.02f, 0.55f, 0.36f, 0.5f}, {0.04f, -0.05f, 0.95f, 0.30f, 0.3f},
        {-0.04f, 0.10f, 0.05f, 0.30f, 0.45f},
    };
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < 4; i++) {
            const F &f = fr[i];
            float x = side ? mx.x - w * f.x : mn.x + w * f.x;
            float a = side ? kPi - f.a - sway * (i + 1) : f.a + sway * (i + 1);
            ImU32 c = i % 2 ? kLeafDark : mix(kLeafDark, kLeaf, 0.45f);
            frond(dl, ImVec2(x, mn.y + h * f.y), a, h * f.l, h * f.l * 0.2f, f.d, c);
        }
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < 3; i++) {
            float x = side ? mx.x + h * 0.02f : mn.x - h * 0.02f;
            float a = side ? -kPi + 0.35f + 0.38f * i - sway : -0.35f - 0.38f * i + sway;
            frond(dl, ImVec2(x, mx.y + h * 0.02f), a, h * 0.32f, h * 0.06f, 0.35f, i == 1 ? kLeafDark : IM_COL32(16, 54, 30, 255));
        }
}

// ---- barrel

void barrel(ImDrawList *dl, ImVec2 c, float h, ImFont *font, const char *carved) {
    float w = h * 0.8f, hw = w * 0.5f, bulge = w * 0.08f, ry = h * 0.075f;
    float top = c.y - h * 0.5f, bot = c.y + h * 0.5f;
    auto half = [&](float u) { return hw + bulge * sinf(kPi * u); };
    ImVector<ImVec2> pts;
    const int n = 20;
    for (int i = 0; i <= n; i++) {   // left side, top to bottom
        float u = (float)i / n;
        pts.push_back(ImVec2(c.x - half(u), top + h * u));
    }
    for (int i = 1; i < n; i++) {    // bottom rim: the front half of an ellipse, left to right
        float a = kPi - kPi * i / n;
        pts.push_back(ImVec2(c.x + hw * cosf(a), bot + ry * sinf(a)));
    }
    for (int i = n; i >= 0; i--) {   // right side, bottom to top
        float u = (float)i / n;
        pts.push_back(ImVec2(c.x + half(u), top + h * u));
    }
    for (int i = 1; i < n; i++) {    // top rim: the front half of the lid's ellipse, right to left
        float a = kPi * i / n;
        pts.push_back(ImVec2(c.x + hw * cosf(a), top + ry * sinf(a)));
    }
    dl->AddConcavePolyFilled(pts.Data, pts.Size, kWood);
    // Staves
    for (int k = -3; k <= 3; k++) {
        if (!k) continue;
        float f = k / 4.0f;
        ImVec2 prev;
        for (int i = 0; i <= n; i++) {
            float u = (float)i / n;
            ImVec2 p(c.x + f * half(u), top + h * u + ry * sqrtf(fmaxf(0, 1 - f * f)));
            if (i) dl->AddLine(prev, p, alpha(kWoodDark, 0.7f), fmaxf(1, h * 0.012f));
            prev = p;
        }
    }
    // Shading: darker left, a soft highlight right of centre
    ImVector<ImVec2> shade;
    for (int i = 0; i <= n; i++) {
        float u = (float)i / n;
        shade.push_back(ImVec2(c.x - half(u), top + h * u));
    }
    for (int i = n; i >= 0; i--) {
        float u = (float)i / n;
        shade.push_back(ImVec2(c.x - half(u) * 0.55f, top + h * u + ry * 0.8f));
    }
    dl->AddConcavePolyFilled(shade.Data, shade.Size, IM_COL32(0, 0, 0, 50));
    for (int i = 0; i < n; i++) {
        float u = (float)i / n, v = (float)(i + 1) / n;
        ImVec2 q[4] = {ImVec2(c.x + half(u) * 0.18f, top + h * u + ry), ImVec2(c.x + half(u) * 0.42f, top + h * u + ry * 0.9f),
                       ImVec2(c.x + half(v) * 0.42f, top + h * v + ry * 0.9f), ImVec2(c.x + half(v) * 0.18f, top + h * v + ry)};
        if (i > 1 && i < n - 1) dl->AddConvexPolyFilled(q, 4, IM_COL32(255, 230, 190, 22));
    }
    // Hoops
    for (float u : {0.17f, 0.83f}) {
        float y = top + h * u, hh = half(u);
        ImVec2 hp[33];
        for (int i = 0; i <= 32; i++) {
            float a = kPi - kPi * i / 32;
            hp[i] = ImVec2(c.x + (hh + h * 0.006f) * cosf(a), y + ry * sinf(a));
        }
        dl->AddPolyline(hp, 33, IM_COL32(52, 52, 56, 255), 0, h * 0.075f);
        for (int i = 0; i <= 32; i++) hp[i].y -= h * 0.022f;
        dl->AddPolyline(hp + 2, 29, IM_COL32(150, 150, 160, 160), 0, h * 0.012f);
        for (float f : {-0.62f, -0.2f, 0.2f, 0.62f})
            dl->AddCircleFilled(ImVec2(c.x + f * hh, y + ry * sqrtf(1 - f * f)), h * 0.014f, IM_COL32(190, 190, 200, 255));
    }
    // Lid
    dl->AddEllipseFilled(ImVec2(c.x, top), ImVec2(hw, ry), kWoodDark, 0, 48);
    dl->AddEllipseFilled(ImVec2(c.x, top + ry * 0.12f), ImVec2(hw * 0.86f, ry * 0.7f), mix(kWood, kWoodDark, 0.3f), 0, 48);
    // Carved letters: a light lower edge, the dark cut over it
    if (font && carved) {
        float fs = h * 0.4f;
        ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, carved);
        ImVec2 p(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f + h * 0.03f);
        dl->AddText(font, fs, ImVec2(p.x + h * 0.012f, p.y + h * 0.016f), alpha(kWoodLight, 0.95f), carved);
        dl->AddText(font, fs, p, IM_COL32(58, 32, 12, 255), carved);
    }
}

// ---- banana

void banana(ImDrawList *dl, ImVec2 c, float size, float angle) {
    const int n = 24;
    float R = size * 0.5f, ca = cosf(angle), sa = sinf(angle);
    auto at = [&](float x, float y) { return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };
    ImVec2 outer[n + 1], inner[n + 1], spine[n + 1];
    for (int i = 0; i <= n; i++) {
        float u = -1 + 2.0f * i / n;
        float x = u * R, y = R * 0.5f * (1 - u * u) - R * 0.25f;   // a smile, belly down
        float dx = R, dy = -R * u;                                // derivative
        float l = sqrtf(dx * dx + dy * dy), nx = -dy / l, ny = dx / l;
        float th = R * 0.2f * powf(fmaxf(0, 1 - u * u), 0.55f) + R * 0.015f;
        outer[i] = at(x + nx * th, y + ny * th);
        inner[i] = at(x - nx * th * 0.8f, y - ny * th * 0.8f);
        spine[i] = at(x - nx * th * 0.1f, y - ny * th * 0.1f);
    }
    ImVec2 poly[2 * (n + 1)];
    for (int i = 0; i <= n; i++) poly[i] = outer[i], poly[2 * n + 1 - i] = inner[i];
    dl->AddConcavePolyFilled(poly, 2 * (n + 1), kBanana);
    dl->AddPolyline(outer + 2, n - 3, kBananaDark, 0, fmaxf(1, size * 0.03f));
    dl->AddPolyline(spine + 5, n - 9, IM_COL32(255, 238, 160, 200), 0, fmaxf(1, size * 0.025f));
    dl->AddPolyline(poly, 2 * (n + 1), IM_COL32(120, 80, 20, 140), ImDrawFlags_Closed, fmaxf(1, size * 0.012f));
    // the stem and the tip
    ImVec2 s0 = at(-R * 1.0f, -R * 0.25f), s1 = at(-R * 1.18f, -R * 0.42f);
    dl->AddLine(s0, s1, IM_COL32(110, 80, 30, 255), size * 0.07f);
    dl->AddCircleFilled(at(R * 0.99f, -R * 0.25f), size * 0.025f, IM_COL32(70, 46, 20, 255));
}

// ---- card scenery

static void snowflake(ImDrawList *dl, ImVec2 c, float r, ImU32 col, float rot) {
    for (int i = 0; i < 3; i++) {
        float a = rot + kPi * i / 3;
        dl->AddLine(ImVec2(c.x - r * cosf(a), c.y - r * sinf(a)), ImVec2(c.x + r * cosf(a), c.y + r * sinf(a)), col, fmaxf(1, r * 0.18f));
    }
}

void card_scene(ImDrawList *dl, ImVec2 mn, ImVec2 mx, const char *game, float s, float t) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    bool frozen = game && game[0] == 'd' && game[3] == 't';   // "dkctf"
    dl->PushClipRect(mn, mx, true);
    if (!frozen) {
        ImU32 sky0 = IM_COL32(236, 96, 66, 255), sky1 = IM_COL32(250, 170, 76, 255), sky2 = IM_COL32(255, 214, 120, 255);
        dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + h * 0.4f), sky0, sky0, sky1, sky1);
        dl->AddRectFilledMultiColor(ImVec2(mn.x, mn.y + h * 0.4f), ImVec2(mx.x, mn.y + h * 0.66f), sky1, sky1, sky2, sky2);
        ImVec2 sun(mn.x + w * 0.66f, mn.y + h * 0.6f);
        for (int i = 4; i > 0; i--) dl->AddCircleFilled(sun, h * (0.16f + 0.05f * i), IM_COL32(255, 236, 170, 22), 64);
        dl->AddCircleFilled(sun, h * 0.16f, IM_COL32(255, 238, 176, 255), 64);
        ImU32 sea0 = IM_COL32(40, 110, 128, 255), sea1 = IM_COL32(18, 60, 78, 255);
        dl->AddRectFilledMultiColor(ImVec2(mn.x, mn.y + h * 0.66f), mx, sea0, sea0, sea1, sea1);
        for (int i = 0; i < 6; i++) {
            float y = mn.y + h * (0.69f + 0.045f * i), x = sun.x + sinf(t * 0.8f + i) * w * 0.01f;
            float hw = w * (0.12f - 0.012f * i);
            dl->AddLine(ImVec2(x - hw, y), ImVec2(x + hw, y), IM_COL32(255, 220, 150, 120 - 15 * i), fmaxf(1, h * 0.012f));
        }
        // the island
        dl->AddEllipseFilled(ImVec2(mn.x + w * 0.22f, mn.y + h * 0.95f), ImVec2(w * 0.42f, h * 0.2f), IM_COL32(222, 184, 120, 255), 0, 64);
        dl->AddEllipseFilled(ImVec2(mn.x + w * 0.16f, mn.y + h * 0.99f), ImVec2(w * 0.36f, h * 0.17f), IM_COL32(30, 90, 50, 255), 0, 64);
        palm(dl, ImVec2(mn.x + w * 0.24f, mn.y + h * 0.84f), h * 0.62f, 0.18f, IM_COL32(110, 70, 36, 255), IM_COL32(34, 110, 52, 255));
        banana(dl, ImVec2(mn.x + w * 0.52f, mn.y + h * 0.86f), h * 0.2f, -0.35f);
        banana(dl, ImVec2(mn.x + w * 0.6f, mn.y + h * 0.84f), h * 0.2f, -0.1f);
    } else {
        ImU32 sky0 = IM_COL32(120, 190, 232, 255), sky1 = IM_COL32(214, 238, 252, 255);
        dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + h * 0.66f), sky0, sky0, sky1, sky1);
        // icy peaks
        auto peak = [&](float x, float base, float hw, float ph, ImU32 col) {
            ImVec2 a(mn.x + w * (x - hw), mn.y + h * base), b(mn.x + w * x, mn.y + h * (base - ph)), c(mn.x + w * (x + hw), mn.y + h * base);
            dl->AddTriangleFilled(a, b, c, col);
            ImVec2 l(b.x + (a.x - b.x) * 0.3f, b.y + (a.y - b.y) * 0.3f), r(b.x + (c.x - b.x) * 0.3f, b.y + (c.y - b.y) * 0.3f);
            dl->AddTriangleFilled(l, b, r, IM_COL32(250, 252, 255, 255));
        };
        peak(0.75f, 0.64f, 0.3f, 0.42f, IM_COL32(110, 150, 190, 255));
        peak(0.45f, 0.64f, 0.22f, 0.3f, IM_COL32(90, 132, 176, 255));
        ImU32 sea0 = IM_COL32(70, 140, 186, 255), sea1 = IM_COL32(30, 80, 128, 255);
        dl->AddRectFilledMultiColor(ImVec2(mn.x, mn.y + h * 0.64f), mx, sea0, sea0, sea1, sea1);
        for (int i = 0; i < 3; i++)
            dl->AddEllipseFilled(ImVec2(mn.x + w * (0.62f + 0.14f * i), mn.y + h * (0.76f + 0.07f * (i % 2))),
                                 ImVec2(w * 0.07f, h * 0.022f), IM_COL32(230, 245, 255, 230), 0, 32);
        dl->AddEllipseFilled(ImVec2(mn.x + w * 0.2f, mn.y + h * 0.97f), ImVec2(w * 0.42f, h * 0.2f), IM_COL32(240, 248, 255, 255), 0, 64);
        palm(dl, ImVec2(mn.x + w * 0.22f, mn.y + h * 0.84f), h * 0.6f, 0.15f, IM_COL32(104, 82, 70, 255), IM_COL32(64, 140, 120, 255));
        // snow on the crown
        dl->AddEllipseFilled(ImVec2(mn.x + w * 0.22f + h * 0.6f * 0.15f, mn.y + h * 0.24f), ImVec2(h * 0.09f, h * 0.03f), IM_COL32(250, 252, 255, 235), 0, 32);
        // a banana in an ice block
        ImVec2 ib0(mn.x + w * 0.46f, mn.y + h * 0.74f), ib1(mn.x + w * 0.66f, mn.y + h * 0.94f);
        banana(dl, ImVec2((ib0.x + ib1.x) * 0.5f, (ib0.y + ib1.y) * 0.5f), h * 0.17f, -0.3f);
        dl->AddRectFilled(ib0, ib1, IM_COL32(200, 236, 255, 110), h * 0.025f);
        dl->AddRect(ib0, ib1, IM_COL32(240, 252, 255, 220), h * 0.025f, 0, fmaxf(1, h * 0.008f));
        dl->AddLine(ImVec2(ib0.x + h * 0.02f, ib0.y + h * 0.05f), ImVec2(ib0.x + h * 0.05f, ib0.y + h * 0.02f), IM_COL32(255, 255, 255, 230), fmaxf(1, h * 0.008f));
        for (int i = 0; i < 14; i++) {
            float x = mn.x + w * fmodf(0.07f * i * 3.7f + 0.05f, 1.0f), y = mn.y + h * fmodf(0.11f * i * 1.9f + t * 0.03f, 0.62f);
            snowflake(dl, ImVec2(x, y), h * (0.012f + 0.006f * (i % 3)), IM_COL32(255, 255, 255, 200), t * 0.3f + i);
        }
    }
    dl->PopClipRect();
}

// ---- icons

void icon_check(ImDrawList *dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(c, r, col, 48);
    ImVec2 p[3] = {ImVec2(c.x - r * 0.45f, c.y + r * 0.02f), ImVec2(c.x - r * 0.12f, c.y + r * 0.35f), ImVec2(c.x + r * 0.5f, c.y - r * 0.32f)};
    dl->AddPolyline(p, 3, IM_COL32(255, 255, 255, 255), 0, r * 0.18f);
}

void icon_alert(ImDrawList *dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(c, r, col, 48);
    dl->AddRectFilled(ImVec2(c.x - r * 0.09f, c.y - r * 0.55f), ImVec2(c.x + r * 0.09f, c.y + r * 0.18f), IM_COL32(255, 255, 255, 255), r * 0.08f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.45f), r * 0.11f, IM_COL32(255, 255, 255, 255));
}

void icon_folder(ImDrawList *dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    dl->AddRectFilled(ImVec2(mn.x, mn.y), ImVec2(mn.x + w * 0.45f, mn.y + h * 0.3f), mix(col, IM_COL32(0, 0, 0, 255), 0.2f), h * 0.1f);
    dl->AddRectFilled(ImVec2(mn.x, mn.y + h * 0.15f), mx, col, h * 0.1f);
}

void icon_gear(ImDrawList *dl, ImVec2 c, float r, ImU32 col, ImU32 hole) {
    for (int i = 0; i < 8; i++) {
        float a = kPi * i / 4, ca = cosf(a), sa = sinf(a), tw = r * 0.2f;
        ImVec2 q[4] = {ImVec2(c.x + ca * r * 0.6f - sa * tw, c.y + sa * r * 0.6f + ca * tw), ImVec2(c.x + ca * r - sa * tw * 0.7f, c.y + sa * r + ca * tw * 0.7f),
                       ImVec2(c.x + ca * r + sa * tw * 0.7f, c.y + sa * r - ca * tw * 0.7f), ImVec2(c.x + ca * r * 0.6f + sa * tw, c.y + sa * r * 0.6f - ca * tw)};
        dl->AddConvexPolyFilled(q, 4, col);
    }
    dl->AddCircleFilled(c, r * 0.72f, col, 32);
    dl->AddCircleFilled(c, r * 0.3f, hole, 24);
}

void triangle(ImDrawList *dl, ImVec2 c, float r, int dir, ImU32 col) {
    if (dir == 2) dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y - r * 0.6f), ImVec2(c.x, c.y + r * 0.7f), col);
    else dl->AddTriangleFilled(ImVec2(c.x + dir * r * 0.75f, c.y), ImVec2(c.x - dir * r * 0.6f, c.y - r), ImVec2(c.x - dir * r * 0.6f, c.y + r), col);
}

void pad_glyph(ImDrawList *dl, ImVec2 c, float r, const char *label, ImU32 col, ImFont *font, float fs) {
    ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, label);
    if (label[1]) {   // a word: a pill
        float hw = fmaxf(r, ts.x * 0.5f + r * 0.5f);
        dl->AddRectFilled(ImVec2(c.x - hw, c.y - r * 0.8f), ImVec2(c.x + hw, c.y + r * 0.8f), col, r * 0.5f);
    } else dl->AddCircleFilled(c, r, col, 32);
    dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), kInk, label);
}

void spinner(ImDrawList *dl, ImVec2 c, float r, float t, ImU32 col, float thick) {
    float a0 = t * 5.0f, a1 = a0 + kPi * (1.0f + 0.5f * sinf(t * 2.0f));
    dl->PathArcTo(c, r, a0, a1, 32);
    dl->PathStroke(col, 0, thick);
}

}  // namespace art
