#include "Screens.h"

#include <math.h>

#include "AlbumArt.h"
#include "Library.h"
#include "MenuEngine.h"
#include "Util.h"
#include "../net/TimeSync.h"
#include "../state/AppState.h"

namespace Screens {
namespace {

TFT_eSPI *tftPtr = nullptr;

// Physical screen is 320x240 landscape (ILI9341, rotation 1) -- same frame
// the simulator's CSS aspect-ratio:320/240 was built against, so these
// pixel values line up 1:1 with the percentages used there.
constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;
constexpr int16_t kStatusbarH = 26; // ~11% of 240, matches .screen .statusbar
constexpr int16_t kBodyY = kStatusbarH;
constexpr int16_t kBodyH = kScreenH - kStatusbarH;

struct Palette {
    uint16_t bg, fg, muted, muted2, border, sbarBg, sbarFg, accent, accent2;
};

// Mirrors the simulator's --scr-* CSS custom properties for .screen /
// .screen.dark.
const Palette kLight = {
    TFT_WHITE, TFT_BLACK, 0x7BEF /*gray*/, 0xC618 /*light gray*/, 0xDEFB,
    0xDEFB, 0x2965, 0x2D9F /*blue*/, 0x0725 /*teal*/,
};
const Palette kDark = {
    0x1082 /*near-black*/, 0xFFFF, 0x8410, 0x6B4D, 0x2965,
    0x18E3, 0xFFFF, 0x2D9F, 0x0725,
};

const Palette &pal() { return state.darkMode ? kDark : kLight; }

void drawBtGlyph(int16_t x, int16_t y, int16_t size, uint16_t color) {
    // Same path as the simulator's inline SVG bluetooth glyph
    // (M7 7 L17 16 L12 21 L12 3 L17 8 L7 17), scaled from a 24x24 viewBox.
    auto sx = [&](float vx) -> int16_t { return x + (int16_t)(vx / 24.0f * size); };
    auto sy = [&](float vy) -> int16_t { return y + (int16_t)(vy / 24.0f * size); };
    int16_t pts[6][2] = {{sx(7), sy(7)}, {sx(17), sy(16)}, {sx(12), sy(21)},
                          {sx(12), sy(3)}, {sx(17), sy(8)}, {sx(7), sy(17)}};
    for (int i = 0; i < 5; i++) {
        tftPtr->drawLine(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], color);
    }
}

// Real vector icons, added after the user asked for nicer badges than
// the plain colored-letter ones everywhere but Bluetooth (which already
// had drawBtGlyph() above). Same deliberately-simple-primitives
// discipline as drawBtGlyph() -- plain lines/circles/rounded-rects from
// a normalized 24x24 viewBox, nothing that needs a curve library or
// careful anti-aliasing, so these can be reasoned about for correctness
// without a way to preview them on real hardware before the user flashes
// them. Each takes the same (x, y, size, color) shape as drawBtGlyph()
// so every call site can treat all icon glyphs identically.

// Single eighth note -- notehead + stem + flag. Used for both the Music
// main-menu tile ("note") and individual track rows ("track", smaller) --
// a track IS a song, so reusing the same glyph at a different scale
// reinforces that instead of needing a second, visually-unrelated icon.
void drawNoteGlyph(int16_t x, int16_t y, int16_t size, uint16_t color) {
    auto sx = [&](float vx) -> int16_t { return x + (int16_t)(vx / 24.0f * size); };
    auto sy = [&](float vy) -> int16_t { return y + (int16_t)(vy / 24.0f * size); };
    int16_t r = max((int16_t)1, (int16_t)(size * 3.0f / 24.0f));
    tftPtr->fillCircle(sx(8), sy(18), r, color);
    tftPtr->drawLine(sx(11), sy(18), sx(11), sy(4), color);
    tftPtr->drawLine(sx(11), sy(4), sx(17), sy(9), color);
}

// Three descending-length horizontal bars -- a standard "playlist/list"
// glyph, used for both the Playlists main-menu tile and playlist rows.
void drawPlaylistGlyph(int16_t x, int16_t y, int16_t size, uint16_t color) {
    auto sx = [&](float vx) -> int16_t { return x + (int16_t)(vx / 24.0f * size); };
    auto sy = [&](float vy) -> int16_t { return y + (int16_t)(vy / 24.0f * size); };
    tftPtr->drawLine(sx(2), sy(5), sx(21), sy(5), color);
    tftPtr->drawLine(sx(2), sy(12), sx(16), sy(12), color);
    tftPtr->drawLine(sx(2), sy(19), sx(11), sy(19), color);
}

// Three sliders (horizontal tracks with a knob at a different position on
// each) -- a standard "settings" glyph. Deliberately NOT a gear: a gear's
// teeth need enough resolution to read as teeth rather than a blob at
// small badge sizes, and get that wrong with no way to preview it first.
// Sliders are the same "settings" idea built entirely from lines + filled
// circles, same primitives as every other glyph here.
void drawSettingsGlyph(int16_t x, int16_t y, int16_t size, uint16_t color) {
    auto sx = [&](float vx) -> int16_t { return x + (int16_t)(vx / 24.0f * size); };
    auto sy = [&](float vy) -> int16_t { return y + (int16_t)(vy / 24.0f * size); };
    int16_t r = max((int16_t)1, (int16_t)(size * 2.0f / 24.0f));
    tftPtr->drawLine(sx(2), sy(5), sx(22), sy(5), color);
    tftPtr->fillCircle(sx(16), sy(5), r, color);
    tftPtr->drawLine(sx(2), sy(12), sx(22), sy(12), color);
    tftPtr->fillCircle(sx(8), sy(12), r, color);
    tftPtr->drawLine(sx(2), sy(19), sx(22), sy(19), color);
    tftPtr->fillCircle(sx(18), sy(19), r, color);
}

// Person silhouette (head + shoulders) -- Artist rows.
void drawArtistGlyph(int16_t x, int16_t y, int16_t size, uint16_t color) {
    auto sx = [&](float vx) -> int16_t { return x + (int16_t)(vx / 24.0f * size); };
    auto sy = [&](float vy) -> int16_t { return y + (int16_t)(vy / 24.0f * size); };
    int16_t r = max((int16_t)1, (int16_t)(size * 3.5f / 24.0f));
    tftPtr->fillCircle(sx(12), sy(8), r, color);
    int16_t rw = max((int16_t)1, (int16_t)(sx(18) - sx(6)));
    int16_t rh = max((int16_t)1, (int16_t)(sy(21) - sy(14)));
    tftPtr->fillRoundRect(sx(6), sy(14), rw, rh, max((int16_t)1, (int16_t)(size / 8)), color);
}

// Disc (outer ring + center hole) -- Album rows.
void drawAlbumGlyph(int16_t x, int16_t y, int16_t size, uint16_t color) {
    auto sx = [&](float vx) -> int16_t { return x + (int16_t)(vx / 24.0f * size); };
    auto sy = [&](float vy) -> int16_t { return y + (int16_t)(vy / 24.0f * size); };
    int16_t rOuter = max((int16_t)2, (int16_t)(size * 9.0f / 24.0f));
    int16_t rInner = max((int16_t)1, (int16_t)(size * 2.0f / 24.0f));
    tftPtr->drawCircle(sx(12), sy(12), rOuter, color);
    tftPtr->fillCircle(sx(12), sy(12), rInner, color);
}

void drawStatusbar() {
    const Palette &p = pal();
    tftPtr->fillRect(0, 0, kScreenW, kStatusbarH, p.sbarBg);
    tftPtr->setTextColor(p.sbarFg, p.sbarBg);
    tftPtr->setTextSize(1);
    tftPtr->drawFastHLine(0, kStatusbarH - 1, kScreenW, p.border);

    tftPtr->setCursor(10, 9);
    tftPtr->print(TimeSync::currentTimeString());

    int battPct = constrain(state.battery, 0, 100);
    int battX = kScreenW - 46;
    if (state.btOn) {
        drawBtGlyph(battX - 22, 6, 14, state.btConnectedTo.length() ? p.accent : p.muted);
    }
    char buf[6];
    snprintf(buf, sizeof(buf), "%d%%", battPct);
    tftPtr->setCursor(battX, 9);
    tftPtr->print(buf);
    int16_t shellX = kScreenW - 18, shellY = 8, shellW = 14, shellH = 9;
    tftPtr->drawRect(shellX, shellY, shellW, shellH, p.sbarFg);
    int16_t fillW = (shellW - 2) * battPct / 100;
    tftPtr->fillRect(shellX + 1, shellY + 1, fillW, shellH - 2, p.sbarFg);
}

// Small mini-device logo for the boot splash -- a rounded-rect body with
// a "screen" near the top and a round click-wheel (+ center button) near
// the bottom, echoing this project's own ANO-rotary-encoder-plus-5-
// buttons input (the actual hardware this whole UI navigates with)
// rather than an unrelated mark. Same deliberately-simple-primitives
// style every other hand-drawn glyph in this file already uses
// (drawBtGlyph()/drawNoteGlyph()/etc.) -- plain lines/circles/rounded-
// rects from a normalized size, nothing needing a curve library, so it
// can be reasoned about for correctness without a way to preview it
// before a flash.
void drawLogo(int16_t cx, int16_t cy, int16_t size, uint16_t bodyColor, uint16_t wheelColor, uint16_t accentColor) {
    int16_t w = size * 11 / 16;
    int16_t h = size;
    int16_t x = cx - w / 2;
    int16_t y = cy - h / 2;
    tftPtr->drawRoundRect(x, y, w, h, w / 6, bodyColor);
    int16_t scrW = w * 2 / 3, scrH = h * 7 / 24;
    tftPtr->drawRoundRect(cx - scrW / 2, y + h / 10, scrW, scrH, 2, bodyColor);
    int16_t wheelR = w * 2 / 5;
    int16_t wheelCy = y + h - wheelR - h / 10;
    tftPtr->drawCircle(cx, wheelCy, wheelR, wheelColor);
    tftPtr->fillCircle(cx, wheelCy, wheelR / 3, accentColor);
}

void drawBoot() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    drawLogo(kScreenW / 2, kBodyY + 58, 70, p.fg, p.fg, p.accent);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setTextSize(3);
    tftPtr->setCursor(kScreenW / 2 - 60, kBodyY + 108);
    tftPtr->print("clickpod");
    tftPtr->setTextSize(1);
    tftPtr->setTextColor(p.muted, p.bg);
    tftPtr->setCursor(kScreenW / 2 - 24, kBodyY + 142);
    tftPtr->print("booting...");
}

