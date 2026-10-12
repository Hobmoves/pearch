#pragma once

// A spinning 3D pear rendered in text. The pear is a signed distance field
// (two blended spheres for the body, a capsule stem, an ellipsoid leaf),
// raymarched per character cell and shaded with the classic ".,-~:;=!*#$@"
// luminance ramp. It doubles as a progress bar: the pear fills with golden
// juice from the bottom up as indexing advances.

#include "tui/screen.h"

namespace tui {

struct PearParams {
    double time = 0;      // seconds, drives rotation and the liquid wave
    double progress = 0;  // 0..1 fill level
    double flash = 0;     // 0..1 completion flash / sparkles
    bool color = true;    // false: characters only (NO_COLOR)
};

// Draws the pear centred in the rectangle (x, y, w, h).
void draw_pear(Canvas& c, int x, int y, int w, int h, const PearParams& p);

}  // namespace tui
