prefer-stockpile
================

.. dfhack-tool::
    :summary: Prefer nearby stockpile items for workshop jobs.
    :tags: fort productivity jobs

By default, when a dwarf accepts a crafting job, DF picks the nearest
suitable ingredient to *the dwarf* — which can mean hand-hauling a heavy
boulder from the far end of the map to a workshop that's surrounded by an
identical boulder sitting in a stockpile right next to it.

``prefer-stockpile`` intercepts newly created jobs at tracked workshops and
pre-attaches a suitable item from a designated preferred stockpile before DF
runs its own selection.  If no suitable item is found in any preferred
stockpile, the job proceeds normally (DF falls back to its standard
nearest-to-dwarf search).  Unlike vanilla stockpile-to-workshop links, jobs
are never blocked — the preferred stockpile is a hint, not a requirement.

Usage
-----

::

    enable prefer-stockpile
    prefer-stockpile
    prefer-stockpile list-buildings
    prefer-stockpile link-workshop <workshop-id> <stockpile-id>...
    prefer-stockpile link-stockpile <stockpile-id> <workshop-id>...
    prefer-stockpile unlink-workshop <workshop-id> [<stockpile-id>...]
    prefer-stockpile unlink-stockpile <stockpile-id> [<workshop-id>...]

Enable the plugin with ``enable prefer-stockpile`` before adding links;
otherwise no pre-attachment happens.  Run ``prefer-stockpile`` with no
arguments to see current links.

``list-buildings``
    Lists all workshops (and furnaces) with their building IDs and
    positions, followed by all stockpiles with their building IDs and
    names.  Use this to find the IDs needed for the link commands.

``link-workshop <workshop-id> <stockpile-id>...``
    Registers one or more preferred stockpiles for a workshop.  When a
    job is created there, the plugin searches those stockpiles first for
    suitable ingredients.  Stockpiles are searched in the order they
    were added.

``link-stockpile <stockpile-id> <workshop-id>...``
    Registers a stockpile as preferred for one or more workshops.
    Equivalent to running ``link-workshop`` once per listed workshop.

``unlink-workshop <workshop-id> [<stockpile-id>...]``
    Removes preference links from a workshop.  With specific stockpile
    IDs, removes only those links.  With no stockpile IDs, removes all
    links for that workshop.

``unlink-stockpile <stockpile-id> [<workshop-id>...]``
    Removes a stockpile from the preference lists of specific workshops,
    or from all workshop preference lists if no workshop IDs are given.

Notes
-----

* Links are not persisted across save/load yet.  Re-add them after
  loading a save.
* The plugin runs on every game tick (``plugin_onupdate``) and scans
  only jobs created since the last tick, so the overhead is minimal.
* Item selection checks: not already in a job, not forbidden, sitting
  in the preferred stockpile (via ``item->getStockpile()``), and
  passing DFHack's ``isSuitableItem`` / ``isSuitableMaterial`` filters.
* Enable DFHack debug output for ``prefer_stockpile:log`` to see
  per-attachment log lines.
