#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>

namespace liquidock {

// Builds a D2D geometry from an SVG path string.
//
// Two logos have to appear in the header and neither exists in any font, so
// they arrive the way they are published: as path data. Hand-transcribing a
// forty-segment outline into AddBezier calls is how a logo ends up subtly wrong
// with nobody able to say why - this reads the published string instead, and
// the string can be pasted from the source unchanged.
//
// Supports the subset those paths use: M m L l H h V v C c S s A a Z z. An
// unrecognised command stops the parse and returns what was built so far, which
// fails as a missing icon rather than as a crash.
Microsoft::WRL::ComPtr<ID2D1PathGeometry> BuildPathGeometry(ID2D1Factory* factory,
                                                            const char* path);

} // namespace liquidock
