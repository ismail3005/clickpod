#pragma once

#include <SD.h>

#include <map>
#include <utility>
#include <vector>

#include "UiTypes.h"

// ALBUMS/PLAYLISTS are a small placeholder set (ported from the browser
// simulator) for bench-testing with no SD card, or a card the index build
// finds nothing on. Once a real on-SD index exists (usingIndex() is true),
// Music/Playlists are driven from the index*() functions below instead --
// ALBUMS/PLAYLISTS stay untouched at their mock values in that case, they
// just aren't what's actually shown.
namespace Library {

extern std::vector<LibraryAlbum> ALBUMS;
extern std::vector<Playlist> PLAYLISTS;

// keyed on keyFor(track) ("artist|album|title"), same as the simulator
extern std::map<String, std::vector<LyricLine>> LYRICS;

Track trackAt(size_t albumIndex, size_t trackIndex);
String keyFor(const Track &t);

// On-SD compact index (/clickpod.idx): built once by walking the SD card
// (same Artist/Album/track / playlist-folder rules as before -- see
// isPlaylistFolderName() in Library.cpp), then read back sequentially
// whenever Music/Playlists need data, instead of holding the whole
// library's Track data in RAM for the entire session. See CLAUDE.md for
// the full writeup (why: boot-time FAT walk only needed once ever, not
// every boot; RAM bounded by what's actually open, not total library
// size).
//
// ensureIndex() only does the slow full SD walk if /clickpod.idx doesn't
// already exist, or force=true -- an existing index is trusted as-is,
// there's no auto-detection of SD content changes (see "Rescan library"
// in Settings for the manual way to force a rebuild). Returns true if a
// usable index is in effect afterward (freshly built, or already there);
// false leaves ALBUMS/PLAYLISTS (the mock set) in effect instead, same as
// the old scanFromSd()'s "keep placeholder library" fallback.
bool ensureIndex(bool force = false);

// True once a real on-SD index is in effect -- callers (MenuEngine) use
// this to pick between the index*() functions below and the old
// ALBUMS/PLAYLISTS mock-data path.
bool usingIndex();

// Cheap summaries: a fast sequential read of the compact index file,
// collecting just names/counts -- NOT the per-track Track data, safe to
// call on every menu build.
std::vector<String> indexArtists();
std::vector<std::pair<String, int>> indexAlbumsForArtist(const String &artist); // {albumName, trackCount}
std::vector<std::pair<String, int>> indexPlaylists(); // {playlistName, trackCount}, includes ephemeral additions

// Bounded loads: only the tracks for ONE album or ONE playlist, read from
// the index -- not the whole library. indexTracksForPlaylist() also
// appends any tracks added this session via addToPlaylist() below.
std::vector<Track> indexTracksForAlbum(const String &artist, const String &album);
std::vector<Track> indexTracksForPlaylist(const String &name);

// "Add to Playlist" support for the index-backed path. Appends to an
// in-RAM, session-only overlay -- never written back to the index file on
// SD, so it doesn't survive a reboot. This matches the old ALBUMS/
// PLAYLISTS-mutation behavior exactly, which also never persisted
// playlist edits across reboots -- not a regression, just the same
// ephemeral behavior under the new storage model.
void addToPlaylist(const String &playlistName, const Track &t);

} // namespace Library
