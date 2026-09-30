#include "AlbumArt.h"

#include <TJpg_Decoder.h>
#include <cstring>

#include "../audio/FlacMeta.h"

namespace AlbumArt {
namespace {

TFT_eSPI *tftPtr = nullptr;
uint16_t *artBuf = nullptr; // kSize*kSize RGB565, allocated once in begin()
bool artValid = false;
int decodeOffsetX = 0, decodeOffsetY = 0; // centers a non-square decode within the square buffer

// TJpg_Decoder calls this per decoded block; write into our cached buffer
// instead of straight to the display, so drawing later is a cheap blit
// (see AlbumArt.h for why re-decoding per redraw would be wasteful).
bool tjpgCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (!artBuf) return false;
    for (uint16_t row = 0; row < h; row++) {
        int ty = y + row + decodeOffsetY;
        if (ty < 0 || ty >= kSize) continue;
        for (uint16_t col = 0; col < w; col++) {
            int tx = x + col + decodeOffsetX;
            if (tx < 0 || tx >= kSize) continue;
            artBuf[ty * kSize + tx] = bitmap[row * w + col];
        }
    }
    return true;
}

} // namespace

void begin(TFT_eSPI &tft) {
    tftPtr = &tft;
    artBuf = (uint16_t *)malloc((size_t)kSize * kSize * sizeof(uint16_t));
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tjpgCallback);
}

void clear() { artValid = false; }
bool hasArt() { return artValid; }

bool loadForTrack(const String &path) {
    artValid = false;
    if (!artBuf) return false;

    uint8_t *jpgData = nullptr;
    size_t jpgLen = 0;
    String mime;
    if (!FlacMeta::readPicture(path, jpgData, jpgLen, mime)) return false;

    bool isJpeg = mime.indexOf("jpeg") >= 0 || mime.indexOf("jpg") >= 0;
    if (!isJpeg) {
        free(jpgData);
        return false;
    }

    uint16_t w = 0, h = 0;
    if (TJpgDec.getJpgSize(&w, &h, jpgData, jpgLen) != JDR_OK || w == 0 || h == 0) {
        free(jpgData);
        return false;
    }

    // Decode at the smallest power-of-2 downscale (1/2/4/8, TJpg_Decoder's
    // only supported factors) that still leaves us at least kSize on each
    // side -- avoids decoding a full 500px+ embedded image just to shrink
    // it back down to a 92px box.
    uint8_t scale = 1;
    while (scale < 8 && (w / (scale * 2)) >= kSize && (h / (scale * 2)) >= kSize) scale *= 2;
    TJpgDec.setJpgScale(scale);
    uint16_t sw = w / scale, sh = h / scale;
    decodeOffsetX = (kSize - (int)sw) / 2;
    decodeOffsetY = (kSize - (int)sh) / 2;
    memset(artBuf, 0, (size_t)kSize * kSize * sizeof(uint16_t));

    JRESULT r = TJpgDec.drawJpg(0, 0, jpgData, jpgLen); // x/y unused by our buffer-writing callback
    free(jpgData);

    artValid = (r == JDR_OK);
    return artValid;
}

void draw(int x, int y, int w, int h, char fallbackGlyph, uint16_t fallbackBg) {
    if (!tftPtr) return;
    if (artValid && artBuf) {
        tftPtr->pushImage(x, y, kSize, kSize, artBuf);
    } else {
        tftPtr->fillRoundRect(x, y, w, h, 8, fallbackBg);
        tftPtr->setTextColor(TFT_WHITE, fallbackBg);
        tftPtr->setTextSize(2);
        tftPtr->setCursor(x + w / 2 - 6, y + h / 2 - 8);
        tftPtr->print(String(fallbackGlyph));
    }
}

} // namespace AlbumArt
