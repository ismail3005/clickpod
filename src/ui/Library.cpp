#include "Library.h"

#include "Util.h"

namespace Library {
namespace {

// Kept as a fallback for bench-testing without an SD card inserted --
// scanFromSd() overwrites ALBUMS/PLAYLISTS with real data whenever it
// finds any, so this is never what a card with real music actually shows.
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

void scanAlbumFolder(File albumDir, const String &albumPath, std::vector<Track> &outTracks) {
    while (File entry = albumDir.openNextFile()) {
        if (!entry.isDirectory()) {
            String fname = entry.name();
            if (hasAudioExtension(fname)) {
                Track t;
                t.title = stripExtension(fname);
                t.durSec = 0; // TODO(spec section 10): real duration needs opening/decoding each file
                t.path = albumPath + "/" + fname;
                outTracks.push_back(std::move(t));
            }
        }
        entry.close();
    }
}

void scanArtistFolder(File artistDir, const String &artistPath, const String &artistName,
                       std::vector<LibraryAlbum> &outAlbums) {
    while (File entry = artistDir.openNextFile()) {
        if (entry.isDirectory()) {
            String albumName = entry.name();
            LibraryAlbum album;
            album.artist = artistName;
            album.album = albumName;
            scanAlbumFolder(entry, artistPath + "/" + albumName, album.tracks);
            if (!album.tracks.empty()) outAlbums.push_back(std::move(album));
            // Yields to the scheduler between albums so a large library
            // scan can't starve the task watchdog into a reboot loop, and
            // gives visible progress on serial instead of a long silence.
            yield();
            if (outAlbums.size() % 10 == 0) {
                Serial.printf("[library] scanning... %u albums so far\n", (unsigned)outAlbums.size());
            }
        }
        entry.close();
    }
}

void scanPlaylistFolder(File plDir, const String &plPath, const String &plName,
                         std::vector<Playlist> &outPlaylists) {
    Playlist pl;
    pl.name = plName;
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
                                Track t;
                                t.artist = artistName;
                                t.album = albumName;
                                t.title = stripExtension(fname);
                                t.path = albumPath + "/" + fname;
                                pl.tracks.push_back(std::move(t));
                            }
                        }
                        fileEntry.close();
                    }
                }
                albumEntry.close();
                // Same watchdog/progress reasoning as scanArtistFolder --
                // this is the deepest-nested loop (root->playlist->artist
                // ->album->file), the one most likely to run long.
                yield();
                if (pl.tracks.size() % 25 == 0 && !pl.tracks.empty()) {
                    Serial.printf("[library] scanning playlist \"%s\"... %u tracks so far\n",
                                  plName.c_str(), (unsigned)pl.tracks.size());
                }
            }
        }
        artistEntry.close();
    }
    if (!pl.tracks.empty()) outPlaylists.push_back(std::move(pl));
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

bool scanFromSd() {
    uint32_t startMs = millis();
    File root = SD.open("/");
    if (!root) return false;

    std::vector<LibraryAlbum> albums;
    std::vector<Playlist> playlists;

    while (File entry = root.openNextFile()) {
        if (entry.isDirectory()) {
            String name = entry.name();
            String path = String("/") + name;
            if (isPlaylistFolderName(name)) {
                scanPlaylistFolder(entry, path, name, playlists);
            } else {
                scanArtistFolder(entry, path, name, albums);
            }
        }
        entry.close();
    }
    root.close();

    if (albums.empty() && playlists.empty()) {
        Serial.println(F("[library] SD scan found no playable tracks -- keeping placeholder library"));
        return false;
    }

    ALBUMS = std::move(albums);
    PLAYLISTS = std::move(playlists);
    // No real lyrics source yet -- scanned tracks just show "No lyrics for
    // this track" (renderLyrics/drawLyrics already handle a missing key).
    LYRICS.clear();
    Serial.printf("[library] scanned SD: %u albums, %u playlists (%lums)\n",
                   (unsigned)ALBUMS.size(), (unsigned)PLAYLISTS.size(), (unsigned long)(millis() - startMs));
    return true;
}

} // namespace Library
