#include "Screens.h"

#include "AlbumArt.h"
#include "Library.h"
#include "MenuEngine.h"
#include "Util.h"
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

void drawStatusbar() {
    const Palette &p = pal();
    tftPtr->fillRect(0, 0, kScreenW, kStatusbarH, p.sbarBg);
    tftPtr->setTextColor(p.sbarFg, p.sbarBg);
    tftPtr->setTextSize(1);
    tftPtr->drawFastHLine(0, kStatusbarH - 1, kScreenW, p.border);

    tftPtr->setCursor(10, 9);
    tftPtr->print("--:--"); // TODO: wire to a real clock/RTC once one exists

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

void drawBoot() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setTextSize(3);
    tftPtr->setCursor(kScreenW / 2 - 60, kBodyY + kBodyH / 2 - 20);
    tftPtr->print("clickpod");
    tftPtr->setTextSize(1);
    tftPtr->setTextColor(p.muted, p.bg);
    tftPtr->setCursor(kScreenW / 2 - 24, kBodyY + kBodyH / 2 + 14);
    tftPtr->print("booting...");
}

void drawOff() {
    tftPtr->fillScreen(TFT_BLACK);
    tftPtr->setTextColor(0x4208, TFT_BLACK);
    tftPtr->setTextSize(1);
    tftPtr->setCursor(kScreenW / 2 - 60, kScreenH / 2 - 4);
    tftPtr->print("hold CENTER to power on");
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
        if (m->items[i].icon == "bt") {
            drawBtGlyph(bx + badgeSize / 4, by + badgeSize / 4, badgeSize / 2, TFT_WHITE);
        } else {
            tftPtr->setTextColor(TFT_WHITE, badgeColors[i % 4]);
            tftPtr->setTextSize(2);
            tftPtr->setCursor(bx + badgeSize / 2 - 6, by + badgeSize / 2 - 8);
            String letter = m->items[i].icon == "note" ? "M" : m->items[i].icon == "playlist" ? "P" : "S";
            tftPtr->print(letter);
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

// Shared by the full drawMenu() loop and updateMenuSelection()'s partial
// redraw below -- one place for the row layout so they can't drift apart.
// Always clears its own row background first (not just when selected),
// since the partial-redraw caller has no prior full-body clear to rely on.
void drawMenuRow(Menu *m, int i, int16_t y, const Palette &p) {
    bool sel = i == m->selected;
    tftPtr->fillRect(0, y, kScreenW, kMenuRowH, sel ? p.accent : p.bg);
    tftPtr->setTextColor(sel ? TFT_WHITE : p.fg, sel ? p.accent : p.bg);
    tftPtr->setCursor(12, y + 6);
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
    int activeIdx = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        if (state.now.posSec >= lines[i].atSec) activeIdx = (int)i;
    }
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

void drawQueue() {
    const Palette &p = pal();
    tftPtr->fillRect(0, kBodyY, kScreenW, kBodyH, p.bg);
    tftPtr->setTextColor(p.fg, p.bg);
    tftPtr->setCursor(10, kBodyY + 10);
    tftPtr->print(state.queueGrabbed ? "Queue - UP/DOWN move, RIGHT drop" : "Queue - RIGHT to grab, select to play");

    if (state.queue.empty()) {
        tftPtr->setTextColor(p.muted, p.bg);
        tftPtr->setCursor(kScreenW / 2 - 40, kBodyY + kBodyH / 2);
        tftPtr->print("Queue is empty");
        return;
    }
    if (state.queueSelected >= (int)state.queue.size()) state.queueSelected = (int)state.queue.size() - 1;

    int16_t rowH = 24;
    int16_t y = kBodyY + 26;
    int16_t maxRows = (kBodyH - 26) / rowH;
    int16_t startIdx = 0;
    if (state.queueSelected >= maxRows) startIdx = state.queueSelected - maxRows + 1;

    for (int i = startIdx; i < (int)state.queue.size() && (i - startIdx) < maxRows; i++) {
        bool sel = i == state.queueSelected;
        bool grabbed = sel && state.queueGrabbed;
        uint16_t rowColor = grabbed ? 0xFD20 /*amber*/ : p.accent;
        if (sel) tftPtr->fillRect(0, y, kScreenW, rowH, rowColor);
        if (grabbed) {
            // Three-bar "grip" glyph in place of the index number, and a
            // dashed border, so a grabbed row for reordering reads
            // differently from a merely-selected one.
            tftPtr->drawRect(0, y, kScreenW, rowH, TFT_WHITE);
            for (int b = 0; b < 3; b++) tftPtr->drawFastHLine(10, y + 8 + b * 4, 6, TFT_WHITE);
        } else {
            tftPtr->setTextColor(sel ? TFT_WHITE : p.muted2, sel ? rowColor : p.bg);
            tftPtr->setCursor(12, y + 6);
            tftPtr->print(String(i + 1));
        }
        tftPtr->setTextColor(sel ? TFT_WHITE : p.fg, sel ? rowColor : p.bg);
        tftPtr->setCursor(34, y + 2);
        tftPtr->print(state.queue[i].title);
        tftPtr->setTextColor(sel ? 0xE73C : p.muted, sel ? rowColor : p.bg);
        tftPtr->setCursor(34, y + 12);
        tftPtr->print(state.queue[i].artist);
        y += rowH;
    }
}

} // namespace

void begin(TFT_eSPI &tft) { tftPtr = &tft; }

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
    }

    state.dirty = false;
}

} // namespace Screens
