#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

// Shared data shapes for the UI layer (menus, tracks, playlists). Mirrors
// the object shapes used by the browser UI simulator 1:1 so the firmware
// port stays easy to cross-check against it.

struct Track {
    String artist;
    String album;
    String title;
    uint16_t durSec = 0;
    char art = '\x01'; // glyph key into Screens' icon set; '\x01' = generic note
    // Real SD path (e.g. "/Artist/Album/01 Song.flac") for tracks that came
    // from Library::scanFromSd(); empty for placeholder/mock tracks, which
    // makes AudioBridge fall back to "play whatever's first on the card"
    // instead of a specific file. No default member initializer needed --
    // String's own default constructor already gives "".
    String path;
};

struct LibraryAlbum {
    String artist;
    String album;
    char art = '\x01';
    std::vector<Track> tracks; // title + durSec + path; artist/album/art inherited from the album
};

struct Playlist {
    String name;
    std::vector<Track> tracks; // full track objects, so "Add to Playlist" can push an arbitrary track in
};

struct LyricLine {
    uint16_t atSec;
    String text;
};

// A single navigable list row. Mirrors the simulator's item shape:
// {label, sub, icon, action, isSlider, isChoice, isTrack, trackData}.
struct MenuItem {
    String label;
    String sub;                        // static sub-label; ignored if subFn is set
    std::function<String()> subFn;      // live sub-label (e.g. Bluetooth status), re-evaluated every render
    String icon;                        // icon key for the main-menu tile grid only

    std::function<void()> action;       // fired on RIGHT/CENTER for plain rows

    bool isSlider = false;
    std::function<int()> getInt;
    std::function<void(int)> setInt;

    bool isChoice = false;
    std::vector<String> options;
    std::function<String()> getChoice;
    std::function<void(const String &)> setChoice;

    // Marks a row as a real track (buildTrackList / playlist rows), making
    // it eligible for the LEFT-long-press track context menu. Rows like
    // "Music" or "Settings" aren't tracks and don't get it.
    bool isTrack = false;
    Track trackData;

    String liveSub() const { return subFn ? subFn() : sub; }
};

struct Menu {
    String title;
    std::vector<MenuItem> items;
    int selected = 0;
};
