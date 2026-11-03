// Calibrated GK-2A HRIT products (the "L2" set), built from the five received channels.
//
// Qt-free on purpose: everything here is plain C++ over arrays, so it can be run and checked on its own.
// gk2ahritimages.cpp supplies the channel counts, the calibration text from the HRIT image data function
// record, the navigation record, a coastline set and a land mask, and saves the pictures that come back.
//
// What the products are (and are not)
//   GK-2A's own L2 (cloud mask, cloud top height, SST, ...) is made from sixteen channels plus numerical
//   weather prediction fields. The HRIT broadcast has five channels and nothing else, so these are
//   threshold and band-difference products on calibrated brightness temperature and reflectance, in the
//   way they are normally done from a quick-look feed. They say so in their own legends.
//
// Calibration: the image data function record carries a piecewise linear table, "count:=value" lines
// with $HALFTONE, _NAME and _UNIT (CGMS LRIT/HRIT global specification). Brightness temperature (K) for
// the four infrared channels and albedo (%) for VI006. A channel without a usable table is not drawn
// from a guess: the products that need it are left out and the reason is returned.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace nadirstation::gk2a::l2 {

// ---- the label font: DejaVu Sans Mono Bold rendered to 10 x 19 bits, printable ASCII ----------------------
inline constexpr unsigned short kFont10x19[95][19]={
{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x000,0x000,0x030,0x030,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x0cc,0x0cc,0x0cc,0x0cc,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x036,0x066,0x06c,0x1ff,0x1ff,0x0cc,0x0d8,0x3fe,0x3fe,0x198,0x1b0,0x1b0,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x030,0x030,0x0fc,0x0fc,0x1f0,0x1f0,0x0fc,0x03c,0x03e,0x1be,0x1fc,0x0f8,0x030,0x030,0x000,0x000},{0x000,0x000,0x000,0x1c0,0x360,0x320,0x360,0x1c6,0x018,0x060,0x11e,0x012,0x033,0x012,0x01e,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x078,0x0f8,0x0c8,0x0c0,0x0e0,0x0e0,0x1f3,0x3be,0x39e,0x39e,0x1fe,0x0fe,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x030,0x030,0x030,0x030,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x018,0x038,0x030,0x030,0x070,0x060,0x060,0x060,0x060,0x070,0x030,0x030,0x038,0x018,0x000,0x000},{0x000,0x000,0x000,0x060,0x060,0x070,0x030,0x030,0x038,0x038,0x038,0x038,0x030,0x030,0x070,0x060,0x060,0x000,0x000},
{0x000,0x000,0x000,0x030,0x030,0x1b4,0x0f8,0x0f8,0x1b4,0x030,0x030,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x030,0x030,0x030,0x3fe,0x3fe,0x030,0x030,0x030,0x000,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x030,0x030,0x070,0x060,0x060,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x0f8,0x0f8,0x000,0x000,0x000,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x070,0x070,0x070,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x006,0x00c,0x00c,0x018,0x018,0x030,0x030,0x060,0x060,0x0c0,0x0c0,0x180,0x180,0x000,0x000,0x000},
{0x000,0x000,0x000,0x078,0x0fc,0x1cc,0x1ce,0x18e,0x1be,0x1be,0x18e,0x1ce,0x1cc,0x0fc,0x078,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x070,0x0f0,0x0b0,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x1fe,0x1fe,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x0f8,0x1fc,0x11c,0x00c,0x00c,0x01c,0x038,0x070,0x0e0,0x1c0,0x1fc,0x1fc,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x0f8,0x1fc,0x19c,0x00c,0x00c,0x078,0x07c,0x00c,0x00e,0x10e,0x1fc,0x0f8,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x01c,0x03c,0x07c,0x07c,0x0dc,0x0dc,0x19c,0x1fe,0x1fe,0x01c,0x01c,0x01c,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x1fc,0x1fc,0x180,0x180,0x1f8,0x1fc,0x19c,0x00e,0x00e,0x11c,0x1fc,0x0f8,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x078,0x0fc,0x0c4,0x1c0,0x1f8,0x1fc,0x1ce,0x1ce,0x1ce,0x1ce,0x0fc,0x078,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x1fe,0x1fe,0x00c,0x01c,0x018,0x038,0x038,0x030,0x070,0x070,0x060,0x0e0,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x078,0x0fc,0x1cc,0x18c,0x1cc,0x0f8,0x0fc,0x1cc,0x18e,0x1ce,0x1fc,0x0f8,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x0f8,0x1fc,0x1cc,0x18c,0x18e,0x1ce,0x1fe,0x0fe,0x00c,0x09c,0x1f8,0x0f0,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x070,0x070,0x070,0x000,0x000,0x070,0x070,0x070,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x070,0x070,0x070,0x000,0x000,0x070,0x070,0x070,0x060,0x060,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x006,0x01e,0x0f8,0x1e0,0x1c0,0x0f8,0x01e,0x006,0x000,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x1fe,0x1fe,0x000,0x000,0x1fe,0x1fe,0x000,0x000,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x100,0x1e0,0x0fc,0x01e,0x01e,0x0fc,0x1e0,0x100,0x000,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x078,0x0fc,0x08c,0x00c,0x01c,0x038,0x030,0x070,0x070,0x000,0x070,0x070,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x078,0x0fc,0x1c6,0x386,0x33e,0x37e,0x366,0x37e,0x33e,0x180,0x1c6,0x0fe,0x07c,0x000,0x000},{0x000,0x000,0x000,0x070,0x078,0x078,0x0f8,0x0dc,0x0dc,0x0cc,0x1fc,0x1fe,0x18e,0x186,0x386,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1f8,0x1fc,0x18e,0x18e,0x18c,0x1fc,0x1fc,0x18e,0x186,0x18e,0x1fe,0x1fc,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x03c,0x0fe,0x0e0,0x1c0,0x1c0,0x1c0,0x1c0,0x1c0,0x1c0,0x0e0,0x0fe,0x03c,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1f8,0x1fc,0x19c,0x18e,0x18e,0x18e,0x18e,0x18e,0x18e,0x19c,0x1fc,0x1f8,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x1fe,0x1fe,0x1c0,0x1c0,0x1c0,0x1fc,0x1fc,0x1c0,0x1c0,0x1c0,0x1fe,0x1fe,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1fe,0x1fe,0x1c0,0x1c0,0x1c0,0x1fc,0x1fc,0x1c0,0x1c0,0x1c0,0x1c0,0x1c0,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x07c,0x0fe,0x0e4,0x1c0,0x1c0,0x180,0x19e,0x1de,0x1c6,0x0e6,0x0fe,0x07c,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x18e,0x18e,0x18e,0x18e,0x18e,0x1fe,0x1fe,0x18e,0x18e,0x18e,0x18e,0x18e,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x1fc,0x1fc,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x1fc,0x1fc,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x07c,0x07c,0x01c,0x01c,0x01c,0x01c,0x01c,0x01c,0x01c,0x11c,0x1fc,0x0f8,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x18e,0x18c,0x19c,0x1b8,0x1f0,0x1f0,0x1f8,0x1d8,0x19c,0x18c,0x18e,0x186,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x0c0,0x0c0,0x0c0,0x0c0,0x0c0,0x0c0,0x0c0,0x0c0,0x0c0,0x0c0,0x0fe,0x0fe,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x1ce,0x1ce,0x1de,0x1de,0x1fe,0x1f6,0x1f6,0x1b6,0x186,0x186,0x186,0x186,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1c6,0x1c6,0x1c6,0x1e6,0x1e6,0x1b6,0x1b6,0x1be,0x19e,0x19e,0x18e,0x18e,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x078,0x0fc,0x1cc,0x18e,0x18e,0x18e,0x18e,0x18e,0x18e,0x1cc,0x0fc,0x078,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1fc,0x1fe,0x1ce,0x1ce,0x1ce,0x1fe,0x1fc,0x1c0,0x1c0,0x1c0,0x1c0,0x1c0,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x078,0x0fc,0x1cc,0x18e,0x18e,0x18e,0x18e,0x18e,0x18e,0x1cc,0x0fc,0x078,0x01c,0x00c,0x000,0x000},
{0x000,0x000,0x000,0x1f8,0x1fc,0x18e,0x18e,0x18c,0x1fc,0x1f8,0x19c,0x19c,0x18e,0x18e,0x187,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x078,0x1fc,0x1c4,0x180,0x1e0,0x0f8,0x07c,0x01e,0x00e,0x18e,0x1fc,0x0f8,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1fe,0x1fe,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x18e,0x18e,0x18e,0x18e,0x18e,0x18e,0x18e,0x18e,0x18e,0x1ce,0x1fc,0x0f8,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x386,0x18e,0x18e,0x1cc,0x1cc,0x0cc,0x0dc,0x0dc,0x0f8,0x078,0x078,0x078,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x307,0x307,0x306,0x336,0x3f6,0x1f6,0x1fe,0x1fe,0x1de,0x1ce,0x1ce,0x1cc,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x386,0x1ce,0x0dc,0x0f8,0x078,0x070,0x070,0x078,0x0fc,0x0cc,0x1ce,0x386,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x386,0x18e,0x1cc,0x0dc,0x0f8,0x078,0x070,0x030,0x030,0x030,0x030,0x030,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1fe,0x1fe,0x00e,0x01c,0x038,0x038,0x070,0x0e0,0x0e0,0x1c0,0x1fe,0x1fe,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x078,0x060,0x060,0x060,0x060,0x060,0x060,0x060,0x060,0x060,0x060,0x060,0x060,0x078,0x000,0x000},
{0x000,0x000,0x000,0x180,0x180,0x0c0,0x0c0,0x060,0x060,0x030,0x030,0x018,0x018,0x00c,0x00c,0x006,0x000,0x000,0x000},{0x000,0x000,0x000,0x0f0,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x0f0,0x000,0x000},
{0x000,0x000,0x000,0x070,0x0f8,0x0dc,0x186,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x3ff},
{0x000,0x000,0x0c0,0x060,0x030,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x0f8,0x0fc,0x08e,0x0fe,0x1fe,0x1ce,0x1ce,0x1fe,0x0fe,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1c0,0x1c0,0x1c0,0x1f8,0x1fc,0x1ce,0x1ce,0x1c6,0x1ce,0x1ce,0x1fc,0x1fc,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x07c,0x0fc,0x0e4,0x1c0,0x1c0,0x1c0,0x0e4,0x0fc,0x07c,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x00e,0x00e,0x00e,0x0ee,0x1fe,0x1de,0x18e,0x18e,0x18e,0x1de,0x1fe,0x0ee,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x078,0x0fc,0x1ce,0x1fe,0x1fe,0x180,0x1c6,0x1fe,0x07c,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x03e,0x03e,0x070,0x1fe,0x1fe,0x070,0x070,0x070,0x070,0x070,0x070,0x070,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x0fe,0x1fe,0x1de,0x18e,0x18e,0x18e,0x1de,0x1fe,0x0fe,0x08e,0x0fc,0x0f8,0x000},
{0x000,0x000,0x000,0x1c0,0x1c0,0x1c0,0x1f8,0x1fc,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x000,0x000,0x000,0x000},{0x000,0x000,0x030,0x030,0x030,0x000,0x0f0,0x0f0,0x030,0x030,0x030,0x030,0x030,0x1fe,0x1fe,0x000,0x000,0x000,0x000},
{0x000,0x000,0x038,0x038,0x038,0x000,0x0f8,0x0f8,0x038,0x038,0x038,0x038,0x038,0x038,0x038,0x038,0x1f0,0x1f0,0x000},{0x000,0x000,0x000,0x1c0,0x1c0,0x1c0,0x1ce,0x1dc,0x1f8,0x1f0,0x1f8,0x1d8,0x1dc,0x1ce,0x1ce,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x1e0,0x1e0,0x060,0x060,0x060,0x060,0x060,0x060,0x060,0x070,0x07e,0x03e,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x1ec,0x1fe,0x1b6,0x1b6,0x1b6,0x1b6,0x1b6,0x1b6,0x1b6,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x1f8,0x1fc,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x078,0x0fc,0x1ce,0x18e,0x18e,0x18e,0x1ce,0x0fc,0x078,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x1fc,0x1fc,0x1ce,0x1ce,0x1c6,0x1ce,0x1ce,0x1fc,0x1fc,0x1c0,0x1c0,0x1c0,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x0ee,0x1fe,0x1de,0x18e,0x18e,0x18e,0x1de,0x1fe,0x0ee,0x00e,0x00e,0x00e,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x0fe,0x0fe,0x0f0,0x0e0,0x0e0,0x0e0,0x0e0,0x0e0,0x0e0,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x0f8,0x0fc,0x1c4,0x1e0,0x0f8,0x01c,0x18c,0x1fc,0x0f8,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x060,0x060,0x1fc,0x1fc,0x060,0x060,0x060,0x060,0x070,0x07c,0x03c,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x1cc,0x1dc,0x1fc,0x0ec,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x18e,0x18e,0x1cc,0x0cc,0x0dc,0x0f8,0x078,0x078,0x070,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x303,0x306,0x336,0x1f6,0x1f6,0x1fe,0x1de,0x1dc,0x1cc,0x000,0x000,0x000,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x1ce,0x0dc,0x0f8,0x078,0x070,0x078,0x0fc,0x1cc,0x18e,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x000,0x000,0x000,0x186,0x18e,0x1cc,0x0cc,0x0fc,0x0f8,0x078,0x070,0x030,0x070,0x1e0,0x1c0,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x1fe,0x1fe,0x01c,0x038,0x030,0x060,0x0c0,0x1fe,0x1fe,0x000,0x000,0x000,0x000},{0x000,0x000,0x000,0x03c,0x030,0x030,0x030,0x030,0x030,0x070,0x1e0,0x070,0x030,0x030,0x030,0x030,0x030,0x03c,0x000},
{0x000,0x000,0x000,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030,0x030},{0x000,0x000,0x000,0x1e0,0x070,0x030,0x030,0x030,0x030,0x038,0x01c,0x030,0x030,0x030,0x030,0x030,0x070,0x1e0,0x000},
{0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x1e2,0x1fe,0x11c,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000},};

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
inline bool isnanf_(float v) { return v != v; }

