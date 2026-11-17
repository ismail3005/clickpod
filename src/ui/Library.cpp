#include "Library.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

#include "../audio/AudioBridge.h"
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

// User-reported bug: many ripped/downloaded files are named
// "Artist - Title.ext" even though they already live under that exact
// artist's own folder. Shown as-is (just the filename minus extension),
// that duplicates the artist name -- once baked into the title text,
// once in the row's own separate artist sub-label -- and for anything
// with a long artist name (their example: "Red Hot Chili Peppers - "
// eating the whole row before the real title even starts) the actual
// title never becomes visible at all. Strips exactly that redundant
// "<artist> - " prefix when it matches the track's own known artist
// (the folder name, case-insensitive) -- leaves anything else (a
// differently-spelled embedded name, a feature/collab credit, no
// prefix at all) completely untouched, rather than guessing at a
// generic "any text before the first ' - '" rule that could eat real
// title text which happens to contain " - " for an unrelated reason.
String cleanTrackTitle(const String &filename, const String &artistName) {
    String title = stripExtension(filename);
    String prefix = artistName + " - ";
    if ((int)title.length() > (int)prefix.length() &&
        title.substring(0, prefix.length()).equalsIgnoreCase(prefix)) {
        return title.substring(prefix.length());
    }
    return title;
}

// ---- On-SD compact index (/clickpod.idx) --------------------------------
//
// Format: 4-byte magic "CPX2" (also a version tag -- a format or content
// change bumps this so an index from an older build is rebuilt, never
// misread), 4-byte LE record count, then that many variable-length
// records:
//   kind: 1 byte (0 = Music track, 1 = Playlist track)
//   [playlistName]   -- only present if kind == 1
//   artist, album, title, path -- each a 2-byte LE length + that many
//   raw bytes (not null-terminated on disk)
// uint16 lengths (not uint8) specifically because a full nested SD path
// (playlist/artist/album/filename) can plausibly exceed 255 bytes with
// real long filenames, even though no single field name usually would.
//
// Records are written in sorted order (artists, albums, and tracks within
// an album, each by name -- FAT hands directory entries back in creation
// order, which is how "07 ..." could end up listed before "01 ..."), and
// all records of one album / one playlist are contiguous.
//
// "CPX1" -> "CPX2": sorted order, the redundant "Artist - " title prefix
// stripped (cleanTrackTitle()), and the file is now validated on load
// instead of trusted blindly -- see loadCatalog().

constexpr const char *kIndexPath = "/clickpod.idx";
constexpr const char *kIndexTmpPath = "/clickpod.tmp";
constexpr uint8_t kIndexMagic[4] = {'C', 'P', 'X', '2'};
constexpr size_t kMaxFieldLen = 300; // on-disk cap per string field, enforced on write AND on read

bool indexReady = false;

// ---- In-RAM catalog ------------------------------------------------------
//
// Names + counts + where each album's / playlist's records start in the
// index file -- NOT the tracks themselves. Built by one sequential pass
// over the index at boot (loadCatalog()). Every list screen above the
// track level (artists, albums, playlists) is then answered from RAM, and
// opening an album or playlist seeks straight to its records instead of
// re-reading the whole index from SD on every menu open, which is what
// each of these used to do (the Playlists screen did it once per playlist
// on top). A few dozen bytes per album, in PSRAM.
struct CatalogAlbum {
    String artist;
    String album;
    uint32_t offset; // file offset of this album's first record
    uint16_t count;
};
struct CatalogPlaylist {
    String name;
    uint32_t offset;
    uint32_t count;
};
std::vector<String> catArtists;
std::vector<CatalogAlbum> catAlbums;
std::vector<CatalogPlaylist> catPlaylists;

// name -> tracks added this session via addToPlaylist() -- never written
// to the index file, see the header comment on addToPlaylist().
std::map<String, std::vector<Track>> extraPlaylistTracks;

