#include "Library.h"

namespace Library {

std::vector<LibraryAlbum> ALBUMS = {
    {"Coral Static", "Nightbus Radio", '\x01', {
        {"", "", "Nightbus Radio", 214},
        {"", "", "Halide", 183},
        {"", "", "Low Tide Fever", 247},
    }},
    {"Coral Static", "Halide EP", '\x01', {
        {"", "", "Halide (Slow Version)", 201},
        {"", "", "Copper Wire", 166},
    }},
    {"Marrow Isle", "Salt & Static", '\x01', {
        {"", "", "Wire to Wire", 198},
        {"", "", "Salt & Static", 231},
        {"", "", "Ferry Song", 176},
        {"", "", "Departures", 264},
    }},
    {"Ninth Hour", "Late Signal", '\x01', {
        {"", "", "Late Signal", 209},
        {"", "", "Runoff", 188},
    }},
};

Track trackAt(size_t albumIndex, size_t trackIndex) {
    const LibraryAlbum &al = ALBUMS[albumIndex];
    const Track &t = al.tracks[trackIndex];
    return Track{al.artist, al.album, t.title, t.durSec, al.art};
}

String keyFor(const Track &t) { return t.artist + "|" + t.album + "|" + t.title; }

std::vector<Playlist> PLAYLISTS = {
    {"Late Drive", {trackAt(0, 0), trackAt(2, 1), trackAt(3, 0), trackAt(1, 0)}},
    {"Rainy Afternoon", {trackAt(0, 2), trackAt(2, 3), trackAt(3, 1)}},
};

std::vector<BtDevice> BT_DEVICES = {
    {"Kitchen Speaker", true},
    {"Workshop Buds", true},
    {"Unknown Receiver", false},
};

std::map<String, std::vector<LyricLine>> LYRICS = {
    {"Coral Static|Nightbus Radio|Nightbus Radio", {
        {0, "[instrumental intro]"},
        {8, "Riding the last bus out of town"},
        {13, "Streetlights blur as the engine slows down"},
        {19, "I left the porch light on for no one"},
        {25, "Static on the radio, nowhere to run"},
        {34, "Nightbus radio, take me home"},
        {40, "Nightbus radio, I'm not alone"},
        {52, "The driver hums a tune I half-know"},
        {58, "Windows fogged up from the cold"},
    }},
};

} // namespace Library
