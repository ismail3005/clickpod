#include "MenuEngine.h"

#include <memory>
#include <set>
#include <utility>

#include "../audio/AudioBridge.h"
#include "../audio/FlacMeta.h"
#include "../bt/BluetoothSource.h"
#include "../state/AppState.h"
#include "../state/Persist.h"
#include "AlbumArt.h"
#include "Library.h"
#include "Screens.h"
#include "UI.h"
#include "Util.h"

namespace MenuEngine {
namespace {

Track trackObj(const NowPlaying &n) {
    return Track{n.artist, n.album, n.title, n.durSec, n.art, n.path};
}

// Strips a leading LRC-style "[mm:ss]"/"[mm:ss.xx]"/"[hh:mm:ss.xx]"
// timestamp tag off the front of `line` (in place), if there is one --
// some taggers/rippers store LYRICS/UNSYNCEDLYRICS tags in LRC format
// even though the field name says "unsynced," which was showing up
// on-screen as a literal "[00:08]" stuck in front of every line (user
// report: "in front of the lyrics is the timestamp"). Handles more than
// one leading tag on the same line too (LRC allows several timestamps
// sharing one lyric). A leading bracket with no colon inside (e.g. a
// genuine "[Chorus]"/"[instrumental intro]" section marker) is left
// alone -- only things that actually parse as a clock get stripped.
// Returns true (via outSec) if at least one real timestamp was found and
// parsed, so the caller can tell "genuinely unsynced" apart from "really
// has per-line timing" and use the real numbers instead of guessing.
bool stripLeadingTimestamp(String &line, uint16_t &outSec) {
    bool found = false;
    while (line.startsWith("[")) {
        int close = line.indexOf(']');
        if (close < 0) break;
        String tag = line.substring(1, close);
        int firstColon = tag.indexOf(':');
        if (firstColon < 0) break; // not a timestamp -- leave this (and any further) bracket alone
        int secondColon = tag.indexOf(':', firstColon + 1);
        int mm, ss;
        if (secondColon < 0) {
            mm = tag.substring(0, firstColon).toInt();
            ss = tag.substring(firstColon + 1).toInt(); // toInt() stops at the '.' -- fractional seconds ignored, fine at our 1s resolution
        } else {
            // hh:mm:ss(.xx) -- fold hh into minutes
            int hh = tag.substring(0, firstColon).toInt();
            mm = hh * 60 + tag.substring(firstColon + 1, secondColon).toInt();
            ss = tag.substring(secondColon + 1).toInt();
        }
        if (!found) {
            outSec = (uint16_t)(mm * 60 + ss);
            found = true;
        }
        line = line.substring(close + 1);
    }
    line.trim();
    return found;
}

// Splits a lyrics blob (as stored in a LYRICS/UNSYNCEDLYRICS Vorbis
// comment) into lines for the Lyrics screen. If the tag turns out to
// actually carry real LRC-style per-line timestamps (see
// stripLeadingTimestamp() above), those real seconds are used directly --
// genuine sync, not an approximation. Otherwise (plain "unsynced" text,
// no timestamps at all -- the common case the field name implies) lines
// are spread evenly across the track's real duration (line i at i/(n-1)
// of durSec) as a best-effort approximation, so the highlighted line at
// least advances over the course of the song instead of sitting on the
// last line the whole time.
std::vector<LyricLine> splitLyricsIntoLines(const String &text, uint16_t durSec) {
    std::vector<String> raw;
    std::vector<uint16_t> realSec;
    std::vector<bool> hasReal;
    int start = 0;
    for (int i = 0; i <= text.length(); i++) {
        if (i == text.length() || text[i] == '\n') {
            String line = text.substring(start, i);
            if (line.endsWith("\r")) line = line.substring(0, line.length() - 1);
            uint16_t sec = 0;
            bool found = stripLeadingTimestamp(line, sec);
            if (line.length() > 0) {
                raw.push_back(line);
                realSec.push_back(sec);
                hasReal.push_back(found);
            }
            start = i + 1;
        }
    }

    // Real sync if the clear majority of lines carried a parsed
    // timestamp -- a handful of untimed lines (e.g. one bare section
    // marker some taggers leave without a tag) shouldn't disqualify an
    // otherwise fully-timed file.
    size_t realCount = 0;
    for (bool h : hasReal) {
        if (h) realCount++;
    }
    bool mostlyTimed = !raw.empty() && realCount * 4 >= raw.size() * 3; // >=75%

    std::vector<LyricLine> lines;
    size_t n = raw.size();
    for (size_t i = 0; i < n; i++) {
        uint16_t atSec;
        if (mostlyTimed) {
            // A rare untimed line in an otherwise-timed file just holds
            // the previous line's timestamp rather than snapping to 0.
            atSec = hasReal[i] ? realSec[i] : (lines.empty() ? (uint16_t)0 : lines.back().atSec);
        } else {
            atSec = (n > 1 && durSec > 0) ? (uint16_t)((uint32_t)i * durSec / (n - 1)) : 0;
        }
        lines.push_back(LyricLine{atSec, raw[i]});
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
    //
    // THIRD startup-latency/correctness fix -- REVERTS part of the second
    // one. The second fix moved FlacMeta::readStreamInfo() to AFTER
    // AudioBridge::playSomething(), to avoid a double SD open. That
    // introduced a real, serious bug: readStreamInfo() was now opening the
    // SAME file a second time WHILE AudioBridge::playSomething()'s
    // connecttoFS() already had that exact file open and actively
    // decoding from it -- a genuine concurrent-file-access hazard. When
    // that caused readStreamInfo() to read corrupted data, it could
    // spuriously report a bogus bitsPerSample, falsely tripping the
    // 24-bit-unsupported skip below -- which calls playNextInQueue() ->
    // setNowPlaying() on the NEXT track WHILE STILL INSIDE THIS CALL. If
    // that next track's read got corrupted the same way, it recursed,
    // chewing through the entire queue in one synchronous burst until
    // empty. The last setNowPlaying() in that cascade had already reset
    // state.now.posSec to 0 at its own top; playNextInQueue()'s empty-
    // queue branch then sets state.now.playing = false and returns.
    // Exactly "goes back to zero and freezes" -- UI.cpp's
    // tickPlaybackClock() bails out immediately every tick once playing
    // is false, so nothing ever recovers.
    //
    // Fixed by moving ONLY the STREAMINFO read back to BEFORE
    // playSomething() -- the 24-bit check's false-positive cascade is
    // dangerous enough that correctness has to win over the minor latency
    // cost of one SD open before playback starts. Tags/art reading stay
    // deferred to AFTER playSomething() (same as the second fix) --
    // unlike STREAMINFO, a bad tags/art read has no skip/recursion path,
    // so it doesn't carry this same risk, and it hasn't been implicated
    // in any reported freeze.
    bool knownUnsupported = false;
    if (t.path.length() > 0) {
        FlacMeta::StreamInfo si;
        if (FlacMeta::readStreamInfo(t.path, si)) {
            float dur = FlacMeta::durationSec(si);
            if (dur > 0 && dur < 65536) t.durSec = (uint16_t)dur;
            // ESP32-audioI2S 3.0.12's FLAC decoder hard-requires 8 or
            // 16-bit samples -- a 24-bit file is guaranteed to fail.
            if (si.bitsPerSample != 0 && si.bitsPerSample != 8 && si.bitsPerSample != 16) {
                knownUnsupported = true;
                Serial.printf("[audio] \"%s\" is %u-bit FLAC -- this decoder only supports 8/16-bit, "
                              "skipping without attempting playback\n",
                              t.title.c_str(), (unsigned)si.bitsPerSample);
            }
        }
    }

    state.now.hasTrack = true;
    state.now.key = Library::keyFor(t); // filename-derived for now; corrected below once tags are read
    state.now.artist = t.artist;
    state.now.album = t.album;
    state.now.title = t.title;
    state.now.art = t.art;
    state.now.durSec = t.durSec;
    state.now.posSec = 0;
    state.now.playing = true;
    state.now.path = t.path;
    // See AppState.h's NowPlaying comment -- UI.cpp's tickPlaybackClock()
    // uses these to notice and skip a track that fails to actually start
    // decoding (some real files can't play at all, e.g. a FLAC frame too
    // large for the decoder's fixed buffer, or -- see knownUnsupported
    // above -- 24-bit samples) instead of silently stalling.
    state.now.playbackConfirmed = false;
    if (knownUnsupported) {
        // Don't even try -- backdate startedAtMs so UI.cpp's existing
        // grace-period check (unmodified) treats this as already-expired
        // on the very next tick, reusing the same non-recursive skip path
        // as a generic decode failure instead of a special-cased one.
        // Deliberately NOT a direct playNextInQueue() call here (unlike
        // the reverted version above) -- this just sets state and returns
        // normally; the actual skip happens on the NEXT tick, outside
        // this call stack entirely, so there's no recursion risk even if
        // every remaining queued track were somehow also 24-bit.
        state.now.startedAtMs = millis() - UI::kPlaybackStartGraceMs;
    } else {
        state.now.startedAtMs = millis();
        AudioBridge::playSomething(t.path); // real audio starts here, right after the one necessary SD read
    }

    // Everything past this point is slower, deferred work -- real
    // artist/title/album tags, lyrics, embedded cover art. Runs after
    // playback has already been kicked off above (still runs for a
    // knownUnsupported file too, matching the original behavior -- it'll
    // auto-skip on the very next tick regardless).
    if (t.path.length() > 0) {
        FlacMeta::Tags tags;
        if (FlacMeta::readTags(t.path, tags)) {
            if (tags.hasArtist) { t.artist = tags.artist; state.now.artist = t.artist; }
            if (tags.hasTitle)  { t.title = tags.title;   state.now.title = t.title; }
            if (tags.hasAlbum)  { t.album = tags.album;   state.now.album = t.album; }
            // BUG FIX (found auditing after a real-hardware lyrics regression):
            // state.now.key was computed ONCE, early, from filename-derived
            // artist/title (before this block ever runs). If the real tags
            // above differ at all from the filename guess -- extremely
            // common -- keyFor(t) here would return a DIFFERENT string than
            // state.now.key already holds, since t.artist/t.title just
            // changed. Lyrics would then get stored under a key that
            // drawLyrics()'s state.now.key lookup can never match, silently
            // losing lyrics for exactly the tracks whose real tags differ
            // from their filename. Recompute state.now.key here too, from
            // the now-tag-updated t, so both stay consistent -- whichever
            // key this track is ultimately looked up by is the same one
            // used to store its lyrics.
            state.now.key = Library::keyFor(t);
            if (tags.hasLyrics) {
                Library::LYRICS[state.now.key] = splitLyricsIntoLines(tags.lyrics, t.durSec);
            }
        }
        AlbumArt::loadForTrack(t.path); // no-op-safe if the file has no (or non-JPEG) embedded art
    } else {
        AlbumArt::clear(); // placeholder/mock track with no real path -- don't show the previous track's art
    }
    state.dirty = true; // picks up any tag/art updates made above
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
    std::vector<MenuItem> items;
    if (Library::usingIndex()) {
        // Cheap: a sequential read of the compact index file collecting
        // just distinct artist names, not the whole library's Track data
        // -- see Library.h for the full "why" writeup.
        for (auto &name : Library::indexArtists()) {
            MenuItem it;
            it.label = name;
            it.icon = "artist";
            it.action = [name]() { buildAlbumListFromIndex(name); };
            items.push_back(std::move(it));
        }
    } else {
        std::vector<String> artists;
        std::set<String> seen;
        for (auto &al : Library::ALBUMS) {
            if (seen.insert(al.artist).second) artists.push_back(al.artist);
        }
        for (auto &name : artists) {
            MenuItem it;
            it.label = name;
            it.icon = "artist";
            it.action = [name]() { buildAlbumList(name); };
            items.push_back(std::move(it));
        }
    }
    pushMenu("Music", std::move(items));
}

void buildAlbumListFromIndex(const String &artist) {
    std::vector<MenuItem> items;
    for (auto &kv : Library::indexAlbumsForArtist(artist)) {
        MenuItem it;
        it.label = kv.first;
        it.icon = "album";
        it.sub = String(kv.second) + " tracks";
        String artistCopy = artist;
        String albumCopy = kv.first;
        it.action = [artistCopy, albumCopy]() { buildTrackListFromIndex(artistCopy, albumCopy); };
        items.push_back(std::move(it));
    }
    pushMenu(artist, std::move(items));
}

void buildAlbumList(const String &artist) {
    std::vector<MenuItem> items;
    for (auto &al : Library::ALBUMS) {
        if (al.artist != artist) continue;
        MenuItem it;
        it.label = al.album;
        it.icon = "album";
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
        it.icon = "track";
        it.sub = fmtTime(t.durSec);
        it.isTrack = true;
        it.trackData = Track{album.artist, album.album, t.title, t.durSec, album.art, t.path};
        size_t idx = i;
        it.action = [albumPtr, idx]() { playAlbumFrom(*albumPtr, idx); };
        items.push_back(std::move(it));
    }
    pushMenu(album.album, std::move(items));
}

// Loads ONLY this one (artist, album) pair's tracks from the on-SD index
// -- not the whole library. The shared_ptr's last reference goes away
// with the row lambdas that captured it once this menu is replaced by
// whatever's opened next, so the data doesn't outlive the screen it's for.
void buildTrackListFromIndex(const String &artist, const String &album) {
    auto tracks = std::make_shared<std::vector<Track>>(Library::indexTracksForAlbum(artist, album));
    std::vector<MenuItem> items;
    items.reserve(tracks->size());
    for (size_t i = 0; i < tracks->size(); i++) {
        const Track &t = (*tracks)[i];
        MenuItem it;
        it.label = t.title;
        it.icon = "track";
        it.sub = fmtTime(t.durSec);
        it.isTrack = true;
        it.trackData = t;
        size_t idx = i;
        it.action = [tracks, idx]() { playQueueFrom(*tracks, idx); };
        items.push_back(std::move(it));
    }
    pushMenu(album, std::move(items));
}

// Loads ONLY this one playlist's tracks from the on-SD index -- the
// actual fix for "funky times" (425 tracks) sitting in RAM for the whole
// session: now it's only materialized while this menu is open, freed
// again once the user navigates elsewhere, and re-read from the index
// (a fast sequential file read, not a FAT walk) next time it's opened.
void buildPlaylistTrackListFromIndex(const String &name) {
    auto tracks = std::make_shared<std::vector<Track>>(Library::indexTracksForPlaylist(name));
    std::vector<MenuItem> items;
    items.reserve(tracks->size());
    for (size_t i = 0; i < tracks->size(); i++) {
        const Track &t = (*tracks)[i];
        MenuItem row;
        row.label = t.title;
        row.icon = "track";
        row.sub = t.artist;
        row.isTrack = true;
        row.trackData = t;
        size_t idx = i;
        row.action = [tracks, idx]() { playQueueFrom(*tracks, idx); };
        items.push_back(std::move(row));
    }
    pushMenu(name, std::move(items));
}

void buildPlaylistList() {
    std::vector<MenuItem> items;
    if (Library::usingIndex()) {
        for (auto &kv : Library::indexPlaylists()) {
            MenuItem it;
            it.label = kv.first;
            it.icon = "playlist";
            it.sub = String(kv.second) + " tracks";
            String plName = kv.first;
            it.action = [plName]() { buildPlaylistTrackListFromIndex(plName); };
            items.push_back(std::move(it));
        }
    } else {
        for (auto &p : Library::PLAYLISTS) {
            MenuItem it;
            it.label = p.name;
            it.icon = "playlist";
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
                    row.icon = "track";
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
    }
    pushMenu("Playlists", std::move(items));
}

void buildSettings() {
    std::vector<MenuItem> items(6);
    items[0].label = "Bluetooth";
    items[0].icon = "bt";
    items[0].subFn = btStatusLabel;
    items[0].action = []() { enterBluetooth(); };

    items[1].label = "Brightness";
    items[1].icon = "brightness";
    items[1].sub = String(state.brightness) + "%";
    items[1].isSlider = true;
    items[1].getInt = []() { return state.brightness; };
    items[1].setInt = [](int v) {
        state.brightness = constrain(v, 10, 100);
        Persist::save();
        Screens::applyBrightness(state.brightness); // real-hardware test, see Screens.h
    };

    items[2].label = "Sort tracks by";
    items[2].icon = "sort";
    items[2].sub = state.sortPref;
    items[2].isChoice = true;
    items[2].options = {"Artist", "Album"};
    items[2].getChoice = []() { return state.sortPref; };
    items[2].setChoice = [](const String &v) { state.sortPref = v; Persist::save(); };

    items[3].label = "Appearance";
    items[3].icon = "theme";
    items[3].sub = state.darkMode ? "Dark" : "Light";
    items[3].isChoice = true;
    items[3].options = {"Light", "Dark"};
    items[3].getChoice = []() { return state.darkMode ? String("Dark") : String("Light"); };
    items[3].setChoice = [](const String &v) { state.darkMode = (v == "Dark"); Persist::save(); };

    items[4].label = "Time zone";
    items[4].icon = "timezone";
    items[4].isSlider = true;
    items[4].sliderStep = 1; // hour offsets, not 0-100%
    items[4].getInt = []() { return state.utcOffsetHours; };
    items[4].setInt = [](int v) { state.utcOffsetHours = constrain(v, -12, 14); Persist::save(); };
    // subFn (not the plain `sub` adjustSlider() writes on adjust, which
    // always appends "%") formats this as "UTC+3"/"UTC-5"/"UTC+0" --
    // takes priority over `sub` per MenuItem::liveSub().
    items[4].subFn = []() {
        char buf[8];
        snprintf(buf, sizeof(buf), "UTC%+d", state.utcOffsetHours);
        return String(buf);
    };

    // Manual rescan: the on-SD index (see Library.h) is only ever built
    // once and trusted after that -- no auto-detection of SD content
    // changes, since re-walking the whole card to check would defeat the
    // point of caching it. This is how you tell it the card changed (new
    // music added/moved) and force a fresh index. Blocking (same ~20s-ish
    // FAT walk as the old boot-time scan), so it shows a direct busy
    // message first -- same reasoning as the boot splash fix, a silent
    // multi-second freeze looks exactly like a hang/crash otherwise.
    items[5].label = "Rescan library";
    items[5].icon = "rescan";
    items[5].sub = "";
    items[5].action = []() {
        Serial.println(F("[ui] manual library rescan requested"));
        Screens::showBusyMessage("Rescanning library...");
        Library::ensureIndex(/*force=*/true);
        // The menu stack may hold rows built from the pre-rescan index
        // (stale artist/album/playlist names, or now-dangling actions) --
        // safest to bounce back to the main menu rather than risk a
        // dangling selection into data that no longer matches the index.
        buildMainMenu();
    };

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

int activeLyricIndex(const std::vector<LyricLine> &lines) {
    int activeIdx = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        if (state.now.posSec >= lines[i].atSec) activeIdx = (int)i;
    }
    return activeIdx;
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
    items[0].icon = "bt";
    items[0].subFn = btStatusLabel;
    items[0].action = []() {
        if (!state.btOn) {
            BluetoothSource::begin(BluetoothSource::kTargetDeviceName);
            state.btOn = true; // optimistic; syncBluetoothToUi() corrects this next loop if begin() actually declined
            Persist::save();
            Serial.println(F("[ui] Bluetooth on, connecting..."));
            state.dirty = true;
        }
    };

    items[1].label = "Turn Bluetooth Off";
    items[1].icon = "bt";
    items[1].action = []() {
        BluetoothSource::end();
        state.btOn = false;
        state.btConnectedTo = "";
        Persist::save();
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
        if (Library::usingIndex()) {
            for (auto &kv : Library::indexPlaylists()) {
                MenuItem row;
                row.label = kv.first;
                row.icon = "playlist";
                row.sub = String(kv.second) + " tracks";
                String plName = kv.first;
                Track track2 = t;
                row.action = [plName, track2]() {
                    // Session-only overlay, never written to the on-SD
                    // index -- same as the old direct-vector-mutation
                    // behavior below, which also never persisted playlist
                    // edits across reboots. See Library.h.
                    Library::addToPlaylist(plName, track2);
                    Serial.printf("[ui] added to \"%s\"\n", plName.c_str());
                    closeTrackMenu();
                };
                plItems.push_back(std::move(row));
            }
        } else {
            for (auto &p : Library::PLAYLISTS) {
                MenuItem row;
                row.label = p.name;
                row.icon = "playlist";
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
    // "%"-suffixed by default (Brightness); rows with their own subFn
    // (e.g. Settings' Time zone) ignore this entirely -- liveSub() prefers
    // subFn over this plain `sub` field.
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