// ---- calibration table -----------------------------------------------------------------------------------

struct Table {
    std::vector<std::pair<double, double>> points;   // key (count in the table's own scale) -> value
    std::string name, unit, text;
    int halftone = 0;                                // $HALFTONE: bits of the key scale (0 = not stated)
    bool usable() const { return points.size() >= 2; }
    bool kelvin() const {
        std::string u = unit;
        for (auto& c : u) c = char(std::toupper((unsigned char)c));
        return u.find("KELVIN") != std::string::npos || u == "K";
    }
    bool albedo() const {
        std::string u = unit;
        for (auto& c : u) c = char(std::toupper((unsigned char)c));
        return u.find("ALBEDO") != std::string::npos || u.find("REFLECT") != std::string::npos || u.find('%') != std::string::npos;
    }
    // Value at a key, linear between the listed points and flat outside them.
    double at(double key) const {
        if (points.empty()) return std::numeric_limits<double>::quiet_NaN();
        if (key <= points.front().first) return points.front().second;
        if (key > points.back().first + 0.5) return std::numeric_limits<double>::quiet_NaN();   // beyond the table: no data
        if (key >= points.back().first) return points.back().second;
        size_t lo = 0, hi = points.size() - 1;
        while (hi - lo > 1) { const size_t m = (lo + hi) / 2; if (points[m].first <= key) lo = m; else hi = m; }
        const double x0 = points[lo].first, x1 = points[hi].first;
        if (x1 <= x0) return points[lo].second;
        const double t = (key - x0) / (x1 - x0);
        return points[lo].second + t * (points[hi].second - points[lo].second);
    }
    // 10-bit image counts -> value. The key of a count is the count itself when the table is on a 10 bit scale or
    // wider (the CGMS convention, counts are not shifted), and the count scaled to the table's range when the
    // table is narrower (an 8-bit table for a 10-bit image).
    std::array<float, 1024> lut10() const {
        std::array<float, 1024> out{};
        double keyMax = 0.0;
        for (const auto& p : points) keyMax = std::max(keyMax, p.first);
        double scale = 1.0;
        if (halftone > 0 && halftone < 10) scale = double((1 << halftone) - 1) / 1023.0;
        else if (halftone == 0 && keyMax <= 255.0) scale = 255.0 / 1023.0;
        for (int c = 0; c < 1024; ++c) out[size_t(c)] = float(at(double(c) * scale));
        return out;
    }
};

