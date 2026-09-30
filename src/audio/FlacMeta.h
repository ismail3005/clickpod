#pragma once

#include <Arduino.h>

// Reads FLAC metadata blocks directly off the SD card -- the FLAC format
// itself is a fixed, openly-documented binary spec (unlike a third-party
// library's API, there's no "which version" uncertainty here), so this is
// implemented by hand rather than depending on ESP32-audioI2S to expose
// it (spec section 10's open question: it may not).
//
// Deliberately NOT run during Library::ensureIndex()'s bulk directory
// walk -- reading/parsing tags (and especially embedded lyrics, which can
// be several KB of text) for every file up front would both slow the
// scan down further and risk holding a lot of string data in RAM for
// tracks that are never played. Called lazily, once, when a track
// actually becomes Now Playing (see MenuEngine::setNowPlaying()) --
// readStreamInfo() is the exception, cheap enough (one small fixed-size
// read) to consider calling eagerly later if duration-in-track-lists
// turns out to be worth the extra scan time.
namespace FlacMeta {

// STREAMINFO (always the first metadata block in a valid FLAC file) --
// gives exact duration without decoding anything.
struct StreamInfo {
    uint32_t sampleRate = 0;
    uint32_t totalSamples = 0;
};
bool readStreamInfo(const String &path, StreamInfo &out);
inline float durationSec(const StreamInfo &si) {
    return si.sampleRate ? (float)si.totalSamples / (float)si.sampleRate : 0.0f;
}

// VORBIS_COMMENT block -- standard tag storage for FLAC. Only the first
// comment of each recognized key is kept if a file has duplicates.
struct Tags {
    String artist, title, album, lyrics;
    bool hasArtist = false, hasTitle = false, hasAlbum = false, hasLyrics = false;
};
bool readTags(const String &path, Tags &out);

// First PICTURE block found (typically front cover art). Caller owns the
// returned buffer and must free() it. Returns false (outData untouched)
// if the file has no PICTURE block.
bool readPicture(const String &path, uint8_t *&outData, size_t &outLen, String &outMime);

} // namespace FlacMeta
