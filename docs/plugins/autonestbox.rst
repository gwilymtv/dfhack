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
its assigned zone. When this happens, `autonestbox` reassigns the zone to the
animal that has actually claimed the nestbox, freeing the previous occupant to
be matched with a different nestbox zone. If a nestbox is claimed by an animal
that `autonestbox` cannot assign to its zone -- for example, an animal that is
not an egg-laying female pet, or one that is pastured, caged, or chained
somewhere outside of `autonestbox`'s control -- an announcement is made so you
can resolve the situation manually. You will also be warned when an animal
that `autonestbox` would otherwise manage has claimed a nestbox that is not in
a manageable zone, such as a nestbox with no pasture zone over it, or one that
is not in the top left corner of its pasture. No warning is given when the
animal is pastured in a zone that covers its claimed nestbox, since that
arrangement works on its own.

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