// Shared analog clock face renderer -- hour/minute hands + 12 tick
// marks, used by both the Set Time screen (editable, one hand
// highlighted) and the AOD/locked screen (read-only, just shows the
// current time). Every trig-derived endpoint uses lroundf(), not a
// truncating (int) cast -- the previous (int) casts truncated toward
// zero, which biases every tick/hand very slightly short, and
// unevenly (the exact bias depends on each angle's fractional part) --
// confirmed real, not cosmetic paranoia: reported as "12 and 00 aren't
// exactly centered." Rounding to the nearest pixel fixes that.
void drawAnalogClockFace(int cx, int cy, int radius, int hour, int minute, uint16_t faceColor,
                          uint16_t tickColor, bool highlightHour, bool highlightMinute, uint16_t highlightColor) {
    tftPtr->drawCircle(cx, cy, radius, faceColor);
    for (int i = 0; i < 12; i++) {
        float a = i * (2 * PI / 12) - PI / 2;
        int x1 = cx + (int)lroundf((radius - 8) * cosf(a));
        int y1 = cy + (int)lroundf((radius - 8) * sinf(a));
        int x2 = cx + (int)lroundf(radius * cosf(a));
        int y2 = cy + (int)lroundf(radius * sinf(a));
        tftPtr->drawLine(x1, y1, x2, y2, tickColor);
    }

    float hourAngle = ((hour % 12) + minute / 60.0f) * (2 * PI / 12) - PI / 2;
    int hourLen = radius * 55 / 100;
    uint16_t hourColor = highlightHour ? highlightColor : faceColor;
    int hx = cx + (int)lroundf(hourLen * cosf(hourAngle));
    int hy = cy + (int)lroundf(hourLen * sinf(hourAngle));
    tftPtr->drawLine(cx, cy, hx, hy, hourColor);
    if (highlightHour) {
        tftPtr->drawLine(cx + 1, cy, hx + 1, hy, hourColor); // thicker: active hand
        tftPtr->drawLine(cx, cy + 1, hx, hy + 1, hourColor);
    }

    float minAngle = minute * (2 * PI / 60) - PI / 2;
    int minLen = radius * 85 / 100;
    uint16_t minColor = highlightMinute ? highlightColor : faceColor;
    int mx = cx + (int)lroundf(minLen * cosf(minAngle));
    int my = cy + (int)lroundf(minLen * sinf(minAngle));
    tftPtr->drawLine(cx, cy, mx, my, minColor);
    if (highlightMinute) {
        tftPtr->drawLine(cx + 1, cy, mx + 1, my, minColor);
        tftPtr->drawLine(cx, cy + 1, mx, my + 1, minColor);
    }

    tftPtr->fillCircle(cx, cy, 2, faceColor);
}

