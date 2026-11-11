.. _solvfx-interface:

Interface
=========

Layout
------

The default layout has the Viewer on the left and the Node Graph over Progress, Curve Editor and
Dope Sheet on the right. A layout file set in the preferences wins over the built-in one.

Node Graph
----------

- Nodes are flat rectangles coloured by category (Color, Filter, Merge, Transform, Draw, Keyer,
  Channel, Time, 3D, Other); the Viewer node is a pill. Selected nodes get a yellow outline,
  disabled nodes a dark cross, nodes in error a red outline and an ``ERROR`` label.
- **Drag a node onto a pipe** to insert it. A Dot, or a Merge with only A connected, joins the
  stream (the pipe goes into B / the main input). Hold Ctrl to skip inserting.
- **Stamps** (F10): select a node to create an Anchor under it; select an Anchor or a Stamp to
  create a Stamp linked to it. Right-click a Stamp to go to its anchor or reconnect it.
- Node thumbnails are grabbed once; right-click > *Refresh thumbnails* updates them.
- The grid (dotted, 110 x 44) is also the snap grid.

Properties
----------

- Properties open in floating windows when the Properties dock is closed (Esc or X closes them).
- Rows follow Nuke: label column, value field, slider, then colour swatch, colour wheel,
  ``4`` (split the channels) and the curve (animation) button. Panels stretch with their width.
- Labels are lowercase; checkbox text comes after the box.
- The bottom block of most nodes is ``mask`` / ``(un)premult by`` / ``mix``.

Parameter fields
----------------

- Type ``=`` in a field to open the expression editor (Python expressions).
- Alt+click a field to set a key at the current frame.
- Field colours: animated (brown), on a key (orange), expression (teal), linked (purple).
- The curve button (and right-click) opens the animation menu: set / delete key, no animation,
  set to default, curve editor, dope sheet, expressions, interpolation, link, copy / paste, and
  the **File** (import / export values as text), **Edit** (generate keys, move, reverse, negate)
  and **Predefined** (loop, ping-pong, reverse, hold, random) submenus.

Menus and dialogs
-----------------

Menus have rounded corners and a muted orange highlight; shortcuts are shown on the right.
Dialogs use an orange default button.
