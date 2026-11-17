#include "FlacMeta.h"

#include <SD.h>
#include <cstring>
#include <functional>
#include <memory>
#include <new>

namespace FlacMeta {
namespace {

uint32_t readBE32(File &f) {
    uint8_t b[4];
    if (f.read(b, 4) != 4) return 0;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

uint32_t readLE32(File &f) {
    uint8_t b[4];
    if (f.read(b, 4) != 4) return 0;
    return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
}

bool skip(File &f, uint32_t n) { return f.seek(f.position() + n); }

// Vorbis comments are "KEY=VALUE" ASCII/UTF-8 strings, length-prefixed
// (4-byte little-endian), preceded by a length-prefixed vendor string and
// a 4-byte little-endian comment count. Block layout per the Vorbis
// comment spec (reused as-is inside a FLAC VORBIS_COMMENT block).
//
// `count` and every length are untrusted: the loop is bounded by the
// block's own end, not by `count` (a corrupt count of 4 billion used to
// mean 4 billion iterations of failed reads -- an effective hang).
void parseVorbisComment(File &f, uint32_t blockLen, Tags &out) {
    const uint32_t blockEnd = f.position() + blockLen;
    auto remaining = [&]() -> uint32_t {
        uint32_t pos = f.position();
        return pos < blockEnd ? blockEnd - pos : 0;
    };

    if (remaining() < 8) return;
    uint32_t vendorLen = readLE32(f);
    if (vendorLen > remaining() || !skip(f, vendorLen)) return;
    if (remaining() < 4) return;
    uint32_t count = readLE32(f);

    for (uint32_t i = 0; i < count && remaining() >= 4; i++) {
        uint32_t clen = readLE32(f);
        if (clen > remaining()) return; // runs past the block: corrupt, stop here
        if (clen == 0) continue;
        if (clen > 16384) { // sanity cap -- lyrics can be a few KB, this is generous
            if (!skip(f, clen)) return;
            continue;
        }
        std::unique_ptr<char[]> buf(new (std::nothrow) char[clen + 1]);
        if (!buf) return;
        size_t got = f.read((uint8_t *)buf.get(), clen);
        buf[got] = 0;
        if (got != clen) return;
        char *eqPtr = (char *)memchr(buf.get(), '=', clen);
        if (!eqPtr) continue;
        *eqPtr = 0; // split in place: key | value
        String key(buf.get());
        key.toUpperCase();
        const char *val = eqPtr + 1;
        if (key == "ARTIST" && !out.hasArtist) { out.artist = val; out.hasArtist = true; }
        else if (key == "TITLE" && !out.hasTitle) { out.title = val; out.hasTitle = true; }
        else if (key == "ALBUM" && !out.hasAlbum) { out.album = val; out.hasAlbum = true; }
        else if ((key == "LYRICS" || key == "UNSYNCEDLYRICS") && !out.hasLyrics) {
            out.lyrics = val;
            out.hasLyrics = true;
        }
    }
}

// Some taggers prepend an ID3v2 tag to a FLAC file. Leaves `f` positioned
// just after the "fLaC" marker and returns true if this is a FLAC stream.
bool seekPastFlacMarker(File &f) {
    uint8_t head[10];
    if (f.read(head, 4) != 4) return false;
    if (memcmp(head, "ID3", 3) == 0) {
        if (f.read(head + 4, 6) != 6) return false;
        // 28-bit syncsafe size, excluding the 10-byte header (and a
        // 10-byte footer if flag bit 4 says there is one).
        uint32_t size = ((uint32_t)(head[6] & 0x7F) << 21) | ((uint32_t)(head[7] & 0x7F) << 14) |
                        ((uint32_t)(head[8] & 0x7F) << 7) | (uint32_t)(head[9] & 0x7F);
        if (head[5] & 0x10) size += 10;
        if (!f.seek(10 + size)) return false;
        if (f.read(head, 4) != 4) return false;
    }
    return memcmp(head, "fLaC", 4) == 0;
}

// Opens the file, finds the "fLaC" marker, and hands each metadata block
// to onBlock(file, type, blockLen) -- onBlock may consume some/all of the
// block's bytes itself (for the block it cares about); the walk always
// continues from the block's real end afterwards, whatever onBlock read.
// onBlock returns true to stop walking early (it has what it came for).
bool walkBlocks(const String &path, const std::function<bool(File &, uint8_t, uint32_t)> &onBlock) {
    File f = SD.open(path.c_str());
    if (!f) return false;

    if (!seekPastFlacMarker(f)) {
        f.close();
        return false;
    }

    const uint32_t fileSize = f.size();
    bool result = false;
    while (true) {
        uint8_t header[4];
        if (f.read(header, 4) != 4) break;
        bool isLast = header[0] & 0x80;
        uint8_t type = header[0] & 0x7F;
        uint32_t len = ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];

        uint32_t blockStart = f.position();
        if (len > fileSize - blockStart) break; // block claims to run past the end of the file

        if (onBlock(f, type, len)) { result = true; break; }
        if (isLast) break;
        if (!f.seek(blockStart + len)) break;
    }
    f.close();
    return result;
}

} // namespace

bool readInfo(const String &path, StreamInfo &info, Tags &tags) {
    bool gotInfo = false;
    walkBlocks(path, [&](File &f, uint8_t type, uint32_t len) {
        if (type == 0 && len >= 34 && !gotInfo) { // STREAMINFO is always block 0, first in the file
            uint8_t data[34];
            if (f.read(data, 34) != 34) return true; // truncated -- nothing further is readable either
            // STREAMINFO bit layout (see FLAC spec): after 10 bytes of
            // block-size/frame-size fields, an 8-byte packed field holds
            // sample_rate(20) | channels-1(3) | bits_per_sample-1(5) |
            // total_samples(36). sample_rate is data[10], data[11] and the
            // top nibble of data[12]; channels-1 is data[12]'s bits 3-1;
            // bits_per_sample-1 is data[12]'s bit 0 plus data[13]'s top
            // nibble; total_samples is the remaining 36 bits.
            info.sampleRate = ((uint32_t)data[10] << 12) | ((uint32_t)data[11] << 4) | (data[12] >> 4);
            uint8_t bitsPerSampleMinus1 = ((data[12] & 0x01) << 4) | (data[13] >> 4);
            info.bitsPerSample = bitsPerSampleMinus1 + 1;
            uint64_t totalSamples = ((uint64_t)(data[13] & 0x0F) << 32) | ((uint64_t)data[14] << 24) |
                                     ((uint64_t)data[15] << 16) | ((uint64_t)data[16] << 8) | data[17];
            info.totalSamples = totalSamples > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)totalSamples;
            gotInfo = true;
            return false;
        }
        if (type == 4) { // VORBIS_COMMENT -- the last thing wanted, stop after it
            parseVorbisComment(f, len, tags);
            return true;
        }
        return false;
    });
    return gotInfo;
}