// Reads the data definition block. Lines look like "$HALFTONE:=16", "_NAME:=INFRARED", "_UNIT:=KELVIN",
// "0:=352.79". The odd "$HALFTONE=:16" spelling that appears in some files is read too.
inline Table parseTable(const std::string& raw) {
    Table t;
    std::string text;
    for (char c : raw) text.push_back((c == '\r' || c == ';') ? '\n' : ((unsigned char)c >= 32 && (unsigned char)c < 127 ? c : (c == '\n' ? '\n' : ' ')));
    t.text = text;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        size_t sep = line.find(":=");
        size_t w = 2;
        if (sep == std::string::npos) { sep = line.find("=:"); }
        if (sep == std::string::npos) { if (end >= text.size()) break; continue; }
        std::string key = line.substr(0, sep), value = line.substr(sep + w);
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        key = trim(key); value = trim(value);
        if (key == "$HALFTONE" || key == "HALFTONE") t.halftone = std::atoi(value.c_str());
        else if (key == "_NAME") t.name = value;
        else if (key == "_UNIT") t.unit = value;
        else if (!key.empty() && key.find_first_not_of("0123456789.") == std::string::npos && !value.empty()) {
            char* e = nullptr;
            const double v = std::strtod(value.c_str(), &e);
            if (e != value.c_str()) t.points.emplace_back(std::atof(key.c_str()), v);
        }
        if (end >= text.size()) break;
    }
    std::sort(t.points.begin(), t.points.end());
    // drop repeated keys, the later one wins nothing: keep the first
    t.points.erase(std::unique(t.points.begin(), t.points.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), t.points.end());
    return t;
}

// ---- geometry ----------------------------------------------------------------------------------------------

struct Nav {
    double subLonDeg = 128.2;
    double cfac = 10212669.0, lfac = -10212669.0, coff = 1375.25, loff = 1375.25;   // IR 2750 x 2750, as broadcast in the HRIT headers
    bool fromFile = false;
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kRs = 42164.0;          // km, satellite distance from the centre of the Earth
constexpr double kReq = 6378.169, kRpol = 6356.5838;
constexpr double kEcc = 1.006739501;     // (req / rpol)^2

// Pixel (1-based column and line of the CGMS convention) -> latitude, longitude. False off the Earth.
inline bool pixelToGeo(const Nav& n, double col, double line, double& latDeg, double& lonDeg) {
    // KMA broadcasts LFAC negated; the image runs north to south and west to east, so the magnitudes are what count.
    const double x = (col - n.coff) * 65536.0 / std::fabs(n.cfac) * kPi / 180.0;
    const double y = (line - n.loff) * 65536.0 / std::fabs(n.lfac) * kPi / 180.0;
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
    const double den = cy * cy + kEcc * sy * sy;
    const double disc = (kRs * cx * cy) * (kRs * cx * cy) - den * (kRs * kRs - kReq * kReq);
    if (disc <= 0.0) return false;
    const double sd = std::sqrt(disc);
    const double sn = (kRs * cx * cy - sd) / den;
    const double s1 = kRs - sn * cx * cy, s2 = sn * sx * cy, s3 = -sn * sy;
    const double sxy = std::sqrt(s1 * s1 + s2 * s2);
    lonDeg = std::atan2(s2, s1) * 180.0 / kPi + n.subLonDeg;
    latDeg = std::atan(kEcc * s3 / sxy) * 180.0 / kPi;
    return true;
}

// Latitude, longitude -> column and line (1-based). False when the point is on the far side.
inline bool geoToPixel(const Nav& n, double latDeg, double lonDeg, double& col, double& line) {
    const double lat = latDeg * kPi / 180.0, dl = (lonDeg - n.subLonDeg) * kPi / 180.0;
    const double cl = std::atan(kRpol * kRpol / (kReq * kReq) * std::tan(lat));
    const double re = kRpol / std::sqrt(1.0 - (kReq * kReq - kRpol * kRpol) / (kReq * kReq) * std::cos(cl) * std::cos(cl));
    const double r1 = kRs - re * std::cos(cl) * std::cos(dl);
    const double r2 = -re * std::cos(cl) * std::sin(dl);
    const double r3 = re * std::sin(cl);
    if (r1 <= 0.0) return false;
    // horizon test in terms of the line of sight: the point is hidden if the sight line enters the Earth first.
    const double px = re * std::cos(cl) * std::cos(dl), py = re * std::cos(cl) * std::sin(dl), pz = re * std::sin(cl);
    const double vx = kRs - px, vy = -py, vz = -pz;                    // point -> satellite
    const double nx = std::cos(cl) * std::cos(dl), ny = std::cos(cl) * std::sin(dl), nz = std::sin(cl);
    if (vx * nx + vy * ny + vz * nz <= 0.0) return false;
    const double x = std::atan(-r2 / r1);
    const double y = std::asin(-r3 / std::sqrt(r1 * r1 + r2 * r2 + r3 * r3));
    col = n.coff + x * 180.0 / kPi * std::fabs(n.cfac) / 65536.0;
    line = n.loff + y * 180.0 / kPi * std::fabs(n.lfac) / 65536.0;
    return true;
}

struct Sun {
    double declRad = 0.0, eotMin = 0.0, distFactor = 1.0, utcHours = 0.0;   // distFactor = (1 AU / distance)^2
};
inline Sun sunAt(std::time_t utc) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &utc);
#else
    gmtime_r(&utc, &tm);
