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
// actually becomes Now Playing (see MenuEngine::setNowPlaying()).
//
// Every length field in here comes straight off the card, so every one of
// them is bounded against the block it sits in before it is used to
// allocate, read or loop -- a truncated or corrupt file gets "no
// metadata", never a huge allocation or a multi-billion-iteration loop.
namespace FlacMeta {

// STREAMINFO (always the first metadata block in a valid FLAC file) --
// gives exact duration without decoding anything.
struct StreamInfo {
    uint32_t sampleRate = 0;
    uint32_t totalSamples = 0;
    // 0 = not parsed/unknown. ESP32-audioI2S 3.0.12's FLAC decoder hard-
    // requires 8 or 16 -- a real file with 24-bit samples (common for
    // "hi-res" FLAC rips) gets rejected with "bits per sample must be 8
    // or 16, is 24" and the file just won't play, a separate, distinct
    // limitation from the maxFrameSize one. Checking this BEFORE even
    // attempting playback (MenuEngine::setNowPlaying()) lets that case
    // get skipped with a specific, useful log message instead of waiting
    // out the generic isRunning()-based failure-detection grace period
    // and a vaguer message.
    uint8_t bitsPerSample = 0;
};
inline float durationSec(const StreamInfo &si) {
    return si.sampleRate ? (float)si.totalSamples / (float)si.sampleRate : 0.0f;
}

// VORBIS_COMMENT block -- standard tag storage for FLAC. Only the first
// comment of each recognized key is kept if a file has duplicates.
struct Tags {
    String artist, title, album, lyrics;
    bool hasArtist = false, hasTitle = false, hasAlbum = false, hasLyrics = false;
};

// STREAMINFO and the tags in ONE pass over ONE open of the file (each SD
// open of a deeply-nested path costs several FAT directory lookups -- the
// dominant per-file cost on this card -- so this is done once per track
// start, not once per kind of metadata). Returns true if STREAMINFO was
// read (i.e. this really is a FLAC file); `tags` is filled in from the
// VORBIS_COMMENT block if there is one (check its has* flags).
bool readInfo(const String &path, StreamInfo &info, Tags &tags);

// First PICTURE block found (typically front cover art). Caller owns the
// returned buffer and must free() it. Returns false (outData untouched)
// if the file has no usable PICTURE block. A cover image is routinely
// hundreds of KB, i.e. a long blocking SD read -- `idle`, if given, is
// called between chunks (pass AudioBridge::pump so playback keeps being
// fed meanwhile).
bool readPicture(const String &path, uint8_t *&outData, size_t &outLen, String &outMime,
                  void (*idle)() = nullptr);

} // namespace FlacMeta
