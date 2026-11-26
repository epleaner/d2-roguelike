# PD2 Roguelike

An offline roguelike mode for Project Diablo 2. Start a run in the Abyss, clear compact areas, and choose where to go next. Build your character as you descend; the route, enemies, and rewards change from run to run.

**Requires Windows, classic Diablo II: Lord of Destruction, and Project Diablo 2 Season 14 Patch #2.** This is not a Diablo II: Resurrected mod.

**Single Player only.** The mod does not block PD2 Online, and connecting with it installed may put your account at risk.

## The run

- The Rogue Encampment is a hub with no wilderness exit. Use the Abyss entrance to start or resume a run.
- After a combat clear, pick from up to three one-way portals, arranged together when space permits. Combat portal labels show the encounter, area level, modifiers, and reward.
- Fight through compact areas drawn from all five acts. Monsters, layouts, and modifiers vary; boss encounters can appear as you get deeper.
- Visit markets for supplies and hirelings, sanctuaries for shrines and elixirs, and treasure rooms for locked chests.
- Gain a level on each combat clear, in addition to normal monster experience. Rewards can focus on equipment, sockets, uniques, sets, jewelry, or charms. Unique and set reward zones can drop items up to six levels above the normal reward level.
- Use a reroll item or an Edict on an unopened portal to change that choice without changing the others.
- After a kill, chat shows how many monsters remain in the area, including monsters you haven't encountered yet. The final kill doesn't print a count.

Each sanctuary has healing, experience, stamina, and skill shrines, plus one random shrine. An unlocked chest holds 1–5 aura elixirs and 10 rejuvenation potions. Elixir availability scales with character level; full rejuvenation potions gradually replace regular ones and make up the entire supply from level 40.

Softcore death ends the run and resets character progression to level one. Normal corpse and gold penalties still apply. The current pool of areas is finite.

## Roguelike items

**Talisman of Transmutation** (`rrl`) rerolls one unopened portal. Right-click it, then click the portal you want to change.

**Edicts** work the same way, but let you choose what the new offer focuses on:

| Item | Code | Rerolls the portal toward |
| --- | --- | --- |
| Overlord's Edict | `qbs` | A boss zone |
| Mercantile Edict | `qmt` | A market |
| Sanctuary Edict | `qsh` | A shrine zone |
| Plunderer's Edict | `qch` | A locked-chest zone |
| Armorer's Edict | `qeq` | Equipment rewards |
| Collector's Edict | `qso` | Socketed gear, gems, runes, and jewels |
| Relic Hunter's Edict | `qun` | Unique equipment |
| Regalia Edict | `qse` | Set equipment |
| Jeweler's Edict | `qjw` | Rings and amulets |
| Charmwright's Edict | `qcm` | Charms |

**Aura elixirs** grant a level-30 Paladin aura for 30 seconds. Akara can stock them, and they can also drop. Each is named “Elixir of” followed by its aura:

- Might `ra1`, Prayer `rpr`, Resist Fire `ra2`, Holy Fire `ra3`, Thorns `ra4`
- Defiance `ra5`, Resist Cold `ra6`, Blessed Aim `ra7`, Cleansing `ra8`, Resist Lightning `ra9`
- Concentration `rb1`, Holy Freeze `rb2`, Vigor `rb3`, Holy Shock `rb4`, Sanctuary `rb5`
- Meditation `rb6`, Fanaticism `rb7`, Conviction `rb8`, Redemption `rb9`, Salvation `rc1`

### Loot filters

The default filter has no hide rules. A third-party filter may hide or rename these items: aura elixirs use the healing-potion item type, while the Talisman and Edicts use the scroll type.

To keep all **31 Roguelike items** visible with their normal names in your existing custom filter, use the opt-in [copy-paste rules](roguelike-filter-rules.txt):

1. In your copied Diablo II installation, find the filter currently selected in the PD2 launcher. Back up that filter first. If `ProjectD2\loot.filter` is a link to a file under `ProjectD2\filters`, edit the selected filter file it points to, not a replacement `loot.filter`. PD2 recommends keeping personal edits in `ProjectD2\filters\local` and selecting that local filter in the launcher so public filter updates do not overwrite them.
2. Copy the **contents** of `roguelike-filter-rules.txt` into the **top** of your existing filter, before any hide, rename, or catch-all rules that might match these items. Do not install the snippet by itself as a new filter: it does not contain your other rules and cannot preserve their behavior alone. Leave the rest of your filter unchanged.
3. Save the selected filter file and reload it in game (default hotkey **Numpad 0**, configurable in Configure Controls). Make sure **Enable Loot Filter** is on. If an item is still missing, try the **Show All Items** filter level or temporarily disable the loot filter.

The Roguelike launcher does **not** automatically modify your selected filter or paste these rules. The rules match only the 31 released custom item codes; other items continue through your existing rules. See the [PD2 filter guide](https://wiki.projectdiablo2.com/wiki/Item_Filtering) for syntax, rule order, filter selection, and reload controls.

## Install

1. Install classic Diablo II: Lord of Destruction and **Project Diablo 2 Season 14 Patch #2** on Windows. Other PD2 patches and Diablo II: Resurrected are not supported.
2. Close the game. Make a copy of the entire Diablo II folder, including `ProjectD2`. The launcher modifies the folder you select, so use the copy if you want to keep your original installation unchanged.
3. Download **[PD2-Roguelike.exe](https://github.com/epleaner/d2-roguelike/releases/latest)** from the latest release and put it in the copied Diablo II folder, alongside `ProjectD2`.
4. Run the launcher and select the copied folder if it isn't already selected. Click **Play**. The first install needs an internet connection.
5. Choose **Single Player**. The entrance appears automatically in town. Click **Enter the Abyss** to begin.

Windows may warn about the launcher because it is unsigned. **If you have v0.8.3 or earlier, download the new executable:** older launchers cannot install this update or update themselves.

## Save and resume

Use **Save and Exit** to pause a run. Load the same character in a new Single Player game and click **Return to the Abyss**. An interrupted area starts over, and items left on the ground are not saved.

After a Softcore death, return to town and a new entrance opens automatically. Close the game completely before using the launcher again.

## Updates and troubleshooting

The launcher checks for mod updates when you click **Play**. If the update service is unavailable, it reports the error and can still launch a valid existing installation. Updates preserve character saves and route snapshots.

The launcher checks the game files against the supported version. If it rejects your installation, check that it is S14 Patch #2 and that you selected the full Diablo II folder, not just `ProjectD2`. Use a short path containing only ASCII characters.

The launcher accepts a `ProjectD2\loot.filter` link only when it points to a regular filter file inside the copied installation's `ProjectD2\filters` folder. If a copied link still points to your original installation, select a local filter in the copied PD2 launcher so it creates a link within the copy; do not change the original installation's filter.

## Source and notices

The [release](https://github.com/epleaner/d2-roguelike/releases/latest) includes corresponding launcher and native source archives. Third-party notices are available from **Licenses** in the launcher. The launcher artwork contains an image from Diablo II, copyright Blizzard Entertainment. This fan-made project is not affiliated with or endorsed by Blizzard.
