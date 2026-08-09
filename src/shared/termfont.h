// ============================================================================
// termfont.h — a 4x6 terminal font with box-drawing and block-element glyphs
// ============================================================================
// LovyanGFX ships fonts, but none of them carry U+2500 box drawing or U+2580
// block elements, and none are legible at the cell size a 172x320 panel needs
// if you want a terminal with enough columns to hold a chart. So this is a
// hand-built bitmap font covering exactly the glyphs terminal_charts.cpp
// draws — nothing more.
//
// One property matters more than looks: **the box-drawing glyphs must tile.**
// A chart line is a run of '-' cells with corner cells at the steps, so the
// horizontal stroke sits on the same row in every glyph that has one and runs
// to the cell edge on whichever side it connects; the vertical stroke sits on
// the same column and runs to the top or bottom edge. Get that wrong by one
// pixel and the woven look collapses into dashes at every cell seam.
//
//   cell        4 x 6 px         172/4 = 43 columns, 320/6 = 53 rows
//   rule row    2                full width for '-', to the edge for corners
//   rule column 1                full height for '|', to the edge for corners
//   text body   cols 0-2, rows 0-4   (col 3 and row 5 are the advance gaps)
//
// At 4px there is no room for a corner radius, so the rounded set (U+256D-70,
// used by the chart lines) and the square set (U+250C-18, used by the panel
// borders) share bitmaps. They stay separate glyph indices because the sketch
// reads better that way, and a larger cell could differentiate them.
//
// The four block elements are density patterns, not letters. Each is a 2px
// lattice, and since the cell is even in both axes the lattice runs unbroken
// across cell seams — a wall of block glyphs reads as one flat tone rather
// than as a grid of tiles.
//
//   U+2591 light    6/24 px   25%
//   U+2592 medium  12/24 px   50%   checkerboard
//   U+2593 dark    18/24 px   75%   inverse of light
//   U+2588 full    24/24 px  100%
//
// Storage is one byte per pixel row, high bit first, so a glyph is six bytes
// and the whole font is under 200. Rows are written as binary literals for
// legibility — read the 1s as a picture of the glyph.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

static const int TERM_CELL_W = 4;
static const int TERM_CELL_H = 6;

// Glyph indices. 0 is the empty cell and is never blitted, so a grid of
// zeroed bytes is an empty screen.
enum TermGlyph : uint8_t {
  G_NONE = 0,
  // box drawing — these are the ones that have to tile
  G_HLINE,      // U+2500  horizontal rule
  G_VLINE,      // U+2502  vertical rule
  G_ARC_TL,     // U+256D  rounded, connects right + down
  G_ARC_TR,     // U+256E  rounded, connects left  + down
  G_ARC_BL,     // U+2570  rounded, connects right + up
  G_ARC_BR,     // U+256F  rounded, connects left  + up
  G_BOX_TL,     // U+250C  square,  connects right + down
  G_BOX_TR,     // U+2510  square,  connects left  + down
  G_BOX_BL,     // U+2514  square,  connects right + up
  G_BOX_BR,     // U+2518  square,  connects left  + up
  // block elements — density, not letters
  G_LIGHT,      // U+2591
  G_MEDIUM,     // U+2592
  G_DARK,       // U+2593
  G_FULL,       // U+2588
  // the ASCII chart set
  G_EQUALS,     // '='
  G_PLUS,       // '+'
  G_PIPE,       // '|'
  // the randomart ramp " .o+=*BOX@%&#/^", minus the '+' and '=' above
  G_DOT,        // '.'
  G_LOWO,       // 'o'
  G_STAR,       // '*'
  G_UPB,        // 'B'
  G_UPO,        // 'O'
  G_UPX,        // 'X'
  G_AT,         // '@'
  G_PCT,        // '%'
  G_AMP,        // '&'
  G_HASH,       // '#'
  G_SLASH,      // '/'
  G_CARET,      // '^'
  // randomart start and end markers
  G_UPS,        // 'S'
  G_UPE,        // 'E'
  TERM_GLYPH_COUNT
};