// Playlists removed this session via "Delete Playlist" (Playlists list,
// LEFT long-press -- see MenuEngine::openPlaylistDeleteConfirm()). For a
// playlist backed by the real on-SD index (e.g. "funky times"), "delete"
// means hide it from this session's listing only -- it reappears after a
// reboot or a rescan, same ephemeral-ness as every other session-only
// overlay in this file. For a SESSION-CREATED playlist (lives only in
// extraPlaylistTracks), deletePlaylist() also erases its data, so that
// case IS a complete removal.
std::set<String> hiddenPlaylists;

void writeStr(File &f, const String &s) {
    uint16_t len = (uint16_t)min((size_t)s.length(), kMaxFieldLen);
    f.write((const uint8_t *)&len, 2);
    if (len > 0) f.write((const uint8_t *)s.c_str(), len);
}

// The length comes off the card, so it is checked against the same cap
// writeStr() enforces BEFORE it is used -- it used to be read straight
// into a fixed stack buffer, so a corrupt or truncated index (a length of
// up to 65535) overflowed the stack by up to ~65KB.
bool readStr(File &f, String &out) {
    uint16_t len;
    if (f.read((uint8_t *)&len, 2) != 2) return false;
    if (len > kMaxFieldLen) return false;
    char buf[kMaxFieldLen + 1];
    if (len > 0 && f.read((uint8_t *)buf, len) != len) return false;
    buf[len] = '\0';
    out = buf;
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

// Case-insensitive, and digit runs compare as numbers ("2" before "10",
// "Track 9" before "Track 10") -- what a person expects a track list to
// look like regardless of whether the files are zero-padded.
bool naturalLess(const String &a, const String &b) {
    const char *p = a.c_str(), *q = b.c_str();
    while (*p && *q) {
        if (isdigit((unsigned char)*p) && isdigit((unsigned char)*q)) {
            while (*p == '0') p++;
            while (*q == '0') q++;
            const char *ps = p, *qs = q;
            while (isdigit((unsigned char)*p)) p++;
            while (isdigit((unsigned char)*q)) q++;
            size_t pl = p - ps, ql = q - qs;
            if (pl != ql) return pl < ql;
            int c = strncmp(ps, qs, pl);
            if (c != 0) return c < 0;
        } else {
            int cp = tolower((unsigned char)*p), cq = tolower((unsigned char)*q);
            if (cp != cq) return cp < cq;
            p++;
            q++;
        }
    }
    return *p == 0 && *q != 0;
}

// One directory's entries, names only, sorted. Split by kind because the
// library layout is strictly folders-of-folders-of-files.
void listDirSorted(File &dir, std::vector<String> &subdirs, std::vector<String> &audioFiles) {
    while (File entry = dir.openNextFile()) {
        String name = entry.name();
        if (entry.isDirectory()) subdirs.push_back(name);
        else if (hasAudioExtension(name)) audioFiles.push_back(name);
        entry.close();
    }
    std::sort(subdirs.begin(), subdirs.end(), naturalLess);
    std::sort(audioFiles.begin(), audioFiles.end(), naturalLess);
}

// Called once per album folder during a build: keeps the task watchdog
// fed, and keeps a track that's playing (a manual "Rescan library" from
// Settings can run with music on) supplied with data.
void buildBreather(uint32_t count) {
    yield();
    AudioBridge::pump();
    static uint32_t lastLogged = 0;
    if (count < lastLogged) lastLogged = 0;
    if (count - lastLogged >= 50) {
        lastLogged = count;
        Serial.printf("[library] indexing... %u tracks so far\n", (unsigned)count);
    }
}

// Indexes one Artist folder (its subfolders are Albums, their audio files
// are Tracks). playlistName non-empty: every track is written as a member
// of that playlist instead of as a Music track.
void indexArtistFolder(const String &artistPath, const String &artistName, const String &playlistName,
                        File &idx, uint32_t &count) {
    File artistDir = SD.open(artistPath);
    if (!artistDir) return;
    std::vector<String> albums, strayFiles;
    listDirSorted(artistDir, albums, strayFiles); // files directly under an artist aren't part of the layout
    artistDir.close();

    for (const String &albumName : albums) {
        String albumPath = artistPath + "/" + albumName;
        File albumDir = SD.open(albumPath);
        if (!albumDir) continue;
        std::vector<String> subdirs, files;
        listDirSorted(albumDir, subdirs, files);
        albumDir.close();

        for (const String &fname : files) {
            String title = cleanTrackTitle(fname, artistName);
            String path = albumPath + "/" + fname;
            if (playlistName.length()) writePlaylistRecord(idx, playlistName, artistName, albumName, title, path);
            else writeMusicRecord(idx, artistName, albumName, title, path);
            count++;
        }
        buildBreather(count);
    }
}

// Builds into a temp file and only swaps it into place once it is complete
// -- a power cut or reset mid-build (this takes ~20s on a real card) used
// to leave a half-written /clickpod.idx behind that every later boot then
// trusted as the whole library.
bool buildIndexFile() {
    File root = SD.open("/");
    if (!root) return false;
    std::vector<String> topDirs, strayFiles;
    listDirSorted(root, topDirs, strayFiles);
    root.close();

    if (SD.exists(kIndexTmpPath)) SD.remove(kIndexTmpPath);
    File idx = SD.open(kIndexTmpPath, FILE_WRITE);
    if (!idx) return false;

    idx.write(kIndexMagic, 4);
    uint32_t count = 0;
    idx.write((const uint8_t *)&count, 4); // placeholder, rewritten below

    for (const String &name : topDirs) {
        String path = String("/") + name;
        if (isPlaylistFolderName(name)) {
            // Same Artist/Album/track tree underneath, but it all becomes
            // ONE playlist named after the folder.
            File plDir = SD.open(path);
            if (!plDir) continue;
            std::vector<String> artists, stray;
            listDirSorted(plDir, artists, stray);
            plDir.close();
            for (const String &artistName : artists) {
                indexArtistFolder(path + "/" + artistName, artistName, name, idx, count);
            }
        } else {
            indexArtistFolder(path, name, "", idx, count);
        }
    }

    idx.seek(4);
    idx.write((const uint8_t *)&count, 4);
    idx.close();

    Serial.printf("[library] built index: %u tracks\n", (unsigned)count);
    if (SD.exists(kIndexPath)) SD.remove(kIndexPath);
    if (count == 0) {
        // Nothing playable: leave NO index behind, so the next boot looks
        // again (an empty card scans in no time) instead of trusting an
        // empty library forever.
        SD.remove(kIndexTmpPath);
        return false;
    }
    if (!SD.rename(kIndexTmpPath, kIndexPath)) {
        Serial.println(F("[library] could not move the finished index into place"));
        return false;
    }
    return true;
}

struct IndexRecord {
    bool isPlaylist = false;
    String playlistName;
    String artist, album, title, path;
};

bool readRecord(File &f, IndexRecord &r) {
    uint8_t kind;
    if (f.read(&kind, 1) != 1) return false;
    if (kind > 1) return false; // not a record -- corrupt file
    r.isPlaylist = (kind == 1);
    if (r.isPlaylist && !readStr(f, r.playlistName)) return false;
    if (!readStr(f, r.artist)) return false;
    if (!readStr(f, r.album)) return false;
    if (!readStr(f, r.title)) return false;
    if (!readStr(f, r.path)) return false;
    return true;
}

// One sequential pass over the index: validates it and builds the in-RAM
// catalog. Returns false (catalog left empty) if the file is missing, from
// an older format, truncated, or otherwise doesn't read back as exactly
// the number of records its header says it holds -- the caller rebuilds.
bool loadCatalog() {
    catArtists.clear();
    catAlbums.clear();
    catPlaylists.clear();

    File f = SD.open(kIndexPath, FILE_READ);
    if (!f) return false;
    uint8_t header[8];
    if (f.read(header, 8) != 8 || memcmp(header, kIndexMagic, 4) != 0) {
        f.close();
        return false;
    }
    uint32_t expected;
    memcpy(&expected, header + 4, 4);

    uint32_t seen = 0;
    IndexRecord r;
    while (seen < expected) {
        uint32_t offset = f.position();
        if (!readRecord(f, r)) break;
        seen++;
        if (r.isPlaylist) {
            CatalogPlaylist *pl = nullptr;
            // Contiguous by construction, so the last entry is nearly
            // always the one; the search is only for robustness.
            if (!catPlaylists.empty() && catPlaylists.back().name == r.playlistName) pl = &catPlaylists.back();
            else for (auto &p : catPlaylists) if (p.name == r.playlistName) { pl = &p; break; }
            if (pl) pl->count++;
            else catPlaylists.push_back({r.playlistName, offset, 1});
        } else {
            CatalogAlbum *al = nullptr;
            if (!catAlbums.empty() && catAlbums.back().artist == r.artist && catAlbums.back().album == r.album) {
                al = &catAlbums.back();
            } else {
                for (auto &a : catAlbums) if (a.artist == r.artist && a.album == r.album) { al = &a; break; }
            }
            if (al) {
                if (al->count < UINT16_MAX) al->count++;
            } else {
                catAlbums.push_back({r.artist, r.album, offset, 1});
                bool knownArtist = !catArtists.empty() && catArtists.back() == r.artist;
                if (!knownArtist) for (auto &a : catArtists) if (a == r.artist) { knownArtist = true; break; }
                if (!knownArtist) catArtists.push_back(r.artist);
            }
        }
        if ((seen & 0x3F) == 0) yield();
    }
    f.close();

    if (seen != expected || seen == 0) {
        Serial.printf("[library] index is unusable (%u of %u records read)\n", (unsigned)seen, (unsigned)expected);
        catArtists.clear();
        catAlbums.clear();
        catPlaylists.clear();
        return false;
    }
    Serial.printf("[library] index: %u tracks, %u artists, %u albums, %u playlists\n", (unsigned)seen,
                  (unsigned)catArtists.size(), (unsigned)catAlbums.size(), (unsigned)catPlaylists.size());
    return true;
}

// Reads up to `wanted` records for which keep(record) is true, starting at
// file offset `offset`, handing each to emit(). Stops at `wanted` or EOF.
// The catalog's offsets point at a group's first record and groups are
// contiguous, so this normally reads exactly the records it wants and
// nothing else.
template <typename Keep, typename Emit>
void readRecordsAt(uint32_t offset, uint32_t wanted, Keep keep, Emit emit) {
    File f = SD.open(kIndexPath, FILE_READ);
    if (!f) return;
    if (f.seek(offset)) {
        IndexRecord r;
        uint32_t got = 0, scanned = 0;
        while (got < wanted && readRecord(f, r)) {
            if (keep(r)) {
                emit(r);
                got++;
            }
            if ((++scanned & 0x3F) == 0) AudioBridge::pump(); // a big playlist is a long read
        }
    }
    f.close();
}

const CatalogPlaylist *findCatalogPlaylist(const String &name) {
    for (auto &p : catPlaylists) if (p.name == name) return &p;
    return nullptr;
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
    indexReady = false;
    if (!force && SD.exists(kIndexPath)) {
        // Reuse the existing index rather than re-walking the card -- that
        // is the whole point (the slow FAT walk only needs to happen once,
        // not every boot). No auto-detection of SD content changes; see
        // "Rescan library" in Settings for the manual way to force one.
        // loadCatalog() still reads it through once, which both builds the
        // in-RAM catalog and proves the file is complete.
        if (loadCatalog()) {
            indexReady = true;
            return true;
        }
        Serial.println(F("[library] existing index is from an older build or damaged -- rebuilding"));
    }

    Serial.println(force ? F("[library] rescanning SD (manual request)...")
                          : F("[library] building on-SD index..."));
    if (buildIndexFile() && loadCatalog()) {
        indexReady = true;
        // Session-only playlist edits from before a rescan refer to a
        // library that may no longer match what's on the card.
        extraPlaylistTracks.clear();
        hiddenPlaylists.clear();
    } else {
        Serial.println(F("[library] nothing playable indexed -- keeping placeholder library"));
    }
    return indexReady;
}

std::vector<String> indexArtists() { return catArtists; }

std::vector<std::pair<String, int>> indexAlbumsForArtist(const String &artist) {
    std::vector<std::pair<String, int>> out;
    for (auto &a : catAlbums) {
        if (a.artist == artist) out.push_back({a.album, (int)a.count});
    }
    return out;
}

std::vector<AlbumRef> indexAllAlbums() {
    std::vector<AlbumRef> out;
    out.reserve(catAlbums.size());
    for (auto &a : catAlbums) out.push_back({a.artist, a.album, (int)a.count});
    std::sort(out.begin(), out.end(), [](const AlbumRef &x, const AlbumRef &y) {
        if (x.album.equalsIgnoreCase(y.album)) return naturalLess(x.artist, y.artist);
        return naturalLess(x.album, y.album);
    });
    return out;
}

std::vector<Track> indexTracksForAlbum(const String &artist, const String &album) {
    std::vector<Track> out;
    for (auto &a : catAlbums) {
        if (a.artist != artist || a.album != album) continue;
        out.reserve(a.count);
        readRecordsAt(a.offset, a.count,
                      [&](const IndexRecord &r) { return !r.isPlaylist && r.artist == artist && r.album == album; },
                      [&](const IndexRecord &r) {
                          Track t;
                          t.artist = artist;
                          t.album = album;
                          t.title = r.title;
                          t.path = r.path;
                          out.push_back(std::move(t));
                      });
        break;
    }
    return out;
}

bool isSdBackedPlaylist(const String &name) { return findCatalogPlaylist(name) != nullptr; }

std::vector<std::pair<String, int>> indexPlaylists() {
    std::vector<std::pair<String, int>> out;
    for (auto &p : catPlaylists) {
        if (!hiddenPlaylists.count(p.name)) out.push_back({p.name, (int)p.count});
    }
    for (auto &kv : extraPlaylistTracks) {
        if (kv.second.empty() || hiddenPlaylists.count(kv.first)) continue;
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
    if (const CatalogPlaylist *p = findCatalogPlaylist(name)) {
        out.reserve(p->count);
        readRecordsAt(p->offset, p->count,
                      [&](const IndexRecord &r) { return r.isPlaylist && r.playlistName == name; },
                      [&](const IndexRecord &r) {
                          Track t;
                          t.artist = r.artist;
                          t.album = r.album;
                          t.title = r.title;
                          t.path = r.path;
                          out.push_back(std::move(t));
                      });
    }
    auto it = extraPlaylistTracks.find(name);
    if (it != extraPlaylistTracks.end()) {
        for (auto &t : it->second) out.push_back(t);
    }
    return out;
}

void addToPlaylist(const String &playlistName, const Track &t) {
    // A name deleted earlier this session and now reused (the auto-namer
    // hands "New Playlist 1" out again once the old one is gone) must not
    // stay on the hidden list, or the new playlist would be invisible.
    hiddenPlaylists.erase(playlistName);
    extraPlaylistTracks[playlistName].push_back(t);
}

void deletePlaylist(const String &name) {
    // Complete removal for a session-created playlist. For an index-backed
    // one the hide below is what takes it off the listing, session-only --
    // see the comment on hiddenPlaylists.
    extraPlaylistTracks.erase(name);
    hiddenPlaylists.insert(name);
    // Mock-fallback path: no SD backing at all, so plain removal.
    for (size_t i = 0; i < PLAYLISTS.size(); i++) {
        if (PLAYLISTS[i].name == name) {
            PLAYLISTS.erase(PLAYLISTS.begin() + i);
            break;
        }
    }
}

String nextNewPlaylistName() {
    for (int n = 1;; n++) {
        String candidate = "New Playlist " + String(n);
        bool taken = false;
        if (usingIndex()) {
            taken = findCatalogPlaylist(candidate) != nullptr;
            if (!taken) {
                auto it = extraPlaylistTracks.find(candidate);
                taken = it != extraPlaylistTracks.end() && !it->second.empty();
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