#endif
    const double doy = tm.tm_yday + 1 + (tm.tm_hour + tm.tm_min / 60.0 + tm.tm_sec / 3600.0) / 24.0;
    const double b = 2.0 * kPi * (doy - 1.0) / 365.0;
    Sun s;
    s.declRad = 0.006918 - 0.399912 * std::cos(b) + 0.070257 * std::sin(b) - 0.006758 * std::cos(2 * b) +
                0.000907 * std::sin(2 * b) - 0.002697 * std::cos(3 * b) + 0.00148 * std::sin(3 * b);
    s.eotMin = 229.18 * (0.000075 + 0.001868 * std::cos(b) - 0.032077 * std::sin(b) - 0.014615 * std::cos(2 * b) - 0.04089 * std::sin(2 * b));
    s.distFactor = 1.000110 + 0.034221 * std::cos(b) + 0.001280 * std::sin(b) + 0.000719 * std::cos(2 * b) + 0.000077 * std::sin(2 * b);
    s.utcHours = tm.tm_hour + tm.tm_min / 60.0 + tm.tm_sec / 3600.0;
    return s;
}
inline double cosSolarZenith(const Sun& s, double latDeg, double lonDeg) {
    const double hourAngle = ((s.utcHours + s.eotMin / 60.0) * 15.0 + lonDeg - 180.0) * kPi / 180.0;
    const double lat = latDeg * kPi / 180.0;
    return std::sin(lat) * std::sin(s.declRad) + std::cos(lat) * std::cos(s.declRad) * std::cos(hourAngle);
}

// ---- radiance -------------------------------------------------------------------------------------------------

// W m-2 sr-1 um-1 at wavelength lambda (um) for temperature T (K)
inline double planck(double lambdaUm, double tK) {
    return 1.191042e8 / (std::pow(lambdaUm, 5.0) * (std::exp(1.4387752e4 / (lambdaUm * tK)) - 1.0));
}

// ---- pictures -----------------------------------------------------------------------------------------------------

struct Rgb {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;   // RGB, row-major
    Rgb() = default;
    Rgb(int width, int height) : w(width), h(height), px(size_t(width) * size_t(height) * 3, 0) {}
    std::uint8_t* at(int x, int y) { return px.data() + (size_t(y) * size_t(w) + size_t(x)) * 3; }
    void set(int x, int y, int r, int g, int b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        auto* p = at(x, y); p[0] = std::uint8_t(r); p[1] = std::uint8_t(g); p[2] = std::uint8_t(b);
    }
    void blend(int x, int y, int r, int g, int b, double a) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        auto* p = at(x, y);
        p[0] = std::uint8_t(p[0] + (r - p[0]) * a); p[1] = std::uint8_t(p[1] + (g - p[1]) * a); p[2] = std::uint8_t(p[2] + (b - p[2]) * a);
    }
    void rect(int x0, int y0, int x1, int y1, int r, int g, int b) {
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y) for (int x = std::max(0, x0); x < std::min(w, x1); ++x) set(x, y, r, g, b);
    }
};

inline int textWidth(const std::string& s, int scale) { return int(s.size()) * 10 * scale; }
inline void drawText(Rgb& img, int x, int y, const std::string& s, int scale, int r, int g, int b) {
    for (size_t k = 0; k < s.size(); ++k) {
        int c = (unsigned char)s[k];
        if (c < 32 || c > 126) c = '?';
        const auto& glyph = kFont10x19[c - 32];
        for (int gy = 0; gy < 19; ++gy)
            for (int gx = 0; gx < 10; ++gx)
                if ((glyph[gy] >> (9 - gx)) & 1)
                    for (int sy = 0; sy < scale; ++sy) for (int sx = 0; sx < scale; ++sx)
                        img.set(x + int(k) * 10 * scale + gx * scale + sx, y + gy * scale + sy, r, g, b);
    }
}

struct Stop { float v; std::uint8_t r, g, b; };
struct Palette {
    std::vector<Stop> stops;
    std::array<std::array<std::uint8_t, 3>, 1024> lut{};
    float lo = 0, hi = 1;
    Palette() = default;
    Palette(std::vector<Stop> s) : stops(std::move(s)) {
        lo = stops.front().v; hi = stops.back().v;
        for (int i = 0; i < 1024; ++i) lut[size_t(i)] = eval(lo + (hi - lo) * float(i) / 1023.0f);
    }
    std::array<std::uint8_t, 3> eval(float v) const {
        if (stops.front().v < stops.back().v) {
            if (v <= stops.front().v) return {stops.front().r, stops.front().g, stops.front().b};
            if (v >= stops.back().v) return {stops.back().r, stops.back().g, stops.back().b};
            for (size_t i = 1; i < stops.size(); ++i)
                if (v <= stops[i].v) {
                    const float t = (v - stops[i - 1].v) / (stops[i].v - stops[i - 1].v);
                    return {std::uint8_t(stops[i - 1].r + t * (stops[i].r - stops[i - 1].r) + 0.5f),
                            std::uint8_t(stops[i - 1].g + t * (stops[i].g - stops[i - 1].g) + 0.5f),
                            std::uint8_t(stops[i - 1].b + t * (stops[i].b - stops[i - 1].b) + 0.5f)};
                }
        }
        return {0, 0, 0};
    }
    std::array<std::uint8_t, 3> at(float v) const {
        const float t = (v - lo) / (hi - lo);
        const int i = std::clamp(int(t * 1023.0f + 0.5f), 0, 1023);
        return lut[size_t(i)];
    }
};

constexpr int kPanel = 112;   // height of the legend strip under every picture

// ---- inputs and outputs ----------------------------------------------------------------------------------------------

struct Channel {
    const char* name = "";
    int width = 0, height = 0;          // the whole image, all ten segments
    const std::uint16_t* counts = nullptr;   // 16-bit grey as the images are stored: 10-bit counts * 65535 / 1023
    std::size_t stride = 0;             // in samples
    std::array<bool, 10> segment{};     // which of the ten segments arrived
    Table table;
    bool present() const { return counts != nullptr; }
};

struct Inputs {
    Channel vi, sw, wv, ir, sp;         // VI006, SW038, WV069, IR105, IR123
    Nav nav;                            // the IR navigation record (2750 x 2750)
    std::time_t utc = 0;                // observation time
    std::string timeText;               // "2026-11-01 15:06 UTC"
    std::function<int(double, double)> land;                       // 1 land, 0 sea
    std::vector<std::vector<std::pair<float, float>>> lines;      // coast and border polylines, (lon, lat) degrees
    bool overlay = true;
};

struct Product {
    std::string file, title, note;
    Rgb image;
};
struct Result {
    std::vector<Product> products;
    std::vector<std::string> skipped;   // "name: reason"
    std::vector<std::string> notes;
    double irMedianK = 0, irMinK = 0, irMaxK = 0;
    std::size_t diskPixels = 0;
};

// ---- the work --------------------------------------------------------------------------------------------------

