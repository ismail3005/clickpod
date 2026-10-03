#include "AlbumArt.h"

#include <TJpg_Decoder.h>
#include <cstring>

#include "../audio/FlacMeta.h"

namespace AlbumArt {
namespace {

TFT_eSPI *tftPtr = nullptr;
uint16_t *artBuf = nullptr; // kSize*kSize RGB565, allocated once in begin()
bool artValid = false;

// Scratch full-size decode target for the CURRENT load only -- allocated
// fresh per track (its size varies per file, unlike artBuf) and freed
// again before loadForTrack() returns. Previously the TJpg callback
// wrote straight into artBuf with a centering offset, i.e. a center-CROP
// of whatever didn't fit -- see CLAUDE.md's "Album art is routinely
// cropped awkwardly" writeup for why that looked wrong on most real
// files (TJpg_Decoder's 1/2/4/8x-only scaling almost never lands exactly
// on kSize, so the pre-crop decode is usually noticeably bigger than the
// box, and cropping it throws away real image content instead of
// shrinking it). Decoding into this full, uncropped buffer first is what
// lets loadForTrack() do a real resize afterward instead.
uint16_t *decodeBuf = nullptr;
uint16_t decodeW = 0, decodeH = 0;

bool tjpgCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (!decodeBuf) return false;
    for (uint16_t row = 0; row < h; row++) {
        int ty = y + row;
        if (ty < 0 || ty >= decodeH) continue;
        for (uint16_t col = 0; col < w; col++) {
            int tx = x + col;
            if (tx < 0 || tx >= decodeW) continue;
            decodeBuf[ty * decodeW + tx] = bitmap[row * w + col];
        }
    }
    return true;
}

} // namespace

void begin(TFT_eSPI &tft) {
    tftPtr = &tft;
    // ps_malloc(), not malloc() -- this buffer (~17KB: kSize*kSize*2 bytes)
    // sat in scarce internal RAM for the whole session for no reason. It's
    // pure pixel data (TJpg_Decoder's callback writes into it, TFT_eSPI's
    // plain pushImage() just reads it back over SPI -- no DMA requirement
    // on the source buffer itself), so PSRAM is exactly where this
    // belongs. ps_malloc() is a real Arduino-ESP32 core function (PSRAM-
    // backed malloc, falls back to regular RAM if PSRAM isn't available)
    // -- reclaims real internal-heap headroom for WiFi/BT (see RadioLock.h)
    // at zero cost, since nothing about this buffer needs to be internal.
    artBuf = (uint16_t *)ps_malloc((size_t)kSize * kSize * sizeof(uint16_t));
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
    // it back down to a 92px box. The result is the smallest decode
    // TJpg_Decoder can give us that's still big enough to resize DOWN
    // from (never up) -- usually still noticeably bigger than kSize on
    // one or both axes, which is exactly why a real resize step below
    // matters instead of just blitting/cropping this directly.
    uint8_t scale = 1;
    while (scale < 8 && (w / (scale * 2)) >= kSize && (h / (scale * 2)) >= kSize) scale *= 2;
    TJpgDec.setJpgScale(scale);
    decodeW = w / scale;
    decodeH = h / scale;

    decodeBuf = (uint16_t *)ps_malloc((size_t)decodeW * decodeH * sizeof(uint16_t));
    if (!decodeBuf) {
        free(jpgData);
        return false;
    }
    memset(decodeBuf, 0, (size_t)decodeW * decodeH * sizeof(uint16_t));

    JRESULT r = TJpgDec.drawJpg(0, 0, jpgData, jpgLen); // x/y unused by our buffer-writing callback
    free(jpgData);

    if (r != JDR_OK) {
        free(decodeBuf);
        decodeBuf = nullptr;
        return false;
    }

    // Real resize, not a crop: fit decodeW x decodeH into the kSize x
    // kSize box preserving aspect ratio (never stretching -- that would
    // visibly distort non-square art), centered with letterbox padding
    // on whichever axis has slack. Nearest-neighbor (not a box/bilinear
    // filter) -- cheap, and the source is already close to kSize after
    // the scale selection above, so there's little to gain from a more
    // expensive filter at this size.
    int scaledW, scaledH;
    if (decodeW >= decodeH) {
        scaledW = kSize;
        scaledH = (int)((long)decodeH * kSize / decodeW);
    } else {
        scaledH = kSize;
        scaledW = (int)((long)decodeW * kSize / decodeH);
    }
    if (scaledW < 1) scaledW = 1;
    if (scaledH < 1) scaledH = 1;
    int offX = (kSize - scaledW) / 2;
    int offY = (kSize - scaledH) / 2;

    memset(artBuf, 0, (size_t)kSize * kSize * sizeof(uint16_t));
    for (int ty = 0; ty < scaledH; ty++) {
        int sy = (int)((long)ty * decodeH / scaledH);
        if (sy >= decodeH) sy = decodeH - 1;
        for (int tx = 0; tx < scaledW; tx++) {
            int sx = (int)((long)tx * decodeW / scaledW);
            if (sx >= decodeW) sx = decodeW - 1;
            artBuf[(ty + offY) * kSize + (tx + offX)] = decodeBuf[sy * decodeW + sx];
        }
    }

    free(decodeBuf);
    decodeBuf = nullptr;
    artValid = true;
    return true;
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