// AOD/locked screen's clock -- separated from drawOff() so a once-a-minute
// tick (see UI.cpp's tickStatusbarClock(), which runs regardless of mode)
// can refresh just this (now including the analog face, not just the
// digital text -- user asked for the clock FACE to accompany the digital
// readout here too, matching the Set Time screen) without re-blitting the
// whole black screen every time, same reasoning as drawStatusbar()'s own
// partial-redraw path. Background here is already solid black from
// drawOff()'s one-time fillScreen(), so clearing just this region before
// redrawing is enough -- no flicker risk the way a full-body redraw would
// have. Redraws the whole face region every tick rather than tracking/
// erasing just the previous hand positions -- simplest correct approach,
// and this only fires once a minute, not worth the extra complexity of a
// hands-only partial erase.
constexpr int16_t kOffClockCx = kScreenW / 2;
constexpr int16_t kOffClockCy = 70;
constexpr int16_t kOffClockRadius = 46;
constexpr int16_t kOffClockDigitalY = kOffClockCy + kOffClockRadius + 12;

void drawOffClock() {
    tftPtr->fillRect(kOffClockCx - kOffClockRadius - 6, kOffClockCy - kOffClockRadius - 6,
                      (kOffClockRadius + 6) * 2, (kOffClockRadius + 6) * 2 + 30, TFT_BLACK);
    String t = TimeSync::currentTimeString();
    int hour = 0, minute = 0;
    if (t.length() == 5 && t[2] == ':') {
        hour = t.substring(0, 2).toInt();
        minute = t.substring(3, 5).toInt();
    }
    // Still shows a (frozen, pointing at 12) face even before any sync/
    // manual set -- "--:--" parses to hour=minute=0 via the guard above
    // failing, leaving hour/minute at their 0 default, which is a
    // reasonable, harmless default appearance rather than skipping the
    // face entirely.
    drawAnalogClockFace(kOffClockCx, kOffClockCy, kOffClockRadius, hour, minute, TFT_WHITE, TFT_WHITE, false, false,
                         TFT_WHITE);
    tftPtr->setTextColor(TFT_WHITE, TFT_BLACK);
    tftPtr->setTextSize(2);
    tftPtr->setCursor(kOffClockCx - 30, kOffClockDigitalY);
    tftPtr->print(t);
    tftPtr->setTextSize(1);
}

void drawOff() {
    tftPtr->fillScreen(TFT_BLACK);
    drawOffClock();
    tftPtr->setTextColor(0x4208, TFT_BLACK);
    tftPtr->setTextSize(1);
    tftPtr->setCursor(kScreenW / 2 - 60, kOffClockDigitalY + 32);
    tftPtr->print("hold CENTER to power on");
    // Real AOD requirement: playback keeps going while locked -- show that
    // it's still doing so, rather than a screen that looks fully "off"
    // while music is actually still playing behind it. User-reported gap,
    // fixed: this used to print just the bare word "playing"/"paused"
    // with no indication of WHAT -- now shows the track title underneath,
    // same centering convention drawNowPlaying() already uses elsewhere
    // (kScreenW/2 - length*3, text size 1's ~6px-wide chars) rather than
    // a new truncation scheme.
    if (state.now.hasTrack) {
        tftPtr->setCursor(kScreenW / 2 - 70, kOffClockDigitalY + 50);
        tftPtr->print(state.now.playing ? "playing" : "paused");
        tftPtr->setCursor(kScreenW / 2 - (int)state.now.title.length() * 3, kOffClockDigitalY + 62);
        tftPtr->print(state.now.title);
    }
}