static const uint8_t TERM_GLYPH[TERM_GLYPH_COUNT][TERM_CELL_H] = {
  // G_NONE
  { 0b0000, 0b0000, 0b0000, 0b0000, 0b0000, 0b0000 },

  // ---- box drawing. Rule row 2, rule column 1. -----------------------------
  // G_HLINE  U+2500      G_VLINE  U+2502
  { 0b0000, 0b0000, 0b1111, 0b0000, 0b0000, 0b0000 },
  { 0b0100, 0b0100, 0b0100, 0b0100, 0b0100, 0b0100 },

  // G_ARC_TL / G_ARC_TR / G_ARC_BL / G_ARC_BR
  { 0b0000, 0b0000, 0b0111, 0b0100, 0b0100, 0b0100 },
  { 0b0000, 0b0000, 0b1100, 0b0100, 0b0100, 0b0100 },
  { 0b0100, 0b0100, 0b0111, 0b0000, 0b0000, 0b0000 },
  { 0b0100, 0b0100, 0b1100, 0b0000, 0b0000, 0b0000 },

  // G_BOX_TL / G_BOX_TR / G_BOX_BL / G_BOX_BR — same bitmaps at this size
  { 0b0000, 0b0000, 0b0111, 0b0100, 0b0100, 0b0100 },
  { 0b0000, 0b0000, 0b1100, 0b0100, 0b0100, 0b0100 },
  { 0b0100, 0b0100, 0b0111, 0b0000, 0b0000, 0b0000 },
  { 0b0100, 0b0100, 0b1100, 0b0000, 0b0000, 0b0000 },

  // ---- block elements. 2px lattices, seamless across cells. ----------------
  // G_LIGHT 25%
  { 0b1010, 0b0000, 0b1010, 0b0000, 0b1010, 0b0000 },
  // G_MEDIUM 50%
  { 0b1010, 0b0101, 0b1010, 0b0101, 0b1010, 0b0101 },
  // G_DARK 75%
  { 0b0101, 0b1111, 0b0101, 0b1111, 0b0101, 0b1111 },
  // G_FULL 100%
  { 0b1111, 0b1111, 0b1111, 0b1111, 0b1111, 0b1111 },

  // ---- text, on a 3x5 body in cols 0-2 / rows 0-4 --------------------------
  // G_EQUALS  '='          G_PLUS  '+'            G_PIPE  '|'
  //   Deliberately not the box glyphs: an ASCII chart is meant to look like
  //   one, so '=' sits off the rule row and '|' stops short of the cell floor.
  { 0b0000, 0b1110, 0b0000, 0b1110, 0b0000, 0b0000 },
  { 0b0000, 0b0100, 0b1110, 0b0100, 0b0000, 0b0000 },
  { 0b0100, 0b0100, 0b0100, 0b0100, 0b0100, 0b0000 },

  // G_DOT '.'
  { 0b0000, 0b0000, 0b0000, 0b0000, 0b0100, 0b0000 },
  // G_LOWO 'o'
  { 0b0000, 0b0000, 0b1110, 0b1010, 0b1110, 0b0000 },
  // G_STAR '*'
  { 0b0000, 0b1010, 0b0100, 0b1010, 0b0000, 0b0000 },
  // G_UPB 'B'
  { 0b1100, 0b1010, 0b1100, 0b1010, 0b1100, 0b0000 },
  // G_UPO 'O'
  { 0b1110, 0b1010, 0b1010, 0b1010, 0b1110, 0b0000 },
  // G_UPX 'X'
  { 0b1010, 0b1010, 0b0100, 0b1010, 0b1010, 0b0000 },
  // G_AT '@'
  { 0b1110, 0b1010, 0b1110, 0b1000, 0b1110, 0b0000 },
  // G_PCT '%'
  { 0b1010, 0b0010, 0b0100, 0b1000, 0b1010, 0b0000 },
  // G_AMP '&'
  { 0b0100, 0b1010, 0b0100, 0b1010, 0b0110, 0b0000 },
  // G_HASH '#'
  { 0b1010, 0b1110, 0b1010, 0b1110, 0b1010, 0b0000 },
  // G_SLASH '/'
  { 0b0010, 0b0010, 0b0100, 0b1000, 0b1000, 0b0000 },
  // G_CARET '^'
  { 0b0100, 0b1010, 0b0000, 0b0000, 0b0000, 0b0000 },
  // G_UPS 'S'
  { 0b0110, 0b1000, 0b0100, 0b0010, 0b1100, 0b0000 },
  // G_UPE 'E'
  { 0b1110, 0b1000, 0b1110, 0b1000, 0b1110, 0b0000 },
};

// The OpenSSH randomart ramp " .o+=*BOX@%&#/^", as glyph indices. Entry 0 is
// the space and is unreachable — drawRandomartWeftSeries skips cells with zero
// visits, so the lowest index it ever emits is 1. Kept so the table indexes
// straight from a visit count, exactly like the JS string does.
static const uint8_t TERM_RAMP[15] = {
  G_NONE, G_DOT, G_LOWO, G_PLUS, G_EQUALS, G_STAR, G_UPB, G_UPO,
  G_UPX,  G_AT,  G_PCT,  G_AMP,  G_HASH,   G_SLASH, G_CARET,
};

// ---------------------------------------------------------------------------
// Blitting
// ---------------------------------------------------------------------------
// A 16bpp LGFX_Sprite stores byte-swapped RGB565 (`swap565_t`) with a stride
// equal to its width — createSprite computes `x_mask = 7 >> (bits >> 1)`,
// which is 0 for 16bpp, so there is no row padding to account for. That makes
// a direct framebuffer write safe, and it is worth taking: the grid is 2279
// cells and drawPixel() per lit pixel would be ~55k virtual calls a frame.
//
// Pass colours through termRaw() once per composition, not once per cell.
static inline uint16_t termRaw(uint16_t c565) {
  return (uint16_t)((c565 >> 8) | (c565 << 8));
}

// Draw one glyph at pixel (x, y). No clipping — the caller is expected to have
// sized the grid so every cell lies inside the framebuffer.
static inline void termBlit(uint16_t *fb, int stride, int x, int y,
                            uint8_t glyph, uint16_t raw) {
  const uint8_t *rows = TERM_GLYPH[glyph];
  uint16_t *p = fb + (size_t)y * (size_t)stride + (size_t)x;
  for (int r = 0; r < TERM_CELL_H; r++, p += stride) {
    const uint8_t m = rows[r];
    if (!m) continue;
    if (m & 0b1000) p[0] = raw;
    if (m & 0b0100) p[1] = raw;
    if (m & 0b0010) p[2] = raw;
    if (m & 0b0001) p[3] = raw;
  }
}
