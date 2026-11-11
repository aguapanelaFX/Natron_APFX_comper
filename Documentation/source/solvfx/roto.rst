.. _solvfx-roto:

Roto and planar tracking
========================

Drawing and editing
-------------------

- Points, tangents and curves are easier to pick (larger click areas).
- **Feather**: the feather point of each selected point is shown with its own handle. Click /
  Shift-click selects feather points, dragging moves them freely; their tangent handles can be
  dragged too. Ctrl+drag a point pulls its feather out.
- **E** pushes the feather of all selected points (or the selected shapes) out one step; hold E and
  drag to set it. **Shift+E** resets the feather of the selected points only (or the whole shape when
  only the shape is selected).
- **Z** smooths the selected points fully in one press; Shift+Z makes them cusps.
- Closing a shape shows both handles of the closing point and both tangents follow the drag.

SolvRoto planar tracking
------------------------

Planar tracking lives inside the Roto / RotoPaint node (*Track* page and viewer buttons):

1. Select the shapes that cover a flat surface (shapes in a layer whose name contains
   ``exclude`` are masked out, for occluders).
2. Track forward / backward (or one frame). The tracked shapes follow the surface.
3. The blue **surface** (4 corners + grid) shows where a corner pin will land; drag its corners to
   the real surface (*align to shapes* resets it).
4. **Adjust track**: turn on *adjust* and drag a surface corner at frames where the track drifts.
   Each drag keys a correction; corrections are interpolated between corrected frames and the
   tracked shapes are re-baked. *clear corrections* removes them.
5. **Export**: CornerPin2D or Transform (linked or baked, at the reference or the current frame),
   a Tracker, or a Roto with the track baked in.
