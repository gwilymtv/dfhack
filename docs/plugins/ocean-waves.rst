ocean-waves
===========

.. dfhack-tool::
    :summary: Restore ocean-wave/foam sprites in graphics mode.
    :tags: fort graphics

In ASCII Dwarf Fortress, ocean waves and sea foam render as ``flow_info``
objects (the same engine system as miasma, steam, fire). In the
Steam/premium renderer, the hardcoded flow → sprite table omits OceanWave
and SeaFoam, so they vanish even though the underlying simulation still
tracks them.

``ocean-waves`` patches the viewport's ``screentexpos_high_flow`` layer
each frame, writing wave/foam sprite texposes at the cells where
ocean-related flows live. Camera-above-sea-level (looking down from a
cliff) is handled via the lower-viewport / z-fog system.

.. warning::

    This plugin currently renders **procedural placeholder sprites**:
    blue-white chevrons pointing in the direction of travel for both
    open-ocean wave fronts and shore surf, plus omnidirectional foam
    bubble stipple. The placeholders exercise every dimension the
    engine exposes so we can validate the data wiring; real artwork
    is a future step.

Sprite combinations (canonical enumeration for real artwork commission):

* **Open-ocean wave** (``world.event.ocean_waves``): direction (8 — N,
  NE, E, SE, S, SW, W, NW from sign of ``dest - cur``) × intensity (2
  buckets from ``vis_duration``) × animation frame (2). Placeholder:
  32 sprites; commission target: 8 × 2 × 4 = 64. 8 directions is
  confirmed necessary — diagonal motion is common.
* **Surf splash** (``flow_info`` of type ``OceanWave``): direction (8,
  inferred per-flow from the nearest spawning ``ocean_wave`` —
  ``flow_info.dest`` is uninitialized garbage for ocean flows) ×
  ``expanding`` flag (2 — incoming vs withdrawing) × density (3
  buckets, alpha-only in the placeholder for legibility) × animation
  frame (2). Placeholder: 96; commission target: 192.
* **Sea foam** (``flow_info`` of type ``SeaFoam``): no direction —
  foam is deposited residue at the spot a wave broke and dissipates
  in place, not streaming. ``expanding`` (2) × density (3) × frame
  (2 — full vs fading). Placeholder: 12; commission target: 24.
* **Ripple** (``map_block.liquid_flow[bx][by].temp_flow_timer``):
  empty on every save tested; plumbing kept defensively. If a save
  does populate it: 4 timer buckets × 1-2 frames.

