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
void buildAlbumList(const String &artist);
void buildTrackList(const LibraryAlbum &album);
void buildPlaylistList();
void buildSettings();

void playAlbumFrom(const LibraryAlbum &album, size_t index);
void playQueueFrom(std::vector<Track> list, size_t index);
void playNextInQueue();
void skipPrevious();
void playFromQueueIndex(int idx);

String btStatusLabel();
void enterBluetooth();
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
