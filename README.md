# PD2 Roguelike

An offline roguelike mode for Project Diablo 2. Start a run in the Abyss, clear compact areas, and choose where to go next. Build your character as you descend; the route, enemies, and rewards change from run to run.

**Requires Windows, classic Diablo II: Lord of Destruction, and Project Diablo 2 Season 14 Patch #2.** This is not a Diablo II: Resurrected mod.

**Single Player only.** The mod does not block PD2 Online, and connecting with it installed may put your account at risk.

## The run

- After a combat clear, pick from up to three one-way portals. Combat portal labels show the encounter, area level, modifiers, and reward.
- Fight through compact areas drawn from all five acts. Monsters, layouts, and modifiers vary; boss encounters can appear as you get deeper.
- Visit markets for supplies and hirelings, sanctuaries for shrines, and treasure rooms for locked chests.
- Gain a level on each combat clear, in addition to normal monster experience. Rewards can focus on equipment, sockets, uniques, sets, jewelry, or charms.
- Use a reroll item or an Edict on an unopened portal to change that choice without changing the others.

Softcore death ends the run and resets character progression to level one. Normal corpse and gold penalties still apply. The current pool of areas is finite.

## Install

1. Install classic Diablo II: Lord of Destruction and **Project Diablo 2 Season 14 Patch #2** on Windows. Other PD2 patches and Diablo II: Resurrected are not supported.
2. Close the game. Make a copy of the entire Diablo II folder, including `ProjectD2`. The launcher modifies the folder you select, so use the copy if you want to keep your original installation unchanged.
3. Download **[PD2-Roguelike.exe](https://github.com/epleaner/d2-roguelike/releases/latest)** from the latest release and put it in the copied Diablo II folder, alongside `ProjectD2`.
4. Run the launcher and select the copied folder if it isn't already selected. Click **Play**. The first install needs an internet connection.
5. Choose **Single Player**. The entrance appears automatically in town. Click **Enter the Abyss** to begin.

Windows may warn about the launcher because it is unsigned. If you have an older Roguelike launcher, download the new executable; it cannot update itself.

## Save and resume

Use **Save and Exit** to pause a run. Load the same character in a new Single Player game and click **Return to the Abyss**. An interrupted area starts over, and items left on the ground are not saved.

After a Softcore death, type `run start` in game chat to open a new entrance in the same game. Close the game completely before using the launcher again.

## Updates and troubleshooting

The launcher checks for mod updates when you click **Play**. If the update service is unavailable, it reports the error and can still launch a valid existing installation. Updates preserve character saves and route snapshots.

The launcher checks the game files against the supported version. If it rejects your installation, check that it is S14 Patch #2 and that you selected the full Diablo II folder, not just `ProjectD2`. Use a short path containing only ASCII characters.

## Source and notices

The [release](https://github.com/epleaner/d2-roguelike/releases/latest) includes corresponding launcher and native source archives. Third-party notices are available from **Licenses** in the launcher. The launcher artwork contains an image from Diablo II, copyright Blizzard Entertainment. This fan-made project is not affiliated with or endorsed by Blizzard.
