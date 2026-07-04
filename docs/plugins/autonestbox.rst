autonestbox
===========

.. dfhack-tool::
    :summary: Auto-assign egg-laying adult female pets to nestbox zones.
    :tags: fort auto animals

To use this feature, you must create pen/pasture zones on the same tiles as
built nestboxes. If the pen is bigger than 1x1, the nestbox must be in the top
left corner. Only 1 unit will be assigned per pen, regardless of the size. Egg
layers who are also grazers (like elk birds) will be ignored, since confining
them to a 1x1 pasture will starve them. Only domesticated units or tamed units
with actively assigned trainers are pastured since half-trained wild egg layers
could destroy your neat nestbox zones when they revert to wild.

Dwarf Fortress sometimes has an animal claim a nestbox other than the one in
its assigned zone. When this happens, `autonestbox` reconciles the situation
automatically where it can. If the animal can be assigned to the zone that
contains its claimed nestbox, the zone is reassigned to it, freeing any
previous occupant to be matched with a different nestbox zone. If the animal
is committed elsewhere -- for example, it is already nesting in a different
nestbox zone, or it is pastured, caged, or chained somewhere it cannot use the
nestbox -- the redundant claim is cleared so the nestbox becomes available
again. An announcement is made for situations `autonestbox` cannot fix by
itself, such as a nestbox claimed by a free-roaming animal that `autonestbox`
will not manage. No action is taken when the claiming animal is pastured in a
zone that covers its claimed nestbox, since that arrangement works on its
own.

Usage
-----

``enable autonestbox``
    Start checking for unpastured egg-layers and assigning them to nestbox
    zones.
``autonestbox``
    Print current status.
``autonestbox now``
    Run a scan and assignment cycle right now. Does not require that the plugin
    is enabled.
