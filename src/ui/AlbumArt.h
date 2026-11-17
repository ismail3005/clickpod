#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Decodes embedded FLAC cover art (FlacMeta::readPicture(), JPEG only --
// TJpg_Decoder doesn't handle PNG, nor progressive JPEG) into a small
// cached RGB565 buffer sized to the Now Playing art box, once per track.
// Screens.cpp just blits the cached buffer on every redraw instead of
// re-decoding a JPEG each time.
namespace AlbumArt {

constexpr int kSize = 92; // matches Screens.cpp's Now Playing art box (artSize)

void begin(TFT_eSPI &tft);

enum class LoadResult {
    Loaded,      // cached art is now this track's cover
    NoArt,       // the file has none this can use; the placeholder stays
    Interrupted, // the user did something meanwhile -- try again in a moment
};

// Loads + decodes the given track's embedded art, if any, into the cached
// buffer. Slow -- an SD read of the image plus a JPEG decode -- so it is
// called from MenuEngine::serviceDeferredWork() once the track is already
// playing, never from the track-start path itself. While it works it
// keeps the decoder fed and the buttons sampled, and it gives up at once
// (Interrupted) if the user presses or turns anything: a cover is never
// worth making an input wait.
LoadResult loadForTrack(const String &path);

// Forgets the cached art (the placeholder is drawn until the next load).
void clear();

// Draws the cached art if there is any, else a rounded placeholder square
// with fallbackGlyph centered in it.
void draw(int x, int y, int w, int h, char fallbackGlyph, uint16_t fallbackBg);

} // namespace AlbumArt
