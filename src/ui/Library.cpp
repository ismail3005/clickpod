#include "Library.h"

#include "Util.h"

namespace Library {
namespace {

// Kept as a fallback for bench-testing without an SD card inserted, or a
// card ensureIndex() finds nothing playable on.
std::vector<LibraryAlbum> MOCK_ALBUMS = {
    {"Coral Static", "Nightbus Radio", '\x01', {
        {"", "", "Nightbus Radio", 214},
        {"", "", "Halide", 183},
        {"", "", "Low Tide Fever", 247},
    }},
    {"Coral Static", "Halide EP", '\x01', {
        {"", "", "Halide (Slow Version)", 201},
        {"", "", "Copper Wire", 166},
    }},
    {"Marrow Isle", "Salt & Static", '\x01', {
        {"", "", "Wire to Wire", 198},
        {"", "", "Salt & Static", 231},
        {"", "", "Ferry Song", 176},
        {"", "", "Departures", 264},
    }},
    {"Ninth Hour", "Late Signal", '\x01', {
        {"", "", "Late Signal", 209},
        {"", "", "Runoff", 188},
    }},
};

// Top-level SD folders listed here have an Artist/Album/track.flac tree
// like everywhere else, but their contents become ONE named playlist
// instead of separate browsable Artist/Album entries in Music -- keeps a
// playlist folder that duplicates albums also downloaded separately from
// showing those albums twice. Match is case-insensitive. Add more names
// here as more playlist folders get added to the card.
bool isPlaylistFolderName(const String &name) {
    String lower = name;
    lower.toLowerCase();
    return lower == "funky times";
}

String stripExtension(const String &filename) {
    int dot = filename.lastIndexOf('.');
    return dot > 0 ? filename.substring(0, dot) : filename;
}

// ---- On-SD compact index (/clickpod.idx) --------------------------------
//
// Format: 4-byte magic "CPX1" (also a version tag -- a future format
// change bumps this so a stale index from an old build is never
// misread), 4-byte LE track count (informational only, see forEachRecord
// below -- readers don't trust it for the loop bound, EOF does that),
// then that many variable-length records:
//   kind: 1 byte (0 = Music track, 1 = Playlist track)
//   [playlistName]   -- only present if kind == 1
//   artist, album, title, path -- each a 2-byte LE length + that many
//   raw bytes (not null-terminated on disk)
// uint16 lengths (not uint8) specifically because a full nested SD path
// (playlist/artist/album/filename) can plausibly exceed 255 bytes with
// real long filenames, even though no single field name usually would --
// cheap insurance (1 extra byte/field) against silent truncation.

constexpr const char *kIndexPath = "/clickpod.idx";
constexpr size_t kMaxFieldLen = 300; // defensive cap, see writeStr()

bool indexReady = false;

// name -> tracks added this session via addToPlaylist() -- never written
// to the index file, see the header comment on addToPlaylist().
std::map<String, std::vector<Track>> extraPlaylistTracks;

void writeStr(File &f, const String &s) {
    uint16_t len = (uint16_t)min((int)s.length(), (int)kMaxFieldLen);
    f.write((const uint8_t *)&len, 2);
    if (len > 0) f.write((const uint8_t *)s.c_str(), len);
}

bool readStr(File &f, String &out) {
    uint16_t len;
    if (f.read((uint8_t *)&len, 2) != 2) return false;
    char buf[kMaxFieldLen + 1];
    if (len > 0) {
        if (f.read((uint8_t *)buf, len) != len) return false;
    }
    buf[len] = '\0';
    out = String(buf);
    return true;
}

void writeMusicRecord(File &idx, const String &artist, const String &album,
                       const String &title, const String &path) {
    uint8_t kind = 0;
    idx.write(&kind, 1);
    writeStr(idx, artist);
    writeStr(idx, album);
    writeStr(idx, title);
    writeStr(idx, path);
}

void writePlaylistRecord(File &idx, const String &playlistName, const String &artist,
                          const String &album, const String &title, const String &path) {
    uint8_t kind = 1;
    idx.write(&kind, 1);
    writeStr(idx, playlistName);
    writeStr(idx, artist);
    writeStr(idx, album);
    writeStr(idx, title);
    writeStr(idx, path);
}

void indexAlbumFolder(File albumDir, const String &albumPath, const String &artistName,
                       const String &albumName, File &idx, uint32_t &count) {
    while (File entry = albumDir.openNextFile()) {
        if (!entry.isDirectory()) {
            String fname = entry.name();
            if (hasAudioExtension(fname)) {
                writeMusicRecord(idx, artistName, albumName, stripExtension(fname), albumPath + "/" + fname);
                count++;
            }
        }
        entry.close();
    }
}

void indexArtistFolder(File artistDir, const String &artistPath, const String &artistName,
                        File &idx, uint32_t &count) {
    while (File entry = artistDir.openNextFile()) {
        if (entry.isDirectory()) {
            String albumName = entry.name();
            indexAlbumFolder(entry, artistPath + "/" + albumName, artistName, albumName, idx, count);
            // Same watchdog/progress reasoning as the old scanArtistFolder --
            // a big library shouldn't starve the task watchdog into a reboot.
            yield();
            if (count % 50 == 0) Serial.printf("[library] indexing... %u tracks so far\n", (unsigned)count);
        }
        entry.close();
    }
}

void indexPlaylistFolder(File plDir, const String &plPath, const String &plName,
                          File &idx, uint32_t &count) {
    while (File artistEntry = plDir.openNextFile()) {
        if (artistEntry.isDirectory()) {
            String artistName = artistEntry.name();
            String artistPath = plPath + "/" + artistName;
            while (File albumEntry = artistEntry.openNextFile()) {
                if (albumEntry.isDirectory()) {
                    String albumName = albumEntry.name();
                    String albumPath = artistPath + "/" + albumName;
                    while (File fileEntry = albumEntry.openNextFile()) {
                        if (!fileEntry.isDirectory()) {
                            String fname = fileEntry.name();
                            if (hasAudioExtension(fname)) {
                                writePlaylistRecord(idx, plName, artistName, albumName,
                                                     stripExtension(fname), albumPath + "/" + fname);
                                count++;
                            }
                        }
                        fileEntry.close();
                    }
                }
                albumEntry.close();
                // Deepest-nested loop (root->playlist->artist->album->file),
                // same reasoning as indexArtistFolder above.
                yield();
                if (count % 25 == 0 && count > 0) {
                    Serial.printf("[library] indexing playlist \"%s\"... %u tracks so far\n",
                                  plName.c_str(), (unsigned)count);
                }
            }
        }
        artistEntry.close();
    }
}

bool buildIndexFile() {
    File root = SD.open("/");
    if (!root) return false;

    if (SD.exists(kIndexPath)) SD.remove(kIndexPath);
    File idx = SD.open(kIndexPath, FILE_WRITE);
    if (!idx) {
        root.close();
        return false;
    }

    const uint8_t magic[4] = {'C', 'P', 'X', '1'};
    idx.write(magic, 4);
    uint32_t countPlaceholder = 0;
    idx.write((const uint8_t *)&countPlaceholder, 4);

    uint32_t count = 0;
    while (File entry = root.openNextFile()) {
        if (entry.isDirectory()) {
            String name = entry.name();
            String path = String("/") + name;
            if (isPlaylistFolderName(name)) {
                indexPlaylistFolder(entry, path, name, idx, count);
            } else {
                indexArtistFolder(entry, path, name, idx, count);
            }
        }
        entry.close();
    }
    root.close();

    // Real count written back over the placeholder now that it's known --
    // informational for the log line below; readers rely on EOF, not this
    // value, to know when to stop (see forEachRecord()).
    idx.seek(4);
    idx.write((const uint8_t *)&count, 4);
    idx.close();

    Serial.printf("[library] built index: %u tracks\n", (unsigned)count);
    return count > 0;
}

struct IndexRecord {
    bool isPlaylist = false;
    String playlistName;
    String artist, album, title, path;
};

bool readRecord(File &f, IndexRecord &r) {
    uint8_t kind;
    if (f.read(&kind, 1) != 1) return false;
    r.isPlaylist = (kind == 1);
    if (r.isPlaylist && !readStr(f, r.playlistName)) return false;
    if (!readStr(f, r.artist)) return false;
    if (!readStr(f, r.album)) return false;
    if (!readStr(f, r.title)) return false;
    if (!readStr(f, r.path)) return false;
    return true;
}

// Streams every record in the index file to fn(), one at a time -- never
// materializes the whole index in RAM, just whatever fn() itself chooses
// to keep. This is the one place that actually touches the index file;
// every indexArtists()/indexAlbumsForArtist()/etc. below is a thin filter
// on top of it. EOF (readRecord() returning false) ends the loop, not the
// header's track count -- robust even if that count is ever wrong/stale.
template <typename Fn>
void forEachRecord(Fn fn) {
    File f = SD.open(kIndexPath, FILE_READ);
    if (!f) return;
    uint8_t header[8];
    if (f.read(header, 8) != 8) {
        f.close();
        return;
    }
    IndexRecord r;
    while (readRecord(f, r)) {
        fn(r);
        yield();
    }
    f.close();
}

} // namespace

std::vector<LibraryAlbum> ALBUMS = MOCK_ALBUMS;

Track trackAt(size_t albumIndex, size_t trackIndex) {
    const LibraryAlbum &al = ALBUMS[albumIndex];
    const Track &t = al.tracks[trackIndex];
    return Track{al.artist, al.album, t.title, t.durSec, al.art, t.path};
}

String keyFor(const Track &t) { return t.artist + "|" + t.album + "|" + t.title; }

std::vector<Playlist> PLAYLISTS = {
    {"Late Drive", {trackAt(0, 0), trackAt(2, 1), trackAt(3, 0), trackAt(1, 0)}},
    {"Rainy Afternoon", {trackAt(0, 2), trackAt(2, 3), trackAt(3, 1)}},
};

std::map<String, std::vector<LyricLine>> LYRICS = {
    {"Coral Static|Nightbus Radio|Nightbus Radio", {
        {0, "[instrumental intro]"},
        {8, "Riding the last bus out of town"},
        {13, "Streetlights blur as the engine slows down"},
        {19, "I left the porch light on for no one"},
        {25, "Static on the radio, nowhere to run"},
        {34, "Nightbus radio, take me home"},
        {40, "Nightbus radio, I'm not alone"},
        {52, "The driver hums a tune I half-know"},
        {58, "Windows fogged up from the cold"},
    }},
};

bool usingIndex() { return indexReady; }

bool ensureIndex(bool force) {
    if (!force && SD.exists(kIndexPath)) {
        // Trust an existing index rather than re-scanning -- this is the
        // whole point (the slow FAT walk only needs to happen once, not
        // every boot). No auto-detection of SD content changes; see
        // "Rescan library" in Settings for the manual way to force this.
        Serial.println(F("[library] using existing on-SD index (no rescan)"));
        indexReady = true;
        return true;
    }

    Serial.println(force ? F("[library] rescanning SD (manual request)...")
                          : F("[library] building on-SD index (first boot)..."));
    indexReady = buildIndexFile();
    if (!indexReady) {
        Serial.println(F("[library] index build found nothing playable -- keeping placeholder library"));
    } else {
        // Ephemeral "Add to Playlist" additions from before a rescan refer
        // to a library that may no longer match what's on the card --
        // drop them rather than risk them pointing at stale paths.
        extraPlaylistTracks.clear();
    }
    return indexReady;
}

std::vector<String> indexArtists() {
    std::vector<String> out;
    forEachRecord([&](const IndexRecord &r) {
        if (r.isPlaylist) return;
        for (auto &a : out) if (a == r.artist) return;
        out.push_back(r.artist);
    });
    return out;
}

std::vector<std::pair<String, int>> indexAlbumsForArtist(const String &artist) {
    std::vector<std::pair<String, int>> out;
    forEachRecord([&](const IndexRecord &r) {
        if (r.isPlaylist || r.artist != artist) return;
        for (auto &p : out) {
            if (p.first == r.album) { p.second++; return; }
        }
        out.push_back({r.album, 1});
    });
    return out;
}

std::vector<Track> indexTracksForAlbum(const String &artist, const String &album) {
    std::vector<Track> out;
    forEachRecord([&](const IndexRecord &r) {
        if (r.isPlaylist || r.artist != artist || r.album != album) return;
        Track t;
        t.artist = artist;
        t.album = album;
        t.title = r.title;
        t.path = r.path;
        out.push_back(std::move(t));
    });
    return out;
}

std::vector<std::pair<String, int>> indexPlaylists() {
    std::vector<std::pair<String, int>> out;
    forEachRecord([&](const IndexRecord &r) {
        if (!r.isPlaylist) return;
        for (auto &p : out) {
            if (p.first == r.playlistName) { p.second++; return; }
        }
        out.push_back({r.playlistName, 1});
    });
    for (auto &kv : extraPlaylistTracks) {
        if (kv.second.empty()) continue;
        bool found = false;
        for (auto &p : out) {
            if (p.first == kv.first) { p.second += (int)kv.second.size(); found = true; break; }
        }
        if (!found) out.push_back({kv.first, (int)kv.second.size()});
    }
    return out;
}

std::vector<Track> indexTracksForPlaylist(const String &name) {
    std::vector<Track> out;
    forEachRecord([&](const IndexRecord &r) {
        if (!r.isPlaylist || r.playlistName != name) return;
        Track t;
        t.artist = r.artist;
        t.album = r.album;
        t.title = r.title;
        t.path = r.path;
        out.push_back(std::move(t));
    });
    auto it = extraPlaylistTracks.find(name);
    if (it != extraPlaylistTracks.end()) {
        for (auto &t : it->second) out.push_back(t);
    }
    return out;
}

void addToPlaylist(const String &playlistName, const Track &t) {
    extraPlaylistTracks[playlistName].push_back(t);
}

String nextNewPlaylistName() {
    for (int n = 1;; n++) {
        String candidate = "New Playlist " + String(n);
        bool taken = false;
        if (usingIndex()) {
            for (auto &kv : indexPlaylists()) {
                if (kv.first == candidate) { taken = true; break; }
            }
        } else {
            for (auto &p : PLAYLISTS) {
                if (p.name == candidate) { taken = true; break; }
            }
        }
        if (!taken) return candidate;
    }
}

void logFailedFile(const String &path, const String &reason) {
    if (SD.cardType() == CARD_NONE) return;
    File f = SD.open("/clickpod_failed.txt", FILE_APPEND);
    if (!f) return;
    f.print(path);
    f.print(" -- ");
    f.println(reason);
    f.close();
}

} // namespace Library
