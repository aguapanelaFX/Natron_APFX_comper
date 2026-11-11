.. _solvfx-project:

Comps and project folders
=========================

New comps
---------

New comps are UHD_4K (3840 x 2160), frames 1001-1200 at 24 fps. Reading a file never changes the
comp format or range; right-click a selected Read in the Node Graph and choose
*Set Comp range from this Read* to use its range.

Set Project and path variables
------------------------------

**File > Set Project...** picks the project folder (``$PRJ``) with the SOLVFX file browser and
creates the default sub folders when they are missing:

=========== ================================ ==========================
Variable    Meaning                          Default
=========== ================================ ==========================
``$PRJ``    project folder                   set with Set Project
``$SCN``    folder of the current comp file  the comp's folder
``$OUT``    renders                          ``$PRJ/05_renders``
``$AST``    assets                           ``$PRJ/01_assets``
=========== ================================ ==========================

**File > Edit Project Variables...** maps any variable to another folder. The variables are stored
in the application settings (they belong to this computer).

- File paths can use the variables (``$PRJ/plates/shot010.####.exr``); the full resolved path is
  kept too, so a comp still opens on a machine without the variables.
- The file browser shows the variable folders as their own group at the top of the sidebar
  (``$PRJ/``, ``$OUT/`` ...). A new Read opens in ``$PRJ``, a new Write in ``$OUT``.
- **Save new version** (Alt+Shift+S) saves the comp with the next ``v###`` number.

Writes and renders
------------------

- W creates a Write without opening the file browser; the browser opens from the file field.
- The render popup opens on *Global*; it can render the input range, in / out, or custom frames.
- Overwriting existing files asks for confirmation.
