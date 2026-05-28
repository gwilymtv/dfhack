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
    cavern-colors boost <float>
    cavern-colors strength <0..1>
    cavern-colors rough-edges on|off
    cavern-colors z-fog on|off
    cavern-colors sample-cell [<wx> <wy> [<wz>]]
    cavern-colors dump-texture <texpos>
    cavern-colors

Running the command with no arguments prints the current mode, tuning
values, and cache stats.

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

Tuning
------

``boost <float>`` (default ``2.0``)
    Brightness multiplier applied to each source pixel before the material
    tint is multiplied in. Compensates for DF's carved-stone sprites being
    darker than the surface they represent.

``strength <0..1>`` (default ``0.8``)
    Tint saturation. ``1.0`` is the full per-material color; ``0.0`` lerps
    the tint all the way to white (no color shift). Lower values keep more
    of the original sprite's neutral shading.

Rough-edge bleed
----------------

When a rough cavern floor sits next to a smoothed or constructed floor of a
different mineral, DF draws fringe sprites that bleed the rough tile's
material a few pixels into its neighbour. ``cavern-colors`` tints those
fringes with the rough neighbour's color and composites them under any
corner fringes (which appear automatically where two cardinals both have
rough neighbours). Toggle with ``cavern-colors rough-edges on|off``;
default is on.

Z-fog tinting
-------------

When the tile at the current view z-level is open/empty, DF renders
depth-fogged sprites from floors on lower z-levels into the same screen
cells. ``cavern-colors`` tints those fogged sprites with the material of
the floor each one depicts, using the same per-material tint (and
rough-edge bleed, where applicable) as cells at the current z. DF's
depth-fog dimming is applied by DF's renderer after the tint, so the fog
effect is preserved. Toggle with ``cavern-colors z-fog on|off``; default
is on.

Debug commands
--------------

``sample-cell [<wx> <wy> [<wz>]]``
    Dump the tile, material, texpos, and ``floor_flag`` bytes for a single
    cell. Defaults to the cell under the mouse cursor.

``dump-texture <texpos>``
    Dump the unique-pixel histogram of a texture in DF's atlas. Useful when
    investigating how a sprite is colored or composited.

Notes
-----

- The colored floor sprite is a procedurally generated tinted tile, not DF's
  original floor art. The base tile has a simple stone texture that is
  multiplied by the mineral's color. Floor sprite variation (the small
  variation DF applies to stone floors) is not preserved.
- The texture table is built once when a world is loaded, so there is no
  per-frame CPU overhead beyond the viewport scan and texpos writes.
- The mode, boost, and strength can all be changed at runtime; the texture
  cache is rebuilt immediately.
