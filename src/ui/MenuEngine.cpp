#include "MenuEngine.h"

#include <memory>
#include <set>
#include <utility>

#include "../audio/AudioBridge.h"
#include "../audio/FlacMeta.h"
#include "../bt/BluetoothSource.h"
#include "../state/AppState.h"
#include "AlbumArt.h"
#include "Library.h"
#include "Util.h"

namespace MenuEngine {
namespace {

Track trackObj(const NowPlaying &n) {
    return Track{n.artist, n.album, n.title, n.durSec, n.art, n.path};
}

// Splits a lyrics blob (as stored in a LYRICS/UNSYNCEDLYRICS Vorbis
// comment -- plain text, NOT time-synced, hence "unsynced") into lines
// for the Lyrics screen. Every line gets atSec=0 since there's no real
// timing data to assign -- drawLyrics()'s "active line" picks the LAST
// line whenever every candidate ties on atSec, so with real embedded
// lyrics the screen shows real text (the actual ask) at the cost of the
// scrolling highlight not tracking playback position the way it does
// for the one hand-written time-synced demo entry. Not worth more
// engineering than that for a tag format that's plain text by design.
std::vector<LyricLine> splitLyricsIntoLines(const String &text) {
    std::vector<LyricLine> lines;
    int start = 0;
    for (int i = 0; i <= text.length(); i++) {
        if (i == text.length() || text[i] == '\n') {
            String line = text.substring(start, i);
            if (line.endsWith("\r")) line = line.substring(0, line.length() - 1);
            if (line.length() > 0) lines.push_back(LyricLine{0, line});
            start = i + 1;
        }
    }
    return lines;
}

void setNowPlaying(Track t) {
    // Lazy real-metadata fetch -- only for the ONE track actually becoming
    // Now Playing, not during the bulk SD scan (see FlacMeta.h for why).
    // Upgrades filename-derived artist/title/album to real tags when
    // present, fills in a real duration (durSec stays 0/unknown if this
    // fails, which UI.cpp's playback clock already guards against), and
    // picks up real embedded lyrics if the file has them.
    if (t.path.length() > 0) {
        FlacMeta::StreamInfo si;
        if (FlacMeta::readStreamInfo(t.path, si)) {
            float dur = FlacMeta::durationSec(si);
            if (dur > 0 && dur < 65536) t.durSec = (uint16_t)dur;
        }

        FlacMeta::Tags tags;
        if (FlacMeta::readTags(t.path, tags)) {
            if (tags.hasArtist) t.artist = tags.artist;
            if (tags.hasTitle) t.title = tags.title;
            if (tags.hasAlbum) t.album = tags.album;
            if (tags.hasLyrics) {
                Library::LYRICS[Library::keyFor(t)] = splitLyricsIntoLines(tags.lyrics);
            }
        }

        AlbumArt::loadForTrack(t.path); // no-op-safe if the file has no (or non-JPEG) embedded art
    } else {
        AlbumArt::clear(); // placeholder/mock track with no real path -- don't show the previous track's art
    }

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
    // Every build*() function (buildArtistList, buildAlbumList,
    // buildTrackList, buildPlaylistList, buildSettings, ...) funnels
    // through here to enter a new menu level -- most of them never set
    // state.dirty themselves, which meant selecting into Music/Playlists/
    // an Artist/an Album/Settings did nothing visible until some LATER,
    // unrelated action (e.g. the next UP/DOWN) happened to set dirty and
    // trigger a redraw -- at which point the already-changed menu would
    // suddenly appear, looking like it belonged to the wrong button press.
    // Setting it here once, in the one function every menu-entry path
    // goes through, fixes all of them at once instead of patching each
    // build*() individually.
    state.dirty = true;
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
    // Shared ONCE for the whole album, not copied per row -- capturing a
    // full LibraryAlbum copy inside this loop (one copy per track) made
    // opening an N-track album do O(N^2) copying. Harmless for a handful
    // of tracks, but the same bug in buildPlaylistList() below crashed the
    // device outright on a real 425-track playlist (425 copies of a
    // 425-track vector, once per row == ~180k Track copies just to open
    // the menu). Fixed the same way in both places.
    auto albumPtr = std::make_shared<LibraryAlbum>(album);
    std::vector<MenuItem> items;
    items.reserve(album.tracks.size());
    for (size_t i = 0; i < album.tracks.size(); i++) {
        const Track &t = album.tracks[i];
        MenuItem it;
        it.label = t.title;
        it.sub = fmtTime(t.durSec);
        it.isTrack = true;
        it.trackData = Track{album.artist, album.album, t.title, t.durSec, album.art, t.path};
        size_t idx = i;
        it.action = [albumPtr, idx]() { playAlbumFrom(*albumPtr, idx); };
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
        // Shared ONCE per playlist-open (see buildTrackList's comment for
        // why this matters -- this was the actual crash).
        auto tracksPtr = std::make_shared<std::vector<Track>>(p.tracks);
        String plName = p.name;
        it.action = [tracksPtr, plName]() {
            std::vector<MenuItem> trackItems;
            for (size_t i = 0; i < tracksPtr->size(); i++) {
                const Track &t = (*tracksPtr)[i];
                MenuItem row;
                row.label = t.title;
                row.sub = t.artist;
                row.isTrack = true;
                row.trackData = t;
                size_t idx = i;
                row.action = [tracksPtr, idx]() { playQueueFrom(*tracksPtr, idx); };
                trackItems.push_back(std::move(row));
            }
            pushMenu(plName, std::move(trackItems));
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
// screen, which is exactly the bug the simulator hit first. state.btOn/
// btConnectedTo are synced from the real BluetoothSource each loop()
// iteration (main.cpp's syncBluetoothToUi()), not a mock device list.
String btStatusLabel() {
    if (!state.btOn) return "Off";
    return state.btConnectedTo.length() ? "Connected: " + state.btConnectedTo : "Connecting...";
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
    state.mode = AppMode::BT;

    // ESP32-A2DP source mode connects to ONE hardcoded target sink by
    // name (see BluetoothSource.h) -- it doesn't enumerate discoverable
    // devices to pick from like a phone's Bluetooth settings, so this is
    // a single real row for that target, not a device picker.
    std::vector<MenuItem> items(2);
    items[0].label = BluetoothSource::kTargetDeviceName;
    items[0].subFn = btStatusLabel;
    items[0].action = []() {
        if (!state.btOn) {
            BluetoothSource::begin(BluetoothSource::kTargetDeviceName);
            state.btOn = true;
            Serial.println(F("[ui] Bluetooth on, connecting..."));
            state.dirty = true;
        }
    };

    items[1].label = "Turn Bluetooth Off";
    items[1].action = []() {
        BluetoothSource::end();
        state.btOn = false;
        state.btConnectedTo = "";
        exitBluetooth();
    };

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
    int prev = m->selected;
    m->selected = ((m->selected + delta) % n + n) % n;
    if (prev == m->selected) return;
    // The main-menu grid's partial-redraw path isn't implemented (only 4
    // items, full redraw there is already cheap) -- everywhere else
    // (Music/Playlists/Artist/Album/Settings/BT/track-context lists) gets
    // the lighter selectionDirty path instead of a full-body redraw on
    // every single UP/DOWN tap.
    if (isMainMenuRoot()) state.dirty = true;
    else state.selectionDirty = true;
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
