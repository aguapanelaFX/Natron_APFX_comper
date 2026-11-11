.. _solvfx-apfx-nodes:

APFX nodes
==========

The APFX nodes are written for SOLVFX by AGUAPANELA FX. Each node page in the reference guide has
the full parameter list; this page is an overview.

Color
-----

**Look**
  One-node look: temperature / tint, exposure, contrast and pivot, highlights /
  shadows / whites / blacks, saturation, vibrance, faded film, shadow / highlight tint, colour
  wheels. *save look* / *load look* keep looks in ``.look`` files; *export .cube* writes the
  colour part as a 3D LUT (log2 shaper, -8 to +8 stops around 0.18).

**GradeEdge**
  Grades the inner edge of the alpha (expand, blur, hue correct, gain / multiply / gamma, optional
  background and mask).

**Toe**
  Lifts the blacks with a smooth curve that only affects values below *range*; values above it
  are untouched.

Filter
------

**Defocus**
  Fast convex bokeh (disc or blades, roundness, aspect) with highlight boost.

**ZDefocus**
  Depth based defocus. Connect the depth pass to *Depth*, choose its channel and the maths
  (*depth*, *far = 0*, *direct*), set the focal plane and depth of field. Output *focal plane*
  shows near (red), in focus (green) and far (blue). The image is blurred in layers (more layers =
  smoother transitions) and composited back to front.

**Convolve**
  Convolves the image with any image connected to *Filter* (FFT): image bokeh, streaks, custom
  glows. *normalize* keeps the brightness; *filter channel* uses rgb, luminance or alpha.

**ExpoGlow**, **Halation**, **ChromaticAberration**, **Vignette**
  Exponential glow, film halation, lens chromatic aberration and physical / artistic vignetting,
  each with presets (approximate starting points).

**EdgeExtend**
  Pushes edge colours outward from the alpha edge (clean colours behind hair and motion blur).

Draw
----

**Grain**
  *add* mode: synthetic grain with film and camera presets (approximate, not measured), per
  channel size, irregularity and intensity, correlation, response (black, minimum, shadows /
  midtones / highlights). *save preset* stores your settings for every comp.

  *match* mode (regrain like the plate): connect *Plate* (original grainy plate) and, if you have
  it, *Degrain* (denoised plate; otherwise a soft internal degrain is used). Press *analyse*: the
  grain response per channel is measured over a few frames and stored in the comp. The plate grain
  is scattered with Voronoi cells (*cell size*, *edge blend*) and added only where the comp differs
  from the plate (*replace threshold* / *softness*). *output* shows the grain or the replace mask
  for checking. Tip: prerender the denoised plate.

**Noise**
  Procedural noise: several noise types, fractal layers (octaves), Z to move through the noise and
  seamless loops.

Keyer
-----

**Keyer**, **DifScreenColour**, **DifScreenKey**: see :ref:`solvfx-keying`.

Channel, Time, Transform and others
-----------------------------------

**Copy**
  Output is B with up to 4 channels copied from A (shortcut K).

**Remove**
  *keep*: only the chosen rgba channels go through and every other layer is removed. *remove*: the
  chosen channels are set to 0 and every other layer goes through.

**Cryptomatte**
  Pick objects in the viewer: Ctrl+click adds the object under the cursor, Ctrl+Shift+click
  removes it. The matte goes to alpha; names come from the EXR manifest.

**Retime**
  Maps an input frame range to an output range, or retimes by a speed; reverse, before / after
  (continue, loop, bounce, hold, black) and frame filtering (none, nearest, box).

**STMapGradient**
  Identity UV map for STMap.
