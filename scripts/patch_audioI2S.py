# Build-time patch for ESP32-audioI2S 3.0.12's FLAC maxFrameSize limitation
# (CLAUDE.md: "Open question, not attempted: real options for the two FLAC
# decode limitations" -- option 3, now attempted).
#
# Why a build script instead of forking the library: this session couldn't
# get write access to fork github.com/schreibfaul1/ESP32-audioI2S (out of
# this repo's scoped access), and vendoring the whole ~10K-line library into
# this repo just to change a few lines would mean maintaining a permanent
# fork by hand. Patching the library in place after PlatformIO downloads it
# (but before it compiles) keeps depending on the clean upstream 3.0.12 tag
# pinned in platformio.ini, stays entirely inside this repo, and needs no
# extra permissions or external repos.
#
# The bug being patched (confirmed by reading the real 3.0.12 source,
# github.com/schreibfaul1/ESP32-audioI2S/blob/3.0.12/src/Audio.cpp): when a
# FLAC file's declared max frame size exceeds the decoder's current input
# buffer threshold, the library just refuses to play the file
# ("FLAC maxFrameSize too large!") instead of growing the buffer to fit --
# even though it already grows that same buffer for every codec via
# InBuff.changeMaxBlockSize() elsewhere (initializeDecoder(), one call per
# codec), and even has a commented-out call to do exactly this sitting
# right there in read_FLAC_Header() (`//        InBuff.changeMaxBlockSize
# (m_flacMaxFrameSize);`) -- just placed AFTER the function's early
# `return -1`, so it can never actually run. The real backing buffer is
# PSRAM, hundreds of KB (confirmed: user's serial log shows
# "inputBufferSize: 638965 bytes") -- m_maxBlockSize is just a threshold
# the decode loop checks against, not a hard memory ceiling, so growing it
# to fit one specific file's real frame size (read straight from that
# file's own STREAMINFO block, not guessed) is safe.
#
# What this patch does: when the too-large check trips, grow the buffer to
# fit that file's real max frame size and continue, instead of refusing to
# play. No separate sanity cap needed beyond the nonzero check below --
# m_flacMaxFrameSize is itself declared `uint16_t` in Audio.h (confirmed by
# reading the real header), so it can never exceed 65535 regardless of what
# a corrupt/malicious file's STREAMINFO claims; changeMaxBlockSize() takes
# the same uint16_t type, so this can never overflow or truncate. Does NOT
# help the separate 24-bit-samples limitation (a different, intentional
# hard requirement in the same library, not a buffer-size issue) -- see
# CLAUDE.md.
#
# NOT verified on real hardware -- no pio run available in the sandbox this
# was written in, same caveat as everything else in this project done this
# way. Verify the very first real build: does it compile, and does a file
# that previously hit "FLAC maxFrameSize too large!" (e.g. a file whose
# logged FLAC maxFrameSize was a few KB over 16384) now play instead of
# skip? If the upstream library's source has drifted from what's expected
# here (shouldn't happen -- lib_deps pins the exact 3.0.12 tag), this
# script safely no-ops with a warning rather than corrupting the file.

Import("env")
import os

ORIGINAL = """        if(m_flacMaxFrameSize > InBuff.getMaxBlockSize()) {
            log_e("FLAC maxFrameSize too large!");
            stopSong();
            return -1;
        }"""

PATCHED = """        if(m_flacMaxFrameSize > InBuff.getMaxBlockSize()) {
            // clickpod patch (scripts/patch_audioI2S.py): grow the buffer to
            // fit this file's real max frame size instead of refusing to
            // play it -- see clickpod's CLAUDE.md for the full writeup.
            // m_flacMaxFrameSize is itself a uint16_t (Audio.h), so it can
            // never exceed 65535 no matter what a file's STREAMINFO claims --
            // no separate cap needed beyond the nonzero check.
            if(m_flacMaxFrameSize > 0) {
                InBuff.changeMaxBlockSize(m_flacMaxFrameSize);
                AUDIO_INFO("FLAC maxFrameSize %u exceeds default buffer, resized to fit", m_flacMaxFrameSize);
            } else {
                log_e("FLAC maxFrameSize too large!");
                stopSong();
                return -1;
            }
        }"""

MARKER = "clickpod patch (scripts/patch_audioI2S.py)"


def find_audio_cpp():
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pioenv = env.subst("$PIOENV")
    search_root = os.path.join(libdeps_dir, pioenv)
    if not os.path.isdir(search_root):
        return None
    for root, _dirs, files in os.walk(search_root):
        if "Audio.cpp" in files and "audioi2s" in root.lower():
            return os.path.join(root, "Audio.cpp")
    return None


def patch_flac_maxframesize():
    path = find_audio_cpp()
    if not path:
        print("[clickpod patch] ESP32-audioI2S Audio.cpp not found under "
              "PROJECT_LIBDEPS_DIR yet -- will retry next build once the "
              "library is installed (first build only)")
        return

    with open(path, "r") as f:
        content = f.read()

    if MARKER in content:
        print("[clickpod patch] FLAC maxFrameSize patch already applied to " + path)
        return

    if ORIGINAL not in content:
        print("[clickpod patch] WARNING: expected FLAC maxFrameSize code not "
              "found in " + path + " -- library source may have changed, "
              "patch NOT applied. See scripts/patch_audioI2S.py.")
        return

    content = content.replace(ORIGINAL, PATCHED)
    with open(path, "w") as f:
        f.write(content)
    print("[clickpod patch] FLAC maxFrameSize patch applied to " + path)


patch_flac_maxframesize()