bool readPicture(const String &path, uint8_t *&outData, size_t &outLen, String &outMime, bool (*idle)()) {
    return walkBlocks(path, [&](File &f, uint8_t type, uint32_t len) {
        if (type != 6) return false; // PICTURE
        // Fixed fields: type(4) mimeLen(4) .. descLen(4) w(4) h(4) depth(4)
        // colors(4) dataLen(4) = 32 bytes, plus the two variable strings.
        if (len < 32) return false;
        const uint32_t blockEnd = f.position() + len;
        readBE32(f); // picture type (e.g. 3 = front cover) -- not used
        uint32_t mimeLen = readBE32(f);
        if (mimeLen > 64) return false; // a real MIME type is a dozen bytes ("image/jpeg")
        char mime[65];
        if (f.read((uint8_t *)mime, mimeLen) != mimeLen) return false;
        mime[mimeLen] = 0;
        uint32_t descLen = readBE32(f);
        if (descLen > blockEnd - f.position() || !skip(f, descLen)) return false;
        if (blockEnd - f.position() < 20) return false;
        readBE32(f); // width -- not used, decoder reports real dimensions
        readBE32(f); // height
        readBE32(f); // color depth
        readBE32(f); // colors used (indexed images only)
        uint32_t dataLen = readBE32(f);
        if (dataLen == 0 || dataLen > blockEnd - f.position()) return false;
        if (dataLen > 2 * 1024 * 1024) return false; // sanity cap: 2MB embedded art would be unusual

        // PSRAM, not internal RAM -- pure JPEG byte data, tens to hundreds
        // of KB, alive only until AlbumArt::loadForTrack() has decoded it.
        uint8_t *buf = (uint8_t *)ps_malloc(dataLen);
        if (!buf) return false;
        // In chunks, not one read -- see the header comment on `idle`.
        constexpr uint32_t kChunk = 16384;
        uint32_t got = 0;
        while (got < dataLen) {
            uint32_t want = min(kChunk, dataLen - got);
            if (f.read(buf + got, want) != want) { free(buf); return false; }
            got += want;
            if (idle && idle()) { free(buf); return false; } // abandoned
        }

        outData = buf;
        outLen = dataLen;
        outMime = mime;
        return true;
    });
}

} // namespace FlacMeta