// Matches the simulator's .menu-grid EXACTLY: flex-direction:column, so
// items stack as a single vertical column of full-width rows -- each row
// itself is icon+text side-by-side (that's the only "side by side" part),
// not a 2x2 grid of rows. Previous version of this function built an
// actual 2x2 grid, which was wrong -- see CLAUDE.md for how that was found.
void drawMainMenuGrid() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    Menu *m = MenuEngine::currentMenu();
    if (!m) return;

    const uint16_t badgeColors[4] = {0x2D9F, 0x855F, 0x0725, 0xFC80}; // blue/purple/teal/amber, matches .badge-0..3
    size_t n = m->items.size();
    if (n > 4) n = 4;
    if (n == 0) return;

    int16_t padX = kScreenW * 5 / 100;   // .menu-grid padding: 4% 5%
    int16_t padY = kBodyH * 4 / 100;
    int16_t gap = kBodyH * 4 / 100;      // .menu-grid gap: 4%
    int16_t rowW = kScreenW - 2 * padX;
    int16_t usableH = kBodyH - 2 * padY - gap * (int16_t)(n - 1);
    int16_t rowH = usableH / (int16_t)n;

    int16_t y = kBodyY + padY;
    for (size_t i = 0; i < n; i++) {
        int16_t cx = padX, cy = y;
        bool sel = (int)i == m->selected;

        if (sel) tftPtr->fillRoundRect(cx, cy, rowW, rowH, 8, p.accent);
        else tftPtr->drawRoundRect(cx, cy, rowW, rowH, 8, p.border);

        int16_t tilePad = rowW * 5 / 100;  // .tile padding: 0 5%
        int16_t badgeSize = min((int16_t)(rowW * 15 / 100), (int16_t)(rowH - 6)); // .tile-badge width:15%
        int16_t bx = cx + tilePad, by = cy + (rowH - badgeSize) / 2;

        tftPtr->fillRoundRect(bx, by, badgeSize, badgeSize, 6, badgeColors[i % 4]);
        const String &tileIcon = m->items[i].icon;
        if (tileIcon == "bt") {
            drawBtGlyph(bx + badgeSize / 4, by + badgeSize / 4, badgeSize / 2, TFT_WHITE);
        } else if (tileIcon == "note") {
            drawNoteGlyph(bx + badgeSize / 4, by + badgeSize / 4, badgeSize / 2, TFT_WHITE);
        } else if (tileIcon == "playlist") {
            drawPlaylistGlyph(bx + badgeSize / 4, by + badgeSize / 4, badgeSize / 2, TFT_WHITE);
        } else if (tileIcon == "gear") {
            drawSettingsGlyph(bx + badgeSize / 4, by + badgeSize / 4, badgeSize / 2, TFT_WHITE);
        } else {
            // Fallback for any future tile that doesn't set a recognized
            // icon -- shouldn't happen for the 4 current main-menu tiles,
            // kept only so a new tile added later degrades gracefully
            // instead of drawing nothing.
            tftPtr->setTextColor(TFT_WHITE, badgeColors[i % 4]);
            tftPtr->setTextSize(2);
            tftPtr->setCursor(bx + badgeSize / 2 - 6, by + badgeSize / 2 - 8);
            tftPtr->print(tileIcon.length() ? tileIcon.substring(0, 1) : "?");
        }

        int16_t textX = bx + badgeSize + (rowW * 4 / 100); // .tile gap: 4%
        tftPtr->setTextSize(1);
        tftPtr->setTextColor(sel ? TFT_WHITE : p.fg, sel ? p.accent : p.bg);
        tftPtr->setCursor(textX, cy + rowH / 2 - 10);
        tftPtr->print(m->items[i].label);
        String sub = m->items[i].liveSub();
        if (sub.length()) {
            tftPtr->setTextColor(sel ? TFT_WHITE : p.muted2, sel ? p.accent : p.bg);
            tftPtr->setCursor(textX, cy + rowH / 2 + 2);
            tftPtr->print(sub);
        }

        y += rowH + gap;
    }
}

constexpr int16_t kMenuRowH = 22;
constexpr int16_t kMenuListY = kBodyY + 26;

int16_t menuMaxRows() { return (kBodyH - 26) / kMenuRowH; }
int menuStartIdx(int selected) {
    int16_t maxRows = menuMaxRows();
    return selected >= maxRows ? selected - maxRows + 1 : 0;
}

// Small colored letter-badge icons for plain list rows (Artist/Album/
// Track/Playlist/Settings entries) -- everywhere that isn't the main-menu
// grid (which already has its own, larger version of this same idea, see
// drawMainMenuGrid() above) or the Bluetooth screen's real drawn glyph
// (drawBtGlyph()). Added because every list screen except Bluetooth's
// title bar had NO icon at all -- MenuItem::icon was only ever being set
// for the 4 main-menu tiles before this round.
//
// Deliberately reuses the same "colored rounded-rect + single capital
// letter" visual language as drawMainMenuGrid()'s tile badges (proven
// working there) rather than hand-drawing new vector glyphs per icon
// type the way drawBtGlyph() does -- inventing several new hand-drawn
// shapes blind, with no way to see how they actually render on real
// hardware, is a lot more surface area for a visual bug than reusing an
// already-shipped pattern. Not cross-checked against the browser
// simulator this round (see CLAUDE.md) -- this is new firmware-only
// polish, not a ported behavior/UX decision.
enum class RowGlyph { kNone, kBt, kNote, kPlaylist, kArtist, kAlbum, kLetter };