namespace detail {

inline float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline std::uint8_t b8(float v) { return std::uint8_t(std::clamp(int(v * 255.0f + 0.5f), 0, 255)); }

// counts of the channel at 10 bits
inline int count10(std::uint16_t g) { return int((std::uint32_t(g) * 1023u + 32767u) / 65535u); }

// Brightness temperature on the IR grid (n x n). NaN where the segment is missing.
inline std::vector<float> irGrid(const Channel& c, int n, const std::array<float, 1024>& lut) {
    std::vector<float> out(size_t(n) * size_t(n), kNaN);
    const int segRows = n / 10;
    for (int y = 0; y < n; ++y) {
        if (!c.segment[size_t(std::min(9, y / segRows))]) continue;
        const std::uint16_t* src = c.counts + size_t(y) * c.stride;
        float* dst = out.data() + size_t(y) * size_t(n);
        for (int x = 0; x < n; ++x) dst[x] = lut[size_t(count10(src[x]))];
    }
    return out;
}

// VI006 albedo (fraction) averaged 4 x 4 onto the IR grid.
inline std::vector<float> visGrid(const Channel& c, int n, const std::array<float, 1024>& lut, float scale) {
    std::vector<float> out(size_t(n) * size_t(n), kNaN);
    const int f = c.width / n;
    const int segRows = n / 10;
    for (int y = 0; y < n; ++y) {
        if (!c.segment[size_t(std::min(9, y / segRows))]) continue;
        float* dst = out.data() + size_t(y) * size_t(n);
        for (int x = 0; x < n; ++x) {
            double sum = 0.0; int cnt = 0;
            for (int j = 0; j < f; ++j) {
                const std::uint16_t* src = c.counts + size_t(y * f + j) * c.stride + size_t(x * f);
                for (int i = 0; i < f; ++i) { const float v = lut[size_t(count10(src[i]))]; if (!isnanf_(v)) { sum += v; ++cnt; } }
            }
            dst[x] = cnt ? float(sum / cnt) * scale : kNaN;
        }
    }
    return out;
}

struct Canvas {
    Rgb img;
    int n = 0;
    Canvas(int size) : img(size, size + kPanel), n(size) {
        img.rect(0, size, size, size + kPanel, 18, 20, 26);
    }
};

inline void drawOverlay(Canvas& cv, const Inputs& in) {
    if (!in.overlay) return;
    const int n = cv.n;
    for (const auto& poly : in.lines) {
        bool have = false; double px = 0, py = 0;
        for (const auto& pt : poly) {
            double c, l;
            if (!geoToPixel(in.nav, pt.second, pt.first, c, l)) { have = false; continue; }
            const double x = c - 1.0, y = l - 1.0;
            if (have && std::fabs(x - px) < 80 && std::fabs(y - py) < 80) {
                const int steps = int(std::max(std::fabs(x - px), std::fabs(y - py))) + 1;
                for (int s = 0; s <= steps; ++s) {
                    const double t = double(s) / steps;
                    const int ix = int(std::lround(px + (x - px) * t)), iy = int(std::lround(py + (y - py) * t));
                    if (ix >= 0 && iy >= 0 && ix < n && iy < n) cv.img.blend(ix, iy, 255, 240, 120, 0.55);
                }
            }
            px = x; py = y; have = true;
        }
    }
}

inline void drawTitle(Canvas& cv, const std::string& title, const std::string& line2, const std::string& line3) {
    const int y0 = cv.n;
    drawText(cv.img, 14, y0 + 6, title, 2, 235, 238, 245);
    drawText(cv.img, 14, y0 + 6 + 40, line2, 1, 175, 182, 196);
    drawText(cv.img, 14, y0 + 6 + 40 + 21, line3, 1, 130, 138, 152);
}

// Colour bar with tick labels, in the right half of the strip.
inline void drawBar(Canvas& cv, const Palette& pal, float lo, float hi, const std::vector<float>& ticks,
                    const std::string& unit, bool reversed = false) {
    const int barW = std::min(cv.n / 2 - 40, 1100), barH = 22;
    const int x0 = cv.n - barW - 30, y0 = cv.n + 26;
    for (int x = 0; x < barW; ++x) {
        const float t = float(x) / float(barW - 1);
        const float v = reversed ? hi + (lo - hi) * t : lo + (hi - lo) * t;
        const auto c = pal.eval(v);
        for (int y = 0; y < barH; ++y) cv.img.set(x0 + x, y0 + y, c[0], c[1], c[2]);
    }
    cv.img.rect(x0 - 1, y0 - 1, x0 + barW + 1, y0, 200, 200, 210);
    cv.img.rect(x0 - 1, y0 + barH, x0 + barW + 1, y0 + barH + 1, 200, 200, 210);
    for (float tick : ticks) {
        const float t = (tick - lo) / (hi - lo);
        const int x = x0 + int((reversed ? 1.0f - t : t) * float(barW - 1));
        cv.img.rect(x, y0 + barH, x + 1, y0 + barH + 7, 220, 220, 230);
        char buf[24]; std::snprintf(buf, sizeof buf, "%g", double(tick));
        const std::string s = buf;
        drawText(cv.img, x - textWidth(s, 1) / 2, y0 + barH + 9, s, 1, 215, 220, 230);
    }
    drawText(cv.img, x0, y0 - 24, unit, 1, 175, 182, 196);
}

struct ClassEntry { std::uint8_t r, g, b; const char* label; };
inline void drawClassLegend(Canvas& cv, const std::vector<ClassEntry>& classes) {
    const int rows = 3;
    const int x0 = cv.n / 2 + 90;
    for (size_t i = 0; i < classes.size(); ++i) {
        const int col = int(i) / rows, row = int(i) % rows;
        const int x = x0 + col * 350, y = cv.n + 10 + row * 31;
        cv.img.rect(x, y, x + 22, y + 22, classes[i].r, classes[i].g, classes[i].b);
        drawText(cv.img, x + 30, y + 1, classes[i].label, 1, 215, 220, 230);
    }
}

}  // namespace detail

