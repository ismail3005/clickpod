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

// "Delete Playlist" (Playlists list, LEFT long-press). True, complete
// removal for a session-created playlist (never had real data
// elsewhere); for one backed by the real on-SD index, removes it from
// this session's listing only -- see Library.cpp's big comment on
// hiddenPlaylists for why a real on-SD removal isn't attempted.
void deletePlaylist(const String &name);

// Appends one line ("<path> -- <reason>") to /clickpod_failed.txt on SD
// (creating it if needed) -- a plain-text, human-readable list of every
// track that failed to play this session (or a previous one; never
// cleared automatically), so finding which files need re-encoding (see
// CLAUDE.md's FLAC-limitations writeup, option 2) doesn't require
// catching the message live in the serial monitor. Called from both
// real skip sites (MenuEngine.cpp's proactive 24-bit-skip, UI.cpp's
// generic decode-failure grace-period skip) so neither path needs its
// own SD-writing logic. Safe to call often -- opens/appends/closes once
// per call, no held file handle, and silently no-ops if SD isn't ready
// (mirrors every other SD helper in this codebase).
void logFailedFile(const String &path, const String &reason);

// Returns the first unused "New Playlist N" name (N starting at 1),
// checking both the real index/overlay path and the mock PLAYLISTS
// fallback so it never collides either way. Used by MenuEngine.cpp's
// "+ New Playlist" row (see openTrackMenu()) -- auto-named rather than
// a real text-entry UI, which this device has no existing component
// for (no keyboard, just an encoder + 5 buttons) and would need a real
// letter-picker screen designed simulator-first per this project's
// usual UX discipline, not improvised here. Not a persistent counter --
// just counts up past whatever names already exist each time it's
// called, so creating N playlists in one session and renaming/deleting
// isn't tracked (there's no rename/delete yet either).
String nextNewPlaylistName();

} // namespace Library