struct RowIcon {
    bool present = false;
    RowGlyph glyph = RowGlyph::kNone;
    char letter = 0;
    uint16_t color = 0;
};

RowIcon rowIconFor(const String &icon) {
    // Same 4-color rotation as drawMainMenuGrid()'s badgeColors.
    static const uint16_t colors[4] = {0x2D9F, 0x855F, 0x0725, 0xFC80};
    if (icon == "bt") return {true, RowGlyph::kBt, 0, colors[2]};
    if (icon == "artist") return {true, RowGlyph::kArtist, 0, colors[0]};
    if (icon == "album") return {true, RowGlyph::kAlbum, 0, colors[1]};
    if (icon == "track") return {true, RowGlyph::kNote, 0, colors[2]};
    if (icon == "playlist") return {true, RowGlyph::kPlaylist, 0, colors[1]};
    // Settings sub-rows: still simple letter badges for now -- not asked
    // for real glyphs this round, and five more distinct small icons is
    // real extra surface area for a rendering bug with no way to preview
    // them first. Letter badges already read fine at this size.
    if (icon == "brightness") return {true, RowGlyph::kLetter, 'B', colors[0]};
    if (icon == "sort") return {true, RowGlyph::kLetter, 'S', colors[1]};
    if (icon == "theme") return {true, RowGlyph::kLetter, 'T', colors[2]};
    if (icon == "timezone") return {true, RowGlyph::kLetter, 'Z', colors[3]};
    if (icon == "rescan") return {true, RowGlyph::kLetter, 'R', colors[0]};
    return {false, RowGlyph::kNone, 0, 0};
}

// Draws a row's icon badge (if it has one) and returns the x position the
// label text should start at -- a row with no icon (or an unrecognized
// one) gets back the old, unindented x=12 so nothing shifts for rows this
// round didn't touch.
int16_t drawRowIconIfAny(const MenuItem &item, int16_t y) {
    if (item.icon.length() == 0) return 12;
    RowIcon ic = rowIconFor(item.icon);
    if (!ic.present) return 12;

    int16_t badgeSize = 16;
    int16_t bx = 8, by = y + (kMenuRowH - badgeSize) / 2;
    tftPtr->fillRoundRect(bx, by, badgeSize, badgeSize, 4, ic.color);
    switch (ic.glyph) {
        case RowGlyph::kBt: drawBtGlyph(bx + 3, by + 3, badgeSize - 6, TFT_WHITE); break;
        case RowGlyph::kNote: drawNoteGlyph(bx + 3, by + 3, badgeSize - 6, TFT_WHITE); break;
        case RowGlyph::kPlaylist: drawPlaylistGlyph(bx + 3, by + 3, badgeSize - 6, TFT_WHITE); break;
        case RowGlyph::kArtist: drawArtistGlyph(bx + 3, by + 3, badgeSize - 6, TFT_WHITE); break;
        case RowGlyph::kAlbum: drawAlbumGlyph(bx + 3, by + 3, badgeSize - 6, TFT_WHITE); break;
        default: {
            tftPtr->setTextColor(TFT_WHITE, ic.color);
            tftPtr->setTextSize(1);
            tftPtr->setCursor(bx + badgeSize / 2 - 3, by + badgeSize / 2 - 4);
            char buf[2] = {ic.letter, 0};
            tftPtr->print(buf);
            break;
        }
    }
    return bx + badgeSize + 6;
}

// Shared by the full drawMenu() loop and updateMenuSelection()'s partial
// redraw below -- one place for the row layout so they can't drift apart.
// Always clears its own row background first (not just when selected),
// since the partial-redraw caller has no prior full-body clear to rely on.
void drawMenuRow(Menu *m, int i, int16_t y, const Palette &p) {
    bool sel = i == m->selected;
    tftPtr->fillRect(0, y, kScreenW, kMenuRowH, sel ? p.accent : p.bg);
    int16_t textX = drawRowIconIfAny(m->items[i], y);
    tftPtr->setTextColor(sel ? TFT_WHITE : p.fg, sel ? p.accent : p.bg);
    tftPtr->setCursor(textX, y + 6);
    tftPtr->print(m->items[i].label);
    String sub = m->items[i].liveSub();
    if (sub.length()) {
        tftPtr->setCursor(kScreenW - 12 - sub.length() * 6, y + 6);
        tftPtr->print(sub);
    }
}

// Baseline for updateMenuSelection()'s partial redraw: what was actually
// drawn on screen last, as of the most recent full drawMenu() call. -1
// means "no valid baseline, must fall back to a full redraw" (e.g. right
// after navigating into a different menu).
int lastDrawnMenuSelected = -1;
int lastDrawnMenuStartIdx = -1;

