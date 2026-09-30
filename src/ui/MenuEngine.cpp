#include "MenuEngine.h"

#include <set>
#include <utility>

#include "../audio/AudioBridge.h"
#include "../state/AppState.h"
#include "Library.h"
#include "Util.h"

namespace MenuEngine {
namespace {

Track trackObj(const NowPlaying &n) {
    return Track{n.artist, n.album, n.title, n.durSec, n.art, n.path};
}

void setNowPlaying(const Track &t) {
    state.now.hasTrack = true;
    state.now.key = Library::keyFor(t);
    state.now.artist = t.artist;
    state.now.album = t.album;
    state.now.title = t.title;
    state.now.art = t.art;
    state.now.durSec = t.durSec;
    state.now.posSec = 0;
    state.now.playing = true;
    state.now.path = t.path;
    AudioBridge::playSomething(t.path);
}

} // namespace

void pushMenu(const String &title, std::vector<MenuItem> items, int selected) {
    Menu m;
    m.title = title;
    m.items = std::move(items);
    m.selected = selected;
    state.menuStack.push_back(std::move(m));
}

Menu *currentMenu() {
    if (state.menuStack.empty()) return nullptr;
    return &state.menuStack.back();
}

void buildMainMenu() {
    state.menuStack.clear();
    std::vector<MenuItem> items(4);
    items[0].label = "Music";
    items[0].icon = "note";
    items[0].action = []() { buildArtistList(); };
    items[1].label = "Playlists";
    items[1].icon = "playlist";
    items[1].action = []() { buildPlaylistList(); };
    items[2].label = "Bluetooth";
    items[2].icon = "bt";
    items[2].subFn = btStatusLabel;
    items[2].action = []() { enterBluetooth(); };
    items[3].label = "Settings";
    items[3].icon = "gear";
    items[3].action = []() { buildSettings(); };
    pushMenu("clickpod", std::move(items));
}

void buildArtistList() {
    std::vector<String> artists;
    std::set<String> seen;
    for (auto &al : Library::ALBUMS) {
        if (seen.insert(al.artist).second) artists.push_back(al.artist);
    }
    std::vector<MenuItem> items;
    items.reserve(artists.size());
    for (auto &name : artists) {
        MenuItem it;
        it.label = name;
        it.action = [name]() { buildAlbumList(name); };
        items.push_back(std::move(it));
    }
    pushMenu("Music", std::move(items));
}

void buildAlbumList(const String &artist) {
    std::vector<MenuItem> items;
    for (auto &al : Library::ALBUMS) {
        if (al.artist != artist) continue;
        MenuItem it;
        it.label = al.album;
        it.sub = String((int)al.tracks.size()) + " tracks";
        LibraryAlbum copy = al; // captured by value: album list is small/static, cheap to copy
        it.action = [copy]() { buildTrackList(copy); };
        items.push_back(std::move(it));
    }
    pushMenu(artist, std::move(items));
}

void buildTrackList(const LibraryAlbum &album) {
    std::vector<MenuItem> items;
    items.reserve(album.tracks.size());
    for (size_t i = 0; i < album.tracks.size(); i++) {
        const Track &t = album.tracks[i];
        MenuItem it;
        it.label = t.title;
        it.sub = fmtTime(t.durSec);
        it.isTrack = true;
        it.trackData = Track{album.artist, album.album, t.title, t.durSec, album.art, t.path};
        LibraryAlbum copy = album;
        size_t idx = i;
        it.action = [copy, idx]() { playAlbumFrom(copy, idx); };
        items.push_back(std::move(it));
    }
    pushMenu(album.album, std::move(items));
}

void buildPlaylistList() {
    std::vector<MenuItem> items;
    for (auto &p : Library::PLAYLISTS) {
        MenuItem it;
        it.label = p.name;
        it.sub = String((int)p.tracks.size()) + " tracks";
        Playlist copy = p; // playlist can grow (Add to Playlist); snapshot at menu-open time, like the sim
        it.action = [copy]() {
            std::vector<MenuItem> trackItems;
            for (size_t i = 0; i < copy.tracks.size(); i++) {
                const Track &t = copy.tracks[i];
                MenuItem row;
                row.label = t.title;
                row.sub = t.artist;
                row.isTrack = true;
                row.trackData = t;
                std::vector<Track> list = copy.tracks;
                size_t idx = i;
                row.action = [list, idx]() { playQueueFrom(list, idx); };
                trackItems.push_back(std::move(row));
            }
            pushMenu(copy.name, std::move(trackItems));
        };
        items.push_back(std::move(it));
    }
    pushMenu("Playlists", std::move(items));
}

void buildSettings() {
    std::vector<MenuItem> items(4);
    items[0].label = "Bluetooth";
    items[0].subFn = btStatusLabel;
    items[0].action = []() { enterBluetooth(); };

    items[1].label = "Brightness";
    items[1].sub = String(state.brightness) + "%";
    items[1].isSlider = true;
    items[1].getInt = []() { return state.brightness; };
    items[1].setInt = [](int v) { state.brightness = constrain(v, 10, 100); };

    items[2].label = "Sort tracks by";
    items[2].sub = state.sortPref;
    items[2].isChoice = true;
    items[2].options = {"Artist", "Album"};
    items[2].getChoice = []() { return state.sortPref; };
    items[2].setChoice = [](const String &v) { state.sortPref = v; };

    items[3].label = "Appearance";
    items[3].sub = state.darkMode ? "Dark" : "Light";
    items[3].isChoice = true;
    items[3].options = {"Light", "Dark"};
    items[3].getChoice = []() { return state.darkMode ? String("Dark") : String("Light"); };
    items[3].setChoice = [](const String &v) { state.darkMode = (v == "Dark"); };

    pushMenu("Settings", std::move(items));
}

void playAlbumFrom(const LibraryAlbum &album, size_t index) {
    std::vector<Track> list;
    list.reserve(album.tracks.size());
    for (auto &t : album.tracks) list.push_back(Track{album.artist, album.album, t.title, t.durSec, album.art, t.path});
    playQueueFrom(std::move(list), index);
}

void playQueueFrom(std::vector<Track> list, size_t index) {
    if (index >= list.size()) return;
    const Track &t = list[index];
    setNowPlaying(t);
    state.queue.assign(list.begin() + index + 1, list.end());
    state.history.clear();
    state.mode = AppMode::NOW_PLAYING;
    state.lastActiveMode = AppMode::NOW_PLAYING;
    Serial.printf("[ui] play \"%s\"\n", t.title.c_str());
    state.dirty = true;
}

void playNextInQueue() {
    if (state.queue.empty()) {
        state.now.playing = false;
        state.dirty = true;
        return;
    }
    if (state.now.hasTrack) state.history.push_back(trackObj(state.now));
    Track t = state.queue.front();
    state.queue.erase(state.queue.begin());
    setNowPlaying(t);
    state.dirty = true;
}

// Classic iPod behavior: a few seconds into the track, "previous" restarts
// it instead of actually going back -- only jumps to the prior track when
// pressed again near the very start.
void skipPrevious() {
    if (state.now.posSec > 3 || state.history.empty()) {
        state.now.posSec = 0;
        state.dirty = true;
        return;
    }
    state.queue.insert(state.queue.begin(), trackObj(state.now));
    Track prev = state.history.back();
    state.history.pop_back();
    setNowPlaying(prev);
    state.dirty = true;
}

void playFromQueueIndex(int idx) {
    if (idx < 0 || idx >= (int)state.queue.size()) return;
    if (state.now.hasTrack) state.history.push_back(trackObj(state.now));
    for (int i = 0; i < idx; i++) state.history.push_back(state.queue[i]);
    Track t = state.queue[idx];
    state.queue.erase(state.queue.begin(), state.queue.begin() + idx + 1);
    setNowPlaying(t);
    state.mode = AppMode::NOW_PLAYING;
    Serial.printf("[ui] play from queue: \"%s\"\n", t.title.c_str());
    state.dirty = true;
}

// Called at render time, not baked into a menu item once -- a plain string
// sub-label would go stale the moment BT state changes on a different
// screen, which is exactly the bug the simulator hit first.
String btStatusLabel() {
    if (!state.btOn) return "Off";
    return state.btConnectedTo.length() ? "Connected: " + state.btConnectedTo : "On";
}

void enterBluetooth() {
    // Bluetooth is reachable globally (long-press RIGHT from any mode), so
    // it needs its own way back to wherever the user actually was --
    // popping the menu stack doesn't work if they entered from
    // NOW_PLAYING/LYRICS/QUEUE, and doesn't work from MENU either since
    // this replaces the stack outright.
    if (state.mode != AppMode::BT) {
        state.btReturn = MenuReturn{true, state.mode, state.menuStack};
    }
    state.btOn = true;
    state.mode = AppMode::BT;

    std::vector<MenuItem> items;
    for (auto &d : Library::BT_DEVICES) {
        MenuItem it;
        it.label = d.name;
        it.sub = (state.btConnectedTo == d.name) ? "Connected" : (d.paired ? "Paired" : "New");
        String name = d.name;
        it.action = [name]() {
            state.btConnectedTo = name;
            Serial.printf("[ui] connected to %s\n", name.c_str());
            state.dirty = true;
        };
        items.push_back(std::move(it));
    }
    MenuItem off;
    off.label = "Turn Bluetooth Off";
    off.action = []() {
        state.btOn = false;
        state.btConnectedTo = "";
        exitBluetooth();
    };
    items.push_back(std::move(off));

    state.menuStack.clear();
    pushMenu("Bluetooth", std::move(items));
    state.dirty = true;
}

void exitBluetooth() {
    MenuReturn back = state.btReturn.valid ? state.btReturn : MenuReturn{true, AppMode::MENU, {}};
    state.mode = back.mode;
    state.menuStack = back.stack;
    if (state.mode == AppMode::MENU && state.menuStack.empty()) buildMainMenu();
    state.dirty = true;
}

// Track context menu ("..." menu on a song: Play Next / Add to Queue / Add
// to Playlist), opened with LEFT long-press on any track row, on Now
// Playing, or on a Queue entry. Same return-to-where-you-were pattern as
// Bluetooth above.
void openTrackMenu(const Track &track) {
    state.trackMenuReturn = MenuReturn{true, state.mode, state.menuStack};
    state.mode = AppMode::TRACK_MENU;

    std::vector<MenuItem> items(4);
    Track t = track;
    items[0].label = "Play Next";
    items[0].action = [t]() {
        state.queue.insert(state.queue.begin(), t);
        Serial.printf("[ui] play next: \"%s\"\n", t.title.c_str());
        closeTrackMenu();
    };
    items[1].label = "Add to Queue";
    items[1].action = [t]() {
        state.queue.push_back(t);
        Serial.printf("[ui] added to queue: \"%s\"\n", t.title.c_str());
        closeTrackMenu();
    };
    items[2].label = "Add to Playlist";
    items[2].action = [t]() {
        std::vector<MenuItem> plItems;
        for (auto &p : Library::PLAYLISTS) {
            MenuItem row;
            row.label = p.name;
            row.sub = String((int)p.tracks.size()) + " tracks";
            String plName = p.name;
            Track track2 = t;
            row.action = [plName, track2]() {
                for (auto &p2 : Library::PLAYLISTS) {
                    if (p2.name == plName) {
                        p2.tracks.push_back(track2);
                        break;
                    }
                }
                Serial.printf("[ui] added to \"%s\"\n", plName.c_str());
                closeTrackMenu();
            };
            plItems.push_back(std::move(row));
        }
        pushMenu("Add to Playlist", std::move(plItems));
    };
    items[3].label = "Cancel";
    items[3].action = []() { closeTrackMenu(); };

    state.menuStack.clear();
    pushMenu(track.title, std::move(items));
    state.dirty = true;
}

void closeTrackMenu() {
    MenuReturn back = state.trackMenuReturn.valid ? state.trackMenuReturn : MenuReturn{true, AppMode::MENU, {}};
    state.mode = back.mode;
    state.menuStack = back.stack;
    if (state.mode == AppMode::MENU && state.menuStack.empty()) buildMainMenu();
    state.dirty = true;
}

void moveSelection(int delta) {
    Menu *m = currentMenu();
    if (!m || m->items.empty()) return;
    int n = (int)m->items.size();
    m->selected = ((m->selected + delta) % n + n) % n;
    state.dirty = true;
}

void moveQueueSelection(int delta) {
    if (state.queue.empty()) return;
    int n = (int)state.queue.size();
    state.queueSelected = ((state.queueSelected + delta) % n + n) % n;
    state.dirty = true;
}

// While a queue row is "grabbed" (see the RIGHT-tap toggle in
// InputRouter), UP/DOWN swap it with its neighbor and move the cursor
// along with it, instead of just moving the cursor -- the drag-and-drop
// equivalent for a device with no touchscreen.
void moveGrabbedQueueItem(int delta) {
    int n = (int)state.queue.size();
    if (n < 2) return;
    int from = state.queueSelected;
    int to = ((from + delta) % n + n) % n;
    std::swap(state.queue[from], state.queue[to]);
    state.queueSelected = to;
    state.dirty = true;
}

void adjustSlider(MenuItem &item, int delta) {
    item.setInt(item.getInt() + delta);
    item.sub = String(item.getInt()) + "%";
    state.dirty = true;
}

void cycleChoice(MenuItem &item, int dir) {
    int i = -1;
    for (size_t k = 0; k < item.options.size(); k++) {
        if (item.options[k] == item.getChoice()) { i = (int)k; break; }
    }
    int n = (int)item.options.size();
    String next = item.options[((i + dir) % n + n) % n];
    item.setChoice(next);
    item.sub = next;
    state.dirty = true;
}

bool isMainMenuRoot() {
    return state.mode == AppMode::MENU && state.menuStack.size() == 1 &&
           currentMenu() && currentMenu()->title == "clickpod";
}

} // namespace MenuEngine
