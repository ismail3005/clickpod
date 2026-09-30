#pragma once

#include <map>
#include <vector>

#include "UiTypes.h"

// Placeholder library data for UI/UX iteration, ported straight from the
// browser simulator (see docs/SPEC.md section 10: real FLAC metadata
// parsing isn't built yet, so this stands in for a real SD-scanned
// library until that work happens). Selecting a track drives real I2S
// playback of whatever audio file AudioBridge can find on the card --
// see AudioBridge.h -- the metadata shown on screen is this mock data,
// not necessarily an exact match for the file actually playing.
namespace Library {

extern std::vector<LibraryAlbum> ALBUMS;
extern std::vector<Playlist> PLAYLISTS;
extern std::vector<BtDevice> BT_DEVICES;

// keyed on keyFor(track) ("artist|album|title"), same as the simulator
extern std::map<String, std::vector<LyricLine>> LYRICS;

Track trackAt(size_t albumIndex, size_t trackIndex);
String keyFor(const Track &t);

} // namespace Library