void drawMenu() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    Menu *m = MenuEngine::currentMenu();
    if (!m) { lastDrawnMenuSelected = -1; return; }

    tftPtr->setTextSize(1);
    if (state.mode == AppMode::BT) drawBtGlyph(10, kBodyY + 8, 12, p.accent);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setCursor(state.mode == AppMode::BT ? 26 : 10, kBodyY + 10);
    tftPtr->print(m->title);

    int maxRows = menuMaxRows();
    int startIdx = menuStartIdx(m->selected);
    int16_t y = kMenuListY;
    for (int i = startIdx; i < (int)m->items.size() && (i - startIdx) < maxRows; i++) {
        drawMenuRow(m, i, y, p);
        y += kMenuRowH;
    }
    lastDrawnMenuSelected = m->selected;
    lastDrawnMenuStartIdx = startIdx;
}

// Redraws just the previously-selected and newly-selected rows instead of
// the whole list -- the common case for plain UP/DOWN/rotate navigation.
// Escalates to a full state.dirty redraw (handled by the caller falling
// through to the normal dispatch in the same render() call, so there's no
// extra frame of delay) if the viewport needs to scroll to keep the new
// selection visible, or if there's no valid baseline yet.
void updateMenuSelection() {
    Menu *m = MenuEngine::currentMenu();
    if (!m || lastDrawnMenuSelected < 0) { state.dirty = true; return; }

    int newStartIdx = menuStartIdx(m->selected);
    if (newStartIdx != lastDrawnMenuStartIdx) { state.dirty = true; return; }

    const Palette &p = pal();
    if (lastDrawnMenuSelected != m->selected) {
        int16_t oldY = kMenuListY + (int16_t)(lastDrawnMenuSelected - lastDrawnMenuStartIdx) * kMenuRowH;
        drawMenuRow(m, lastDrawnMenuSelected, oldY, p);
    }
    int16_t newY = kMenuListY + (int16_t)(m->selected - newStartIdx) * kMenuRowH;
    drawMenuRow(m, m->selected, newY, p);
    lastDrawnMenuSelected = m->selected;
}

// Redraws just the progress bar + elapsed/remaining time strip, without
// touching the rest of the screen. Used for the once-a-second position
// tick (see UI.cpp's tickPlaybackClock/state.progressDirty) so playback
// doesn't full-screen-flicker every second -- only drawNowPlaying() (art,
// title, play state, etc. -- things that only change on a real track/mode
// change) does the full-body fillRect.
void drawNowPlayingProgress() {
    const Palette &p = pal();
    NowPlaying &n = state.now;
    if (!n.hasTrack) return;

    int16_t progY = kBodyY + kBodyH - 34;
    float pct = n.durSec ? min(1.0f, n.posSec / (float)n.durSec) : 0;
    int16_t barW = kScreenW - 40;

    // Clear just this strip (bar + time labels), not the whole body.
    tftPtr->fillRect(20, progY, barW, 16, p.bg);
    tftPtr->drawRect(20, progY, barW, 4, p.border);
    tftPtr->fillRect(20, progY, (int16_t)(barW * pct), 4, 0x2D9F);
    tftPtr->setTextColor(p.muted, p.bg);
    tftPtr->setCursor(20, progY + 8);
    tftPtr->print(fmtTime(n.posSec));
    String remain = "-" + fmtTime(n.durSec - n.posSec);
    tftPtr->setCursor(kScreenW - 20 - (int)remain.length() * 6, progY + 8);
    tftPtr->print(remain);
}

void drawNowPlaying() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    NowPlaying &n = state.now;

    if (!n.hasTrack) {
        tftPtr->setTextColor(p.muted, p.bg);
        tftPtr->setCursor(kScreenW / 2 - 60, kBodyY + kBodyH / 2 - 6);
        tftPtr->print("Nothing playing");
        tftPtr->setCursor(kScreenW / 2 - 80, kBodyY + kBodyH / 2 + 10);
        tftPtr->print("select a track from Music");
        return;
    }

    int16_t artSize = AlbumArt::kSize; // matches the simulator's ratio after the overflow fix
    int16_t artX = kScreenW / 2 - artSize / 2, artY = kBodyY + 8;
    AlbumArt::draw(artX, artY, artSize, artSize, n.art, 0x2D9F);

    int16_t metaY = artY + artSize + 8;
    tftPtr->setTextSize(1);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setCursor(kScreenW / 2 - (int)n.title.length() * 3, metaY);
    tftPtr->print(n.title);
    tftPtr->setTextColor(p.muted, p.bg);
    tftPtr->setCursor(kScreenW / 2 - (int)n.artist.length() * 3, metaY + 12);
    tftPtr->print(n.artist);
    tftPtr->setTextColor(p.muted2, p.bg);
    tftPtr->setCursor(kScreenW / 2 - (int)n.album.length() * 3, metaY + 23);
    tftPtr->print(n.album);

    drawNowPlayingProgress();

    int16_t bottomY = kBodyY + kBodyH - 14;
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setCursor(20, bottomY);
    tftPtr->print(n.playing ? "|| PAUSE" : "> PLAY");

    int16_t volX = kScreenW - 90;
    tftPtr->setTextColor(p.muted, p.bg);
    tftPtr->setCursor(volX, bottomY);
    tftPtr->print("V");
    int16_t volBarX = volX + 12, volBarW = 60;
    tftPtr->drawRect(volBarX, bottomY + 1, volBarW, 5, p.border);
    tftPtr->fillRect(volBarX, bottomY + 1, volBarW * state.volume / 100, 5, 0x2D9F);
}