// Builds every product the inputs allow.
inline Result makeProducts(const Inputs& in) {
    using namespace detail;
    Result res;
    const int n = in.ir.present() ? in.ir.width : 0;
    if (n <= 0 || in.ir.height != n || in.ir.width % 10 != 0) { res.skipped.push_back("all: IR105 missing or not square"); return res; }

    // ---- calibration per channel ---------------------------------------------------------------------------
    struct Cal { bool ok = false; std::array<float, 1024> lut{}; std::string why; };
    auto kelvinCal = [&](const Channel& c) {
        Cal k;
        if (!c.present()) { k.why = std::string(c.name) + " not received"; return k; }
        if (c.width != n || c.height != n) { k.why = std::string(c.name) + " has unexpected size"; return k; }
        if (!c.table.usable()) { k.why = std::string(c.name) + " has no calibration table in its header"; return k; }
        if (!c.table.kelvin()) { k.why = std::string(c.name) + " table is not in kelvin (unit '" + c.table.unit + "')"; return k; }
        k.lut = c.table.lut10();
        for (auto& v : k.lut) if (!(v > 60.0f && v < 400.5f)) v = kNaN;   // 49 K style entries mark counts that carry no data
        k.ok = true;
        return k;
    };
    const Cal calIr = kelvinCal(in.ir), calSp = kelvinCal(in.sp), calSw = kelvinCal(in.sw), calWv = kelvinCal(in.wv);
    Cal calVi;
    float visScale = 1.0f;
    if (!in.vi.present()) calVi.why = "VI006 not received";
    else if (in.vi.width != n * (in.vi.width / n) || in.vi.width / n < 1 || in.vi.height != in.vi.width) calVi.why = "VI006 has unexpected size";
    else if (!in.vi.table.usable()) calVi.why = "VI006 has no calibration table in its header";
    else if (!in.vi.table.albedo()) calVi.why = "VI006 table is not albedo (unit '" + in.vi.table.unit + "')";
    else {
        calVi.lut = in.vi.table.lut10();
        double mx = 0; for (float v : calVi.lut) if (!isnanf_(v)) mx = std::max(mx, double(v));
        visScale = (in.vi.table.unit.find('%') != std::string::npos || mx > 1.5) ? 0.01f : 1.0f;
        calVi.ok = true;
    }

    if (!calIr.ok) { res.skipped.push_back("all: " + calIr.why); return res; }

    // ---- geometry: latitude, longitude, solar zenith per pixel -------------------------------------------------------
    const Sun sun = sunAt(in.utc);
    std::vector<float> lat(size_t(n) * size_t(n), kNaN), lon(size_t(n) * size_t(n), kNaN), cosz(size_t(n) * size_t(n), kNaN);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double la, lo;
            if (!pixelToGeo(in.nav, x + 1.0, y + 1.0, la, lo)) continue;
            const size_t i = size_t(y) * size_t(n) + size_t(x);
            lat[i] = float(la); lon[i] = float(lo);
            cosz[i] = float(cosSolarZenith(sun, la, lo));
        }

    // ---- brightness temperatures and albedo on one grid ---------------------------------------------------------------
    std::vector<float> bIr = irGrid(in.ir, n, calIr.lut);
    std::vector<float> bSp = calSp.ok ? irGrid(in.sp, n, calSp.lut) : std::vector<float>();
    std::vector<float> bSw = calSw.ok ? irGrid(in.sw, n, calSw.lut) : std::vector<float>();
    std::vector<float> bWv = calWv.ok ? irGrid(in.wv, n, calWv.lut) : std::vector<float>();
    std::vector<float> alb = calVi.ok ? visGrid(in.vi, n, calVi.lut, visScale) : std::vector<float>();

    // sanity of the calibration: the median of the disk should be an Earth temperature
    {
        std::vector<float> sample;
        for (size_t i = 0; i < bIr.size(); i += 97) if (!isnanf_(lat[i]) && !isnanf_(bIr[i])) sample.push_back(bIr[i]);
        res.diskPixels = 0;
        for (size_t i = 0; i < lat.size(); ++i) if (!isnanf_(lat[i])) ++res.diskPixels;
        if (sample.size() > 100) {
            std::sort(sample.begin(), sample.end());
            res.irMedianK = sample[sample.size() / 2];
            res.irMinK = sample[size_t(sample.size() * 0.001)];
            res.irMaxK = sample[size_t(sample.size() * 0.999)];
            if (res.irMedianK < 200 || res.irMedianK > 320)
                res.notes.push_back("IR105 median brightness temperature is implausible (" + std::to_string(int(res.irMedianK)) + " K): check the calibration table");
        }
    }

    // pixel state: 0 off the Earth, 1 usable, bit per channel for missing rows
    auto rowMask = [&](int y) {
        const int seg = std::min(9, y / (n / 10));
        unsigned m = 0;
        if (in.vi.present() && in.vi.segment[size_t(seg)]) m |= 1;
        if (in.sw.present() && in.sw.segment[size_t(seg)]) m |= 2;
        if (in.wv.present() && in.wv.segment[size_t(seg)]) m |= 4;
        if (in.ir.present() && in.ir.segment[size_t(seg)]) m |= 8;
        if (in.sp.present() && in.sp.segment[size_t(seg)]) m |= 16;
        return m;
    };
    std::vector<unsigned> masks(static_cast<size_t>(n));
    for (int y = 0; y < n; ++y) masks[size_t(y)] = rowMask(y);

    bool anyMissing = false;
    for (const Channel* c : {&in.vi, &in.sw, &in.wv, &in.ir, &in.sp})
        if (c->present()) for (bool b : c->segment) if (!b) anyMissing = true;
    const std::string when = anyMissing ? in.timeText + "   dark red = segment not received" : in.timeText;
    auto newProduct = [&](const char* file, const char* title, const char* note) {
        res.products.emplace_back();
        res.products.back().file = file; res.products.back().title = title; res.products.back().note = note;
        return &res.products.back();
    };

    // generic painter: f(i, y) returns an rgb triple for pixel i when usable; need = channels the picture needs
    auto paint = [&](Canvas& cv, unsigned need, auto&& f) {
        for (int y = 0; y < n; ++y) {
            const bool rowOk = (masks[size_t(y)] & need) == need;
            for (int x = 0; x < n; ++x) {
                const size_t i = size_t(y) * size_t(n) + size_t(x);
                std::uint8_t* p = cv.img.at(x, y);
                if (isnanf_(lat[i])) { p[0] = p[1] = p[2] = 0; continue; }
                if (!rowOk) { p[0] = 52; p[1] = 20; p[2] = 24; continue; }
                const auto c = f(i);
                p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
            }
        }
    };
    using C3 = std::array<std::uint8_t, 3>;
    auto grey = [](float v) { const auto g = b8(v); return C3{g, g, g}; };

    // ---- 1. IR 10.5 um, cloud-top temperature ---------------------------------------------------------------------------
    {
        const Palette pal({{183, 255, 255, 255}, {193, 200, 0, 200}, {203, 230, 0, 0}, {213, 255, 140, 0}, {223, 255, 240, 0},
                           {233, 0, 225, 120}, {243, 0, 170, 230}, {253, 40, 90, 255}, {263, 235, 235, 245}, {273, 200, 200, 205},
                           {300, 70, 70, 74}, {330, 6, 6, 8}});
        Product* pr = newProduct("GK2A_L2_CloudTopTemp_IR105.png", "IR 10.5 um brightness temperature (cloud-top temp.)",
                                 "calibrated IR105 brightness temperature, colour enhanced");
        Canvas cv(n);
        paint(cv, 8, [&](size_t i) -> C3 { const float t = bIr[i]; return isnanf_(t) ? C3{0, 0, 0} : pal.at(t); });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "GK-2A AMI IR105, calibrated with the table in the HRIT header", when);
        drawBar(cv, pal, 183, 330, {190, 200, 220, 240, 260, 273, 290, 310, 330}, "K", true);
        pr->image = std::move(cv.img);
    }

    // ---- 2. water vapour 6.9 um -----------------------------------------------------------------------------------------
    if (calWv.ok) {
        const Palette pal({{205, 255, 255, 255}, {212, 20, 70, 200}, {220, 60, 170, 245}, {228, 170, 230, 245}, {236, 250, 245, 205},
                           {244, 235, 170, 70}, {252, 120, 60, 10}, {262, 40, 20, 5}});
        Product* pr = newProduct("GK2A_L2_WaterVapor_WV069.png", "Water vapour 6.9 um brightness temperature",
                                 "calibrated WV069 brightness temperature: cold = moist or high, warm = dry");
        Canvas cv(n);
        paint(cv, 4, [&](size_t i) -> C3 { const float t = bWv[i]; return isnanf_(t) ? C3{0, 0, 0} : pal.at(t); });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "GK-2A AMI WV069, calibrated with the table in the HRIT header", when);
        drawBar(cv, pal, 205, 262, {210, 220, 230, 240, 250, 260}, "K", true);
        pr->image = std::move(cv.img);
    } else res.skipped.push_back("WaterVapor: " + calWv.why);

    // ---- 3. split window 10.5 - 12.3 -------------------------------------------------------------------------------------
    if (calSp.ok) {
        const Palette pal({{-2, 20, 60, 200}, {0, 90, 100, 120}, {1, 190, 190, 190}, {2, 250, 235, 60}, {4, 250, 120, 20}, {6, 200, 20, 20}});
        Product* pr = newProduct("GK2A_L2_SplitWindow_BTD_105-123.png", "Split window: BT10.5 minus BT12.3",
                                 "thin cirrus, dust and moist boundary layers show as large positive differences");
        Canvas cv(n);
        paint(cv, 8 | 16, [&](size_t i) -> C3 { const float d = bIr[i] - bSp[i]; return isnanf_(d) ? C3{0, 0, 0} : pal.at(std::clamp(d, -2.0f, 6.0f)); });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "IR105 minus IR123, both calibrated", when);
        drawBar(cv, pal, -2, 6, {-2, 0, 1, 2, 3, 4, 5, 6}, "K", false);
        pr->image = std::move(cv.img);
    } else res.skipped.push_back("SplitWindow: " + calSp.why);

    // ---- 4. WV - IR, overshooting tops --------------------------------------------------------------------------------------
    if (calWv.ok) {
        const Palette pal({{-40, 15, 15, 70}, {-20, 50, 100, 190}, {-8, 150, 190, 235}, {-2, 235, 235, 235}, {0, 255, 205, 0}, {3, 255, 90, 0}, {8, 220, 0, 0}});
        Product* pr = newProduct("GK2A_L2_ConvectiveTops_BTD_WV069-IR105.png", "Convective tops: BT6.9 minus BT10.5",
                                 "values near or above zero mark deep convection reaching the tropopause");
        Canvas cv(n);
        paint(cv, 4 | 8, [&](size_t i) -> C3 { const float d = bWv[i] - bIr[i]; return isnanf_(d) ? C3{0, 0, 0} : pal.at(std::clamp(d, -40.0f, 8.0f)); });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "WV069 minus IR105, both calibrated", when);
        drawBar(cv, pal, -40, 8, {-40, -30, -20, -10, -2, 0, 4, 8}, "K", false);
        pr->image = std::move(cv.img);
    }

    // ---- 5. fog / low cloud at night, 3.8 - 10.5 -------------------------------------------------------------------------------
    if (calSw.ok) {
        const Palette pal({{-8, 255, 255, 255}, {-4, 150, 235, 245}, {-2, 60, 160, 210}, {0, 70, 75, 95}, {2, 40, 45, 55}, {5, 20, 22, 28}});
        Product* pr = newProduct("GK2A_L2_FogLowCloud_BTD_SW038-IR105.png", "Night fog / low cloud: BT3.8 minus BT10.5",
                                 "night only: water droplet cloud is colder at 3.8 um than at 10.5 um");
        Canvas cv(n);
        paint(cv, 2 | 8, [&](size_t i) -> C3 {
            const float d = bSw[i] - bIr[i];
            if (isnanf_(d)) return C3{0, 0, 0};
            const float night = 1.0f - smooth(-0.12f, 0.02f, cosz[i]);     // 1 in the dark, 0 where the sun contaminates 3.8 um
            const auto c = pal.at(std::clamp(d, -8.0f, 5.0f));
            const float g = (330.0f - bIr[i]) / 150.0f;
            const C3 base = grey(0.25f + 0.35f * std::clamp(g, 0.0f, 1.0f));
            C3 o;
            for (int k = 0; k < 3; ++k) o[size_t(k)] = std::uint8_t(std::clamp(base[size_t(k)] * (1.0f - night) + c[size_t(k)] * night, 0.0f, 255.0f));
            return o;
        });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "SW038 minus IR105, both calibrated; the day side is left grey (solar 3.8 um)", when);
        drawBar(cv, pal, -8, 5, {-8, -6, -4, -2, 0, 2, 5}, "K", false);
        pr->image = std::move(cv.img);
    } else res.skipped.push_back("FogLowCloud: " + calSw.why);

    // ---- 6. visible reflectance -----------------------------------------------------------------------------------------------
    if (calVi.ok) {
        Product* pr = newProduct("GK2A_L2_VisibleReflectance_VI006.png", "Visible reflectance 0.64 um (sun-angle corrected)",
                                 "VI006 albedo divided by cos(solar zenith); dark where the sun is low or down");
        Canvas cv(n);
        paint(cv, 1, [&](size_t i) -> C3 {
            const float a = alb[i]; if (isnanf_(a)) return C3{0, 0, 0};
            const float c = cosz[i];
            if (c < 0.05f) return C3{10, 10, 16};
            const float r = std::clamp(a / std::max(c, 0.12f), 0.0f, 1.2f);
            return grey(std::pow(std::clamp(r, 0.0f, 1.0f), 0.62f) * smooth(0.03f, 0.2f, c) + 0.04f);
        });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "VI006 albedo / cos(solar zenith), 4 x 4 pixel mean; shown only where the sun is up", when);
        const Palette pal({{0, 10, 10, 10}, {1, 255, 255, 255}});
        drawBar(cv, pal, 0, 1, {0, 0.2f, 0.4f, 0.6f, 0.8f, 1}, "reflectance", false);
        pr->image = std::move(cv.img);
    } else res.skipped.push_back("VisibleReflectance: " + calVi.why);

    // ---- 7. cloud mask (threshold) -------------------------------------------------------------------------------------------
    {
        Product* pr = newProduct("GK2A_L2_CloudMask_Threshold.png", "Cloud mask (threshold test)",
                                 "threshold quick-look mask, not the KMA operational cloud mask");
        Canvas cv(n);
        const bool haveVi = calVi.ok, haveSw = calSw.ok, haveSp = calSp.ok;
        const unsigned need = 8;
        paint(cv, need, [&](size_t i) -> C3 {
            const float t = bIr[i]; if (isnanf_(t)) return C3{0, 0, 0};
            const float c = cosz[i];
            const bool land = in.land ? in.land(double(lat[i]), double(lon[i])) != 0 : false;
            const bool sunUp = c > 0.2f, night = c < 0.0f;
            // each test is one reason to call the pixel cloudy
            const bool coldLand = land && std::fabs(lat[i]) > 40.0f;           // frozen ground is cold without being cloud
            const bool veryCold = t < 225.0f, cold = t < (coldLand ? 248.0f : 262.0f);
            const bool cirrus = haveSp && !isnanf_(bSp[i]) && (t - bSp[i]) > 2.0f && t < 288.0f;
            const bool lowNight = night && haveSw && !isnanf_(bSw[i]) && (bSw[i] - t) < -2.5f && t > 255.0f;
            const bool bright = sunUp && haveVi && !isnanf_(alb[i]) && (alb[i] / std::max(c, 0.2f)) > (land ? 0.34f : 0.22f);
            const bool warmTop = t < 272.0f;
            if (veryCold) return C3{250, 250, 255};
            if (cold) return C3{185, 205, 235};
            if (cirrus) return C3{120, 190, 215};
            if (lowNight || (bright && (warmTop || t < 295.0f))) return C3{190, 160, 120};
            return land ? C3{44, 74, 46} : C3{14, 34, 84};
        });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "IR 10.5 um temperature, split-window cirrus test, 3.8 um night low-cloud test, visible brightness test", when);
        drawClassLegend(cv, {{14, 34, 84, "CLEAR SEA"}, {44, 74, 46, "CLEAR LAND"}, {190, 160, 120, "LOW CLOUD"},
                             {185, 205, 235, "MID/HIGH CLOUD T<262K"}, {120, 190, 215, "THIN CIRRUS"}, {250, 250, 255, "DEEP COLD TOP T<225K"}});
        pr->image = std::move(cv.img);
    }

    // ---- 8. night microphysics RGB ---------------------------------------------------------------------------------------------
    if (calSp.ok && calSw.ok) {
        Product* pr = newProduct("GK2A_RGB_NightMicrophysics.png", "RGB night microphysics (EUMETSAT recipe)",
                                 "R: BT12.3-BT10.5 (-4..+2 K), G: BT10.5-BT3.8 (0..10 K), B: BT10.5 (243..293 K)");
        Canvas cv(n);
        paint(cv, 2 | 8 | 16, [&](size_t i) -> C3 {
            const float t = bIr[i], s = bSp[i], w = bSw[i];
            if (isnanf_(t) || isnanf_(s) || isnanf_(w)) return C3{0, 0, 0};
            const float night = 1.0f - smooth(-0.12f, 0.02f, cosz[i]);
            const float r = std::clamp(((s - t) + 4.0f) / 6.0f, 0.0f, 1.0f);
            const float g = std::clamp(((t - w) - 0.0f) / 10.0f, 0.0f, 1.0f);
            const float b = std::clamp((t - 243.0f) / 50.0f, 0.0f, 1.0f);
            const float gr = std::clamp((330.0f - t) / 150.0f, 0.0f, 1.0f);
            const C3 base = grey(0.15f + 0.6f * gr);
            C3 o;
            const float rgb[3] = {r, g, b};
            for (int k = 0; k < 3; ++k) o[size_t(k)] = std::uint8_t(std::clamp(base[size_t(k)] * (1.0f - night) + 255.0f * rgb[k] * night, 0.0f, 255.0f));
            return o;
        });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "R BT12.3-BT10.5  G BT10.5-BT3.8  B BT10.5; the day side is shown as grey IR", when);
        drawClassLegend(cv, {{255, 60, 60, "R: BT12.3 - BT10.5  -4..+2 K"}, {60, 255, 60, "G: BT10.5 - BT3.8  0..+10 K"}, {60, 60, 255, "B: BT10.5  243..293 K"}});
        pr->image = std::move(cv.img);
    } else res.skipped.push_back("RGB_NightMicrophysics: " + (calSp.ok ? calSw.why : calSp.why));

    // ---- 9. day microphysics RGB -------------------------------------------------------------------------------------------------
    if (calVi.ok && calSw.ok) {
        Product* pr = newProduct("GK2A_RGB_DayMicrophysics.png", "RGB day microphysics (EUMETSAT recipe)",
                                 "R: reflectance, G: solar 3.8 um reflectance (0..60 %, gamma 2.5), B: BT10.5 (203..323 K)");
        Canvas cv(n);
        const double lamSw = 3.83, esun = 4.5 * sun.distFactor;   // approximate band solar irradiance, W m-2 um-1
        paint(cv, 1 | 2 | 8, [&](size_t i) -> C3 {
            const float t = bIr[i], w = bSw[i], a = alb[i];
            if (isnanf_(t) || isnanf_(w) || isnanf_(a)) return C3{0, 0, 0};
            const float c = cosz[i];
            const float day = smooth(0.05f, 0.25f, c);
            const float refl = std::clamp(a / std::max(c, 0.12f), 0.0f, 1.0f);
            // solar part of the 3.8 um signal: observed radiance minus what the surface or cloud emits (taken from BT10.5)
            const double lobs = planck(lamSw, double(w)), lemit = planck(lamSw, double(t));
            const double sol = esun * std::max(double(c), 0.12) / kPi;
            const float r39 = std::clamp(float((lobs - lemit) / (sol - lemit)), 0.0f, 1.0f);
            const float r = refl, g = std::pow(std::clamp(r39 / 0.6f, 0.0f, 1.0f), 1.0f / 2.5f), b = std::clamp((t - 203.0f) / 120.0f, 0.0f, 1.0f);
            const float gr = std::clamp((330.0f - t) / 150.0f, 0.0f, 1.0f);
            const C3 base = grey(0.12f + 0.6f * gr);
            C3 o;
            const float rgb[3] = {r, g, b};
            for (int k = 0; k < 3; ++k) o[size_t(k)] = std::uint8_t(std::clamp(base[size_t(k)] * (1.0f - day) + 255.0f * rgb[k] * day, 0.0f, 255.0f));
            return o;
        });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "R VI006 reflectance  G solar 3.8 um reflectance  B BT10.5; the night side is shown as grey IR", when);
        drawClassLegend(cv, {{255, 60, 60, "R: VI006 reflectance  0..100 %"}, {60, 255, 60, "G: solar 3.8 um reflectance  0..60 %"}, {60, 60, 255, "B: BT10.5  203..323 K"}});
        pr->image = std::move(cv.img);
    } else res.skipped.push_back("RGB_DayMicrophysics: " + (calVi.ok ? calSw.why : calVi.why));

    // ---- 10. false colour -------------------------------------------------------------------------------------------------------------
    if (calVi.ok) {
        Product* pr = newProduct("GK2A_RGB_FalseColor.png", "False colour (calibrated visible + IR 10.5 um)",
                                 "surface colours from the land mask, cloud from reflectance and temperature");
        Canvas cv(n);
        paint(cv, 1 | 8, [&](size_t i) -> C3 {
            const float t = bIr[i], a = alb[i];
            if (isnanf_(t) || isnanf_(a)) return C3{0, 0, 0};
            const float c = cosz[i];
            const float sun = smooth(0.0f, 0.22f, c);
            const float refl = std::clamp(a / std::max(c, 0.12f), 0.0f, 1.0f);
            const bool land = in.land ? in.land(double(lat[i]), double(lon[i])) != 0 : false;
            const float cold = std::clamp((290.0f - t) / 70.0f, 0.0f, 1.0f);
            const float cloudV = smooth(0.30f, 0.80f, refl);
            const float cloudI = smooth(0.15f, 0.9f, cold);
            const float cloud = std::clamp(std::max(cloudV * sun, cloudI * (1.0f - 0.55f * sun)), 0.0f, 0.97f);
            const float lit = 0.18f + 0.82f * sun;
            const float sr = land ? 0.19f + 0.36f * refl : 0.02f + 0.12f * refl;
            const float sg = land ? 0.26f + 0.40f * refl : 0.13f + 0.28f * refl;
            const float sb = land ? 0.13f + 0.22f * refl : 0.30f + 0.36f * refl;
            const float w = 0.78f + 0.2f * std::clamp(refl, 0.0f, 1.0f);
            C3 o;
            const float sc[3] = {sr, sg, sb};
            for (int k = 0; k < 3; ++k) o[size_t(k)] = b8((sc[k] * (1.0f - cloud) + w * cloud) * lit);
            return o;
        });
        drawOverlay(cv, in);
        drawTitle(cv, pr->title, "VI006 reflectance + IR105, land and sea from the bundled land mask; the night side is dark", when);
        drawClassLegend(cv, {{14, 40, 110, "SEA"}, {70, 100, 55, "LAND"}, {235, 235, 235, "CLOUD (BRIGHT OR COLD)"}});
        pr->image = std::move(cv.img);
    } else res.skipped.push_back("RGB_FalseColor: " + calVi.why);

    return res;
}

}  // namespace nadirstation::gk2a::l2