All sprites are 32×32 RGBA32. Animation tick source is
``enabler->gputicks`` (real-time; matches DF's own water).

Lower viewports (z-fog for camera-above-sea-level) reuse the same
sprite set — DF applies depth fog after our texpos substitution.

The commission counts above assume native artwork for each
direction × density × intensity. Two reduction techniques bring it
down significantly:

* **Rotate at load.** Artist supplies only N-facing sprites; the
  plugin transposes for 90° rotations and bilinear-rotates for
  diagonals. (Foam has no direction so this doesn't apply.)
* **Alpha-derive density and intensity.** These are pure
  visibility/strength dimensions — multiplying the alpha channel of
  a base sprite produces the variants. Direction, ``expanding``,
  and frame all need hand-drawn shapes.

Absolute-minimum commission with both techniques:

* Open-ocean wave: 1 dir × 1 intensity × 4 frame = **4 sprites**
* Surf splash: 1 dir × 2 expanding × 1 density × 4 frame = **8 sprites**
* Sea foam: 2 expanding × 1 density × 4 frame = **8 sprites**

Total: **~20 sprites** for the whole thing. The plugin generates
~228 internal variants from those.

Usage
-----

::

    ocean-waves enable|disable
    ocean-waves list-flows [--include-dead] [-n MAX]
    ocean-waves list-ocean-waves [-n MAX]
    ocean-waves list-makers
    ocean-waves edit-maker <index> [interval=N] [dir=N|NE|...|NW|auto]
    ocean-waves surf-mode maker|wave
    ocean-waves dump-flow [-n MAX]
    ocean-waves sample-flow [<wx> <wy> [<wz>]]
    ocean-waves tile-flow [<wx> <wy> [<wz>]]
    ocean-waves paint-test on|off
    ocean-waves clobber-check on|off
    ocean-waves force-spawn <wx> <wy> <wz> [wave|foam]
    ocean-waves clear-synthetic

Run with no arguments to see plugin status.

Debug commands
--------------

These exist to validate assumptions before the plugin is feature-complete.

``list-flows``
    Walks blocks overlapping the current viewport and prints any
    ``OceanWave`` / ``SeaFoam`` ``flow_info`` objects found. Answers
    "do these objects actually exist on this map, and where?" Pass
    ``--include-dead`` to also count flows with the ``DEAD`` flag set.

``list-makers``
    Lists ``world.event.ocean_wave_makers`` with their cached
    direction (computed at world load from the vector between
    ``wave_origin`` and ``coastline`` path centroids). This is the
    primary signal for surf direction — a surf cell takes its
    direction from the nearest maker by coastline centroid.

``edit-maker <index> key=value ...``
    PATCH-style editor for ``ocean_wave_makers[index]``. Only the
    specified fields change:

    * ``interval=N`` — directly writes the maker's ``interval`` field
      (game ticks between wave spawns; clamped to [1, 127]). Persists
      in DF's data and is the engine's actual spawn rate.
    * ``dir=N|NE|E|SE|S|SW|W|NW`` — plugin-side direction override.
      Does *not* modify the maker's ``coastline`` or ``wave_origin``
      paths (those drive DF's own wave geometry). Affects only the
      direction the plugin uses when ``surf-mode maker`` is active.
    * ``dir=auto`` or ``dir=clear`` — removes the direction override.

    The number of makers on an embark is determined at worldgen by
    coastline complexity; it is not a fixed limit. ``list-makers``
    shows the current count.

``surf-mode maker|wave``
    Selects the surf direction strategy:

    * **maker** (default) — closest ``ocean_wave_maker`` by coastline
      centroid. Stable, uniform per region; every surf cell in a
      maker's stretch of shore gets the same direction.
    * **wave** — nearest spawning ``ocean_wave`` by Euclidean distance.
      More variation, per-flow truth, but adjacent surf cells can
      flip direction.

``list-ocean-waves [-n MAX]``
    Lists ``world.event.ocean_waves`` — the open-ocean wave-front
    objects (each is one point; multiple spawned together by an
    ``ocean_wave_maker`` form a visible front line). Prints counts of
    total waves, waves in the current viewport at z, and waves with
    ``spawn_flows=1`` (about to hit the coast and spawn surf flow_info).

``dump-flow [-n MAX]``
    For up to ``MAX`` (default 5) ocean flows in the viewport, prints
    the flow_info and resolves its ``guide_id`` via
    ``world.flow_guides.all``. If the guide is a
    ``flow_guide_trailing_flowst``, prints its 15-coord ``line[]``
    array. The hypothesis being tested: one ``flow_info`` covers many
    cells via its trail, which would explain why ASCII shows broader
    wave coverage than the bare ``flow_info`` count suggests.

``sample-flow [<wx> <wy> [<wz>]]``
    Comprehensive per-tile dump. With no args, uses the mouse position.
    Reports:

    * Every ``flow_info`` whose ``pos`` matches this tile (full fields:
      type, density, expanding, mat_type, mat_index, dest, guide_id,
      flags).
    * ``tile_designation`` water-relevant bits (flow_size, liquid_type,
      liquid_static, water_stagnant, water_salt, outside, light,
      subterranean, hidden, rained).
    * ``liquid_flow`` bitfield (temp_flow_timer, temp_dir,
      perm_flow_dir, sink_dist).
    * Every ``ocean_wave`` whose ``cur`` matches this tile.
    * ``screentexpos_high_flow``, ``screentexpos_background``, and
      ``screentexpos_floor_flag`` across the main viewport and all
      eight lower viewports.

    Use to debug what data drives a specific visual on the map.

``tile-flow [<wx> <wy> [<wz>]]``
    Dumps the per-tile ``liquid_flow`` bitfield (``temp_flow_timer``,
    ``temp_dir``, ``perm_flow_dir``, ``sink_dist``) at the named coord
    (or under the mouse), then prints a viewport-wide histogram of
    ``temp_flow_timer`` values. Use this to verify the ripple data
    exists on tiles where ``list-flows`` shows no flow_info — it's
    the data source for the broader ``~`` decoration ASCII shows
    outside the surf zone.

``paint-test on|off``
    When on, stamps the placeholder wave sprite at every cell in the
    viewport — no material filter, no flow_info dependency. If the
    whole viewport fills with magenta, ``screentexpos_high_flow``
    renders and the index math is right; if only part fills, the
    index math is wrong; if nothing fills, the layer isn't rendering
    or sprite registration failed.

``clobber-check on|off``
    When on, stamps a sentinel value into 16 ``screentexpos_high_flow``
    slots *before* DF's render call each frame, then counts how many
    survived afterwards. If none survive, DF clears the layer per frame
    and we're safe writing post-render. If some survive, DF only writes
    selectively and we may need a suppress mask the way ``cavern-colors``
    zeroes ``floor_flag`` bytes. Sentinels are restored to their prior
    value before the visible frame so this is safe to leave on.

``force-spawn <wx> <wy> <wz> [wave|foam]``
    Synthesizes a transient ``flow_info`` in the plugin's own storage
    (not pushed into ``block->flows``) so the render path can be tested
    on non-coastal maps. Currently a no-op render-side until the
    skeleton is fleshed out.

``clear-synthetic``
    Drops all synthetic flows created by ``force-spawn``.

Development order
-----------------

Validation sequence:

1. ``ocean-waves list-flows`` on a coastal map — confirm OceanWave /
   SeaFoam objects exist at runtime. **Done** (403 waves / 17 foam at
   z=142 of a test save).
2. ``enable ocean-waves`` then look at the ocean — placeholder
   magenta/cyan tiles should appear where flows are. Tests the whole
   pipeline (hook + scan + sprite + layer write) in one step.
3. ``ocean-waves clobber-check on`` then ``ocean-waves`` to read
   survival stats — confirms whether DF clears
   ``screentexpos_high_flow`` per frame or selectively, which decides
   whether we need a suppress mask.
4. Replace placeholder sprites in ``load_sprites()`` with real
   wave/foam artwork via ``Textures::loadTileset``.
5. Decide animation tick source (``enabler->gputicks`` vs
   ``cur_year_tick``) by toggling pause and observing vanilla water.