void drawLyrics() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    auto it = Library::LYRICS.find(state.now.key);
    if (it == Library::LYRICS.end()) {
        tftPtr->setTextColor(p.muted, p.bg);
        tftPtr->setCursor(kScreenW / 2 - 60, kBodyY + kBodyH / 2 - 4);
        tftPtr->print("No lyrics for this track");
        return;
    }
    const std::vector<LyricLine> &lines = it->second;
    int activeIdx = MenuEngine::activeLyricIndex(lines); // shared with UI.cpp's tick -- see its comment
    int16_t lineH = 18;
    int16_t centerY = kBodyY + kBodyH / 2;
    for (size_t i = 0; i < lines.size(); i++) {
        int16_t y = centerY + ((int)i - activeIdx) * lineH;
        if (y < kBodyY - lineH || y > kBodyY + kBodyH) continue;
        bool active = (int)i == activeIdx;
        bool near = abs((int)i - activeIdx) == 1;
        tftPtr->setTextColor(active ? p.fg : (near ? p.muted : p.muted2), p.bg);
        tftPtr->setTextSize(active ? 1 : 1);
        tftPtr->setCursor(kScreenW / 2 - (int)lines[i].text.length() * 3, y);
        tftPtr->print(lines[i].text);
    }
}

// Renders the COMBINED [history | now playing | upcoming queue] list --
// previously this only ever showed state.queue (upcoming tracks), the
// classic "Spotify can't scroll back past where you started" limitation.
// state.queueSelected is now an index into this same combined space (see
// MenuEngine.cpp's playFromCombinedIndex()/moveQueueSelection()), not a
// plain state.queue index.
void drawQueue() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setCursor(10, kBodyY + 10);
    tftPtr->print(state.queueGrabbed ? "Queue - UP/DOWN move, RIGHT drop" : "Queue - RIGHT to grab, select to play");

    int histN = (int)state.history.size();
    int nowSlot = state.now.hasTrack ? 1 : 0;
    int queueN = (int)state.queue.size();
    int total = histN + nowSlot + queueN;

    if (total == 0) {
        tftPtr->setTextColor(p.muted, p.bg);
        tftPtr->setCursor(kScreenW / 2 - 40, kBodyY + kBodyH / 2);
        tftPtr->print("Queue is empty");
        return;
    }
    if (state.queueSelected >= total) state.queueSelected = total - 1;
    if (state.queueSelected < 0) state.queueSelected = 0;

    int16_t rowH = 24;
    int16_t y = kBodyY + 26;
    int16_t maxRows = (kBodyH - 26) / rowH;
    int16_t startIdx = 0;
    if (state.queueSelected >= maxRows) startIdx = state.queueSelected - maxRows + 1;

    for (int i = startIdx; i < total && (i - startIdx) < maxRows; i++) {
        bool sel = i == state.queueSelected;
        bool isNowRow = (i == histN) && nowSlot;
        bool grabbed = sel && state.queueGrabbed;
        uint16_t rowColor = grabbed ? 0xFD20 /*amber*/ : p.accent;

        const String *title;
        const String *artist;
        if (i < histN) {
            title = &state.history[i].title;
            artist = &state.history[i].artist;
        } else if (isNowRow) {
            title = &state.now.title;
            artist = &state.now.artist;
        } else {
            int qi = i - histN - nowSlot;
            title = &state.queue[qi].title;
            artist = &state.queue[qi].artist;
        }

        if (sel) tftPtr->fillRect(0, y, kScreenW, rowH, rowColor);
        if (grabbed) {
            // Three-bar "grip" glyph in place of the index number, and a
            // dashed border, so a grabbed row for reordering reads
            // differently from a merely-selected one.
            tftPtr->drawRect(0, y, kScreenW, rowH, TFT_WHITE);
            for (int b = 0; b < 3; b++) tftPtr->drawFastHLine(10, y + 8 + b * 4, 6, TFT_WHITE);
        } else if (isNowRow) {
            // A small filled "play" triangle instead of an index number --
            // this row is the anchor the rest of the list scrolls around
            // (history above, upcoming below), so it needs to read as
            // structurally different from a plain numbered row, not just
            // another list entry.
            uint16_t glyphColor = sel ? TFT_WHITE : p.accent;
            tftPtr->fillTriangle(12, y + 6, 12, y + 18, 20, y + 12, glyphColor);
        } else {
            tftPtr->setTextColor(sel ? TFT_WHITE : p.muted2, sel ? rowColor : p.bg);
            tftPtr->setCursor(12, y + 6);
            // Upcoming queue rows count forward from 1 (unchanged from
            // before); history rows count backward from -1 at the track
            // just before "now" -- a single index across the whole
            // combined list (e.g. "7") would have no obvious meaning
            // relative to what's actually playing.
            int label = (i < histN) ? (i - histN) : (i - histN - nowSlot + 1);
            tftPtr->print(String(label));
        }
        tftPtr->setTextColor(sel ? TFT_WHITE : (i < histN ? p.muted : p.fg), sel ? rowColor : p.bg);
        tftPtr->setCursor(34, y + 2);
        tftPtr->print(*title);
        tftPtr->setTextColor(sel ? 0xE73C : p.muted, sel ? rowColor : p.bg);
        tftPtr->setCursor(34, y + 12);
        tftPtr->print(*artist);
        y += rowH;
    }
}

