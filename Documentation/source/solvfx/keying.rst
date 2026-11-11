.. _solvfx-keying:

Keying
======

APFX Keyer
----------

The *operation* turns each pixel into a value (luminance, red, green, blue, saturation, min, max, greenscreen,
bluescreen); the range graph with the A, B, C, D handles maps that value to alpha. Alpha is 0 below A, ramps from A to B,
is 1 from B to C and ramps down from C to D. Dragging a handle updates the fields and is one undo
step.

DifScreenColour and DifScreenKey
--------------------------------

A two-step screen keyer:

- **DifScreenColour** builds a clean screen plate from the green / blue screen: *size* removes the
  foreground, *darks* / *lights* set what counts as screen, *erode* and *patch black* clean the
  rest. Output the clean plate or the screen mask.
- **DifScreenKey** keys the plate against that clean screen (connect it to *Plate*, the new
  background to *Bg*): red weight, blue/green weight, luminance match, autolevels, screen
  subtraction (despill), use bkg luminance / chroma, black / white clip and premultiply.

Tip: start with *size* in DifScreenColour until no foreground is left in the clean plate, then
balance the weights in DifScreenKey on the edges.
