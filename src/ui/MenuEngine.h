#pragma once

#include "UiTypes.h"

// Menu-stack construction and navigation, ported function-for-function
// from the browser simulator (buildMainMenu, buildArtistList, ...,
// enterBluetooth/exitBluetooth, openTrackMenu/closeTrackMenu). Mutates the
// global `state` (src/state/AppState.h) exactly like the JS version
// mutates its `state` object. Cross-check against the simulator source
// before changing behavior here.
namespace MenuEngine {

void pushMenu(const String &title, std::vector<MenuItem> items, int selected = 0);
Menu *currentMenu();

void buildMainMenu();
void buildArtistList();
void buildAlbumList(const String &artist);           // mock/fallback path -- see Library::usingIndex()
void buildTrackList(const LibraryAlbum &album);       // mock/fallback path
void buildAlbumListFromIndex(const String &artist);   // real on-SD-index path
void buildTrackListFromIndex(const String &artist, const String &album);
void buildPlaylistTrackListFromIndex(const String &name);
void buildPlaylistList();
void buildSettings();

void playAlbumFrom(const LibraryAlbum &album, size_t index);
void playQueueFrom(std::vector<Track> list, size_t index);
void playNextInQueue();
void skipPrevious();
void playFromQueueIndex(int idx);

String btStatusLabel();
void enterBluetooth();
// Real device-picker screen -- ESP32-A2DP's source mode genuinely
// supports discovery. enterBluetoothDevicePicker() starts a scan and
// pushes the list screen; refreshBluetoothDevicesMenu() rebuilds its rows
// (called by UI.cpp whenever BluetoothSource::discoveredCount() changes
// while that screen is open); chooseBluetoothDevice() is the row action
// that connects to and persists the picked device.
void enterBluetoothDevicePicker();
void refreshBluetoothDevicesMenu();
void chooseBluetoothDevice(const String &name);

// Which line of `lines` is "active" right now (state.now.posSec), i.e.
// the one Screens.cpp's drawLyrics() highlights. Shared between it and
// UI.cpp's tickPlaybackClock() -- the latter uses this to only mark the
// Lyrics screen dirty (full redraw) when the active line actually
// changes, instead of on every ~500ms position tick regardless, which
// was producing a visible flicker on that screen once it started
// actually tracking. Returns 0 for an empty list (matches drawLyrics()'s
// prior inline behavior).
int activeLyricIndex(const std::vector<LyricLine> &lines);
void exitBluetooth();

void openTrackMenu(const Track &track);
void closeTrackMenu();

void moveSelection(int delta);
void moveQueueSelection(int delta);
void moveGrabbedQueueItem(int delta);
void adjustSlider(MenuItem &item, int delta);
void cycleChoice(MenuItem &item, int dir);

bool isMainMenuRoot();

} // namespace MenuEngine
