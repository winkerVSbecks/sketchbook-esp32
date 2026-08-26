---
name: port-canvas-sketch-to-esp32
description: Port JavaScript/TypeScript canvas generative art sketches (ssam, canvas-sketch, p5, raw 2D context) to C++ running on the repo's Waveshare ESP32-S3 display boards (LCD-1.47B, Touch-AMOLED-1.8, Touch-LCD-2), with a native SDL build for fast iteration. Use this whenever the user wants to run a sketch on hardware, put a generative piece on a small display, port canvas code to a microcontroller, or mentions one of those boards, LovyanGFX, ST7789, SH8601, CST816, or "get this sketch on the board" — even if they only describe the sketch and the display without using the word "port".
---

# Porting canvas sketches to the ESP32-S3-LCD-1.47B

The goal is a sketch that runs standalone on a 172×320 display, keeps the
composition the original produced, and stays editable on a desktop.

The core insight that makes this tractable: **almost all the work is deciding
what the sketch actually draws, not translating syntax.** Canvas path code often
resolves to simple analytic primitives once you trace it, and a sketch built
from those ports cleanly. A sketch that genuinely needs arbitrary path filling
does not. Establish which one you have before writing any C++.

## Workflow

1. **Read the sketch and classify it** (see "Feasibility triage" below). Say
   plainly whether it ports well, ports with compromises, or doesn't port.
2. **Reduce the drawing code to primitives.** Trace every `beginPath` /
   `arcTo` / `bezierCurveTo` construction by hand and work out the shape it
   actually produces. Report what you found — the user often doesn't know.
3. **Decide static vs animated.** This sets the entire performance budget and
   changes what rendering approach is worth using.
4. **Set up the dual-target project** — native SDL plus ESP32, one source file.
   See `references/board-and-toolchain.md`.
5. **Port the generator first, rendering second.** Layout, seeding, and colour
   selection are pure math and port almost verbatim. Get those printing correct
   values to the console before drawing anything.
6. **Build and run on SDL.** Iterate there.
7. **Flash to hardware and check the panel config.** Only physical properties
   remain — see the four-item checklist in the toolchain reference.

Don't skip to hardware if the user doesn't have the board yet, and don't skip
SDL if they do — the loop is seconds instead of a minute, and the SDL build
catches every logic bug.

## Feasibility triage

Ask what the sketch draws, then judge:

**Ports cleanly** — rectangles, circles, ellipses, quarter/half discs, straight
lines, axis-aligned clipping, flat fills, translation. Blind systems, grid
systems, tile systems, Wang tiles, colour-field work, anything Ellsworth
Kelly-adjacent.

**Ports with compromises** — rotation (precompute corners), gradients (band in
RGB565 unless dithered), text (LovyanGFX has fonts, but not your web font),
many small alpha-blended layers (no readback-free path, gets slow).

**Doesn't port without a rewrite** — arbitrary bezier path filling, Voronoi or
Delaunay cells, particle systems above a few thousand elements, per-pixel
shader-style effects at 60fps, anything leaning on `globalCompositeOperation`
or `filter`. There is no path rasteriser anywhere in this stack.

Say so early and directly rather than discovering it halfway through. If it
falls in the third bucket, offer the honest alternatives: pick a different
sketch, rewrite the piece around the primitives that do exist, or keep the
board as a display and stream frames over WiFi from a host running the JS.

## Static vs animated changes everything

**Static** (`animate: false`, or regenerates on a timer): performance is a
non-issue. A full-screen redraw every few seconds means you can afford
per-pixel analytic anti-aliasing, float math everywhere, and no optimisation
at all. Spend the budget on quality. This is the better first port.

**Animated**: the frame budget is real. Pushing all 110,080 bytes of the
framebuffer at 40MHz SPI costs ~22ms before you've drawn anything, capping you
near 45fps. Push dirty rects instead of full frames — most generative
animation only changes small regions — and profile before optimising.

## Anti-aliasing is usually the quality bottleneck

At 172×320, aliasing is the single most visible difference from the browser.
LovyanGFX's `fillArc` and friends alias badly. For any curved edge, compute
coverage analytically instead — see the worked recipe in
`references/porting-patterns.md`. It's about fifteen lines, it's exact, and
on a static sketch it costs nothing.

The enabling trick: if shapes don't overlap each other (each confined to its
own cell or region), every antialiased pixel blends against the background and
nothing else, so no framebuffer readback is needed.

## Colour needs two different metrics

Sketches often use one contrast function for everything. That's usually a bug
worth surfacing:

- **Colour vs background** — WCAG contrast ratio is correct here. It's a
  luminance ratio designed for exactly this.
- **Colour vs colour** — WCAG is the wrong tool. It cannot see hue, so a
  saturated blue and a saturated rust score ~1.0 and one gets discarded. Use
  perceptual distance in oklab instead.

When a ported palette collapses to one or two colours, this is almost always
why. Diagnose it by naming the specific palette entries that knocked each
other out, not by guessing.

## Report what the port revealed

Porting forces a precise reading of the original, which surfaces things the
user couldn't see in JS. Tell them:

- Shapes that turned out simpler than the code suggested
- Dead code — zero-area subpaths, unreachable branches
- Bugs, especially closure-over-wrong-variable in filter chains
- Metrics being used for the wrong job

Port the _behaviour_, not the intent, when they diverge — the existing output
is what the user has been selecting on. Flag the divergence, implement what the
code does, and let them decide. Say which fixes also apply to the JS side.

## References

- `references/board-and-toolchain.md` — verified pin config for the
  **LCD-1.47B**, `platformio.ini`, platform shims, SDL entry point, flashing,
  and the panel-config checklist. Read this before writing any project files.
- `references/board-touch-lcd-2.md` — verified pin config and bringup notes
  for the **Touch-LCD-2** (2", 240×320, ST7789T3 + CST816D touch), from its
  demo zip and schematic. The board is researched but not yet brought up in
  the repo; start there when it is.
- The **Touch-AMOLED-1.8** is already brought up; its facts live in the repo's
  `CLAUDE.md` and `src/shared/boards/amoled_18.h`.
- `references/porting-patterns.md` — JS→C++ translation rules, seeded PRNG,
  colour conversion, the analytic AA recipe, performance notes. Read this
  before writing any drawing code.