// Manual "Set Time" screen (AppMode::SET_TIME) -- a real analog clock
// face (plain lines from the center, Arduino-trig, same simple-
// primitives style this file already uses for the hand-drawn glyphs --
// no curve library needed) plus a live digital readout underneath for
// actual glanceability (CLAUDE.md's plan item 6: "analog looks nice but
// isn't a quick read"). The active hand (state.setTimeEditingMinute --
// toggled by a RIGHT tap, see InputRouter.cpp) is drawn thicker/in the
// accent color so it's clear which one rotate() is currently sweeping.
void drawSetTime() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setCursor(10, kBodyY + 8);
    tftPtr->print("Set Time - RIGHT: switch hand, CENTER: save");

    int cx = kScreenW / 2;
    int cy = kBodyY + 26 + 70;
    int radius = 64;
    drawAnalogClockFace(cx, cy, radius, state.setTimeHour, state.setTimeMinute, p.fg, p.muted,
                         !state.setTimeEditingMinute, state.setTimeEditingMinute, p.accent);

    // Digital readout below the face -- the actual glanceable value.
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", state.setTimeHour, state.setTimeMinute);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setTextSize(2);
    tftPtr->setCursor(cx - 30, cy + radius + 14);
    tftPtr->print(buf);
    tftPtr->setTextSize(1);
}

} // namespace

void begin(TFT_eSPI &tft) { tftPtr = &tft; }

// ILI9341 commands (0x51 WRDISBV, 0x53 WRCTRLD) -- CONFIRMED on real
// hardware to be a no-op on this specific Waveshare module: its backlight
// bypasses the controller's internal PWM driver, going straight to an
// external transistor off the BL pin instead (see CLAUDE.md's backlight-
// hardware section). Left in anyway, harmless, in case a future board
// swap ever uses a module that DOES route brightness through the
// controller. No GPIO PWM drive right now -- BL is back on the 3.3V
// rail; both GPIO0 and GPIO12 failed real-hardware testing for this
// signal (see Pins.h), parked until a non-strapping pin is freed up
// instead of guessing at a third strapping pin.
void applyBrightness(int percent) {
    percent = constrain(percent, 0, 100);
    // ::map() -- same std::map/Arduino-map() ambiguity as main.cpp's
    // syncBluetoothToUi(), here because this file also includes
    // Library.h (for its <map>-based playlist overlay).
    uint8_t level = ::map(percent, 0, 100, 0, 255);

    if (tftPtr) {
        tftPtr->writecommand(0x53); // WRCTRLD
        tftPtr->writedata(0x2C);    // BCTRL + DD + BL on
        tftPtr->writecommand(0x51); // WRDISBV
        tftPtr->writedata(level);
    }
}

// One-off direct draw for a blocking operation with no other visual
// feedback (e.g. a manual library rescan) -- same reasoning as the boot
// splash fix (CLAUDE.md's third hardware bug): a blocking call with
// nothing on screen looks exactly like a hang/crash to whoever's holding
// the board. Bypasses the normal dirty-flag render() path on purpose --
// this is for a message that needs to appear immediately, synchronously,
// right before a long blocking call, not on the next render() pass.
void showBusyMessage(const String &msg) {
    if (!tftPtr) return;
    tftPtr->fillScreen(TFT_BLACK);
    tftPtr->setTextColor(TFT_WHITE, TFT_BLACK);
    tftPtr->setTextSize(1);
    tftPtr->setCursor(10, 110);
    tftPtr->print(msg);
}

void render() {
    if (!tftPtr) return;

    if (state.progressDirty && !state.dirty && state.mode == AppMode::NOW_PLAYING) {
        drawNowPlayingProgress();
    }
    state.progressDirty = false;

    if (state.selectionDirty && !state.dirty) {
        if (state.mode == AppMode::MENU || state.mode == AppMode::BT || state.mode == AppMode::TRACK_MENU) {
            updateMenuSelection(); // may itself set state.dirty as a scroll-needed fallback
        }
    }
    state.selectionDirty = false;

    // Lightest redraw of the three -- see UI.cpp's tickStatusbarClock().
    // Skipped for BOOT (splash hasn't necessarily drawn the statusbar yet).
    // OFF has no statusbar strip at all (that screen is deliberately
    // blank elsewhere) but DOES have its own big clock (drawOffClock())
    // that needs the same once-a-minute refresh, for the same reason --
    // AOD's "show clock" requirement needs it to actually advance while
    // locked, not freeze at whatever it showed when the screen locked.
    if (state.statusbarDirty && !state.dirty) {
        if (state.mode != AppMode::BOOT && state.mode != AppMode::OFF) {
            drawStatusbar();
        } else if (state.mode == AppMode::OFF) {
            drawOffClock();
        }
    }
    state.statusbarDirty = false;

    if (!state.dirty) return;

    if (state.mode == AppMode::BOOT) {
        drawStatusbar();
        drawBoot();
    } else if (state.mode == AppMode::OFF) {
        drawOff();
    } else if (MenuEngine::isMainMenuRoot()) {
        drawStatusbar();
        drawMainMenuGrid();
    } else if (state.mode == AppMode::MENU || state.mode == AppMode::BT || state.mode == AppMode::TRACK_MENU) {
        drawStatusbar();
        drawMenu();
    } else if (state.mode == AppMode::NOW_PLAYING) {
        drawStatusbar();
        drawNowPlaying();
    } else if (state.mode == AppMode::LYRICS) {
        drawStatusbar();
        drawLyrics();
    } else if (state.mode == AppMode::QUEUE) {
        drawStatusbar();
        drawQueue();
    } else if (state.mode == AppMode::SET_TIME) {
        drawStatusbar();
        drawSetTime();
    }

    state.dirty = false;
}

} // namespace Screens
