.. _solvfx-viewer:

Viewer and timeline
===================

Viewer
------

- The viewer process uses the OCIO display / view of the ``OCIO`` config (for example
  *sRGB (ACES)*). The view list updates when the config file changes.
- Second toolbar row: exposure (f/8, ``,`` and ``.`` = minus / plus one stop), gamma and
  saturation. They only affect the display, never the render.
- Middle click (without dragging) frames the image, middle-drag pans, the wheel zooms.
- Q toggles the overlays. Roto, RotoPaint and Tracker overlays work as soon as their properties
  are open, even with nothing connected (draw shapes, Ctrl+Alt+click to add tracks).

Timeline
--------

- I / O set the in and out points (orange); Alt+I / Alt+O clear them.
- The range menu sets what the timeline shows: Global, Input, In/Out, or Visible (middle-drag a
  segment of the timeline). Right-drag pans the timeline.
- Play works with nothing connected (loop, bounce, once).
- The cache bar shows the cached frames; keyframes are red ticks.
