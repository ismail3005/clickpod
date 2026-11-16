#pragma once

#include <Arduino.h>

String fmtTime(float totalSec);

bool hasAudioExtension(const String &name);

// Text as the screen can actually show it. The display font (TFT_eSPI's
// built-in GLCD font) only has plain ASCII glyphs that mean what they
// should: a UTF-8 "e-acute" in a tag came out as two unrelated symbols,
// and every such character also threw off anything laid out by string
// length (centering, right-aligned sub-labels). This folds accented Latin
// letters to their base letter, typographic quotes/dashes/ellipsis to
// their ASCII look-alikes, and anything else it can't represent to '?'
// -- one output character cell per input character (a few expand to two
// or three, e.g. "ae", "..."). Pure ASCII input comes back unchanged.
String asciiFold(const String &in);
