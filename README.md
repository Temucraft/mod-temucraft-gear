# mod-temucraft-gear

Gear for Temucraft: Cataclysm-style reforging and Mists-style item upgrades, two NPCs that improve what you wear.

Before: mod-temucraft-reforging, mod-temucraft-item-upgrade. Their tables, SQL file names, texts (`module_string` rows under their old names), settings and addon prefixes are unchanged, so an existing server only swaps the folders.

## From mod-temucraft-reforging


Cataclysm-style reforging for AzerothCore 3.3.5a. Talk to the Reforger, pick an equipped item, pick one of its
secondary stats and the secondary stat to move it into: 40% (configurable) of the first is taken away and added as
the second. One reforge per item; it stays on the item when traded or mailed, and undoing it is free.

Stats that can be moved: Spirit, Defense, Dodge, Parry, Hit, Critical Strike, Haste, Expertise and Armor Penetration
rating. Heirlooms can't be reforged.

### Use

- Spawn the Reforger: `.npc add 608800`.
- `.temucraft reforge` lists what is reforged on your equipped items.
- The item tooltip shows the new numbers (sent privately to the owner) and a "Reforged: ..." line.

### Install

1. Put this folder in `modules/`, re-run CMake and build.
2. The SQL in `data/sql/` is applied by the worldserver on its next start.
3. Copy `conf/mod-temucraft-reforging.conf.dist` to your configs folder as `mod-temucraft-reforging.conf` and adjust.

### What it adds

| Database | Table | IDs |
|---|---|---|
| world | `creature_template`, `creature_template_model` | 608800 (Reforger) |
| world | `npc_text` | 608800 |
| world | `module_string` | module `mod-temucraft-reforging`, ids 1-15 |
| characters | `mod_temucraft_reforge` | one row per reforged item |

### Translations

All player-facing text is in `module_string`; add rows to `module_string_locale` for other languages. If a row is
missing, the English text built into the script is used.

### Independence

Doesn't need any other module. No core changes; no client changes.

Part of the Temucraft modules for AzerothCore 3.3.5a.

## From mod-temucraft-item-upgrade


Mists-style item upgrades for AzerothCore 3.3.5a. The Upgrade Vendor raises an equipped weapon or armor piece one
upgrade level at a time; each level adds 4% (configurable) to its stats, armor and weapon damage. The price per
level is gold and, optionally, an item (a token or currency of your choice). Upgrades stay on the item when it is
traded or mailed.

### Use

- Spawn the Upgrade Vendor: `.npc add 608801`.
- `.temucraft upgrades` lists the upgrade levels of your equipped items.
- The owner sees the upgraded numbers and "Upgrade Level: x/y" in the tooltip.

### Install

1. Put this folder in `modules/`, re-run CMake and build.
2. The SQL in `data/sql/` is applied by the worldserver on its next start.
3. Copy `conf/mod-temucraft-item-upgrade.conf.dist` to your configs folder as `mod-temucraft-item-upgrade.conf`.

### What it adds

| Database | Table | IDs |
|---|---|---|
| world | `creature_template`, `creature_template_model` | 608801 (Upgrade Vendor) |
| world | `npc_text` | 608801 |
| world | `module_string` | module `mod-temucraft-item-upgrade`, ids 1-12 |
| characters | `mod_temucraft_item_upgrade` | one row per upgraded item |

### Works with other modules

- mod-temucraft-reforging: when both are installed, the tooltip shows the upgrade and the reforge together (each
  module reads the other's table if it exists; neither needs the other to build or run).

### Translations

All player-facing text is in `module_string`; add rows to `module_string_locale` for other languages. Missing rows
fall back to the English built into the script.

Part of the Temucraft modules for AzerothCore 3.3.5a.
