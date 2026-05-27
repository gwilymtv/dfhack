cavern-colors
=============

.. dfhack-tool::
    :summary: Restore per-mineral floor colors in premium graphics mode.
    :tags: fort graphics

In ASCII Dwarf Fortress, mined cavern floors inherit the color of the mineral
they're embedded in: native gold floors appear yellow, gem clusters appear in
their gem color, and so on. In the Steam/premium renderer, all cavern floors
use a single grey stone sprite regardless of the underlying mineral.

``cavern-colors`` patches the floor sprite layer each frame, replacing the
grey sprite with a tinted variant that reflects the mineral's color. Buildings
and other overlaid graphics are unaffected. The plugin works automatically for
all modded minerals without any additional configuration.

Usage
-----

::

    cavern-colors enable|disable
    cavern-colors mode <hybrid|mat_rgb|basic_color>
    cavern-colors

Running the command with no arguments prints the current mode and enabled
state.

Color modes
-----------

``hybrid`` (default)
    Uses the material's ``mat_rgb`` floating-point color when available,
    falling back to the DF 16-color palette (``basic_color``) when unset.
    Provides the most accurate colors across both vanilla and modded minerals.

``mat_rgb``
    Always uses the material's ``mat_rgb`` color. If a material has no
    ``mat_rgb`` value, the floor tile is left uncolored.

``basic_color``
    Always uses the DF 16-color palette (the ``DISPLAY_COLOR`` token in
    raws). Faithful to ASCII-mode colors; fewer shades but universally
    present for all minerals.

Notes
-----

- The colored floor sprite is a procedurally generated tinted tile, not DF's
  original floor art. The base tile has a simple stone texture that is
  multiplied by the mineral's color. Floor sprite variation (the small
  variation DF applies to stone floors) is not preserved.
- The texture table is built once when a world is loaded, so there is no
  per-frame CPU overhead beyond the viewport scan and texpos writes.
- The mode can be changed at runtime; the texture table is rebuilt
  immediately.
