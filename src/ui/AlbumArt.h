#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Decodes embedded FLAC cover art (FlacMeta::readPicture(), JPEG only --
// TJpg_Decoder doesn't handle PNG) into a small cached RGB565 buffer sized
// to the Now Playing art box, once per track. Screens.cpp's drawNowPlaying()
// just blits the cached buffer on every redraw instead of re-decoding a
// JPEG every time the screen redraws (play/pause, volume change, returning
// from another screen, ...), which would be a real performance hit on top
// of everything else this UI already redraws more than strictly needed.
//
// Least-verified piece of the FLAC/art work -- TJpg_Decoder's exact API
// (setCallback's signature, drawJpg's return type/constants) is written
// from general knowledge of this library, not checked against its actual
// installed header. If this doesn't compile or doesn't decode correctly,
// start here.
namespace AlbumArt {

constexpr int kSize = 92; // matches Screens.cpp's Now Playing art box (artSize)

void begin(TFT_eSPI &tft);

// Loads + decodes the given track's embedded art, if any, into the cached
// buffer. Call once when a track becomes Now Playing (MenuEngine::
// setNowPlaying()), not on every redraw. Clears any previously cached art
// first, so a track with no art (or non-JPEG art) correctly falls back to
// the placeholder instead of showing the previous track's picture.
bool loadForTrack(const String &path);

void clear();
bool hasArt();

// Draws the cached art if hasArt(), else a rounded placeholder square with
// fallbackGlyph centered in it (the pre-existing behavior).
void draw(int x, int y, int w, int h, char fallbackGlyph, uint16_t fallbackBg);

} // namespace AlbumArt
