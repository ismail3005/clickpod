#include "FlacMeta.h"

#include <SD.h>
#include <cstring>
#include <functional>
#include <memory>

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
void parseVorbisComment(File &f, Tags &out) {
    uint32_t vendorLen = readLE32(f);
    skip(f, vendorLen);
    uint32_t count = readLE32(f);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t clen = readLE32(f);
        if (clen == 0 || clen > 16384) { // sanity cap -- lyrics can be a few KB, this is generous
            skip(f, clen);
            continue;
        }
        std::unique_ptr<char[]> buf(new char[clen + 1]);
        size_t got = f.read((uint8_t *)buf.get(), clen);
        buf[got] = 0;
        String comment(buf.get());
        int eq = comment.indexOf('=');
        if (eq < 0) continue;
        String key = comment.substring(0, eq);
        key.toUpperCase();
        String val = comment.substring(eq + 1);
        if (key == "ARTIST" && !out.hasArtist) { out.artist = val; out.hasArtist = true; }
        else if (key == "TITLE" && !out.hasTitle) { out.title = val; out.hasTitle = true; }
        else if (key == "ALBUM" && !out.hasAlbum) { out.album = val; out.hasAlbum = true; }
        else if ((key == "LYRICS" || key == "UNSYNCEDLYRICS") && !out.hasLyrics) {
            out.lyrics = val;
            out.hasLyrics = true;
        }
    }
}

// Opens the file, checks the "fLaC" magic, and hands each metadata block
// to onBlock(type, blockLen) -- onBlock may consume some/all of the
// block's bytes itself (for the block it cares about); whatever it
// doesn't consume gets skipped automatically before moving to the next
// block. onBlock returns true to stop walking early (block found).
bool walkBlocks(const String &path, const std::function<bool(File &, uint8_t, uint32_t)> &onBlock) {
    File f = SD.open(path.c_str());
    if (!f) return false;

    char magic[4];
    if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "fLaC", 4) != 0) {
        f.close();
        return false;
    }

    bool result = false;
    while (true) {
        uint8_t header[4];
        if (f.read(header, 4) != 4) break;
        bool isLast = header[0] & 0x80;
        uint8_t type = header[0] & 0x7F;
        uint32_t len = ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];

        size_t before = f.position();
        bool stop = onBlock(f, type, len);
        size_t consumed = f.position() - before;
        if (consumed < len) skip(f, len - consumed);

        if (stop) { result = true; break; }
        if (isLast) break;
    }
    f.close();
    return result;
}

} // namespace

bool readStreamInfo(const String &path, StreamInfo &out) {
    return walkBlocks(path, [&](File &f, uint8_t type, uint32_t len) {
        if (type != 0 || len < 34) return false; // STREAMINFO is always block 0, first in the file
        uint8_t data[34];
        if (f.read(data, 34) != 34) return false;
        // STREAMINFO bit layout (see FLAC spec): after 18 bytes of block-
        // size/frame-size fields, an 8-byte packed field holds
        // sample_rate(20) | channels-1(3) | bits_per_sample-1(5) |
        // total_samples(36).
        out.sampleRate = ((uint32_t)data[10] << 12) | ((uint32_t)data[11] << 4) | (data[12] >> 4);
        uint64_t totalSamples = ((uint64_t)(data[13] & 0x0F) << 32) | ((uint64_t)data[14] << 24) |
                                 ((uint64_t)data[15] << 16) | ((uint64_t)data[16] << 8) | data[17];
        out.totalSamples = (uint32_t)totalSamples;
        return true;
    });
}

bool readTags(const String &path, Tags &out) {
    return walkBlocks(path, [&](File &f, uint8_t type, uint32_t) {
        if (type != 4) return false; // VORBIS_COMMENT
        parseVorbisComment(f, out);
        return true;
    });
}

bool readPicture(const String &path, uint8_t *&outData, size_t &outLen, String &outMime) {
    return walkBlocks(path, [&](File &f, uint8_t type, uint32_t) {
        if (type != 6) return false; // PICTURE
        readBE32(f);                 // picture type (e.g. 3 = front cover) -- not used
        uint32_t mimeLen = readBE32(f);
        std::unique_ptr<char[]> mimeBuf(new char[mimeLen + 1]);
        size_t gotMime = f.read((uint8_t *)mimeBuf.get(), mimeLen);
        mimeBuf[gotMime] = 0;
        uint32_t descLen = readBE32(f);
        skip(f, descLen);
        readBE32(f); // width -- not used, decoder reports real dimensions
        readBE32(f); // height
        readBE32(f); // color depth
        readBE32(f); // colors used (indexed images only)
        uint32_t dataLen = readBE32(f);
        if (dataLen == 0 || dataLen > 2 * 1024 * 1024) return false; // sanity cap: 2MB embedded art would be unusual

        uint8_t *buf = (uint8_t *)malloc(dataLen);
        if (!buf) return false;
        size_t got = f.read(buf, dataLen);
        if (got != dataLen) { free(buf); return false; }

        outData = buf;
        outLen = dataLen;
        outMime = mimeBuf.get();
        return true;
    });
}

} // namespace FlacMeta
