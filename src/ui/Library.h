#pragma once

#include <SD.h>

#include <map>
#include <vector>

#include "UiTypes.h"

// ALBUMS/PLAYLISTS start out as a small placeholder set (ported from the
// browser simulator) and get replaced by scanFromSd() once real files are
// found. Real FLAC metadata parsing isn't built yet (docs/SPEC.md section
// 10) -- scanned track titles come from filenames, not tags, and
// durations are unknown (0) until that exists.
namespace Library {

extern std::vector<LibraryAlbum> ALBUMS;
extern std::vector<Playlist> PLAYLISTS;
extern std::vector<BtDevice> BT_DEVICES;

// keyed on keyFor(track) ("artist|album|title"), same as the simulator
extern std::map<String, std::vector<LyricLine>> LYRICS;

Track trackAt(size_t albumIndex, size_t trackIndex);
String keyFor(const Track &t);

// Walks the SD card's root: each top-level folder becomes an Artist
// (its subfolders are Albums, their files are Tracks), except folder
// names recognized as playlist folders (see isPlaylistFolderName in
// Library.cpp), which become a single named Playlist instead -- so a
// playlist folder that duplicates albums also downloaded separately
// doesn't show those albums twice in Music. Replaces ALBUMS/PLAYLISTS
// with the scan results if it finds anything; leaves them as the
// placeholder set and returns false otherwise (no SD, empty card, or
// genuinely no audio files found).
bool scanFromSd();

} // namespace Library
