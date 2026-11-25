# PD2 Roguelike

An experimental **single-player** Roguelike mod for classic Diablo II: Lord of Destruction and Project Diablo 2. This is not a Diablo II: Resurrected mod.

> **Compatibility:** v0.8.0 requires the exact **Project Diablo 2 Season 14 Patch #2** game archives (plus the matching classic Diablo II: Lord of Destruction files). It does not install, downgrade, or adapt other PD2 patches. The previous launcher cannot update itself: download the latest **PD2-Roguelike.exe** separately.
>
> **Offline only:** The mod does not block PD2 Online. Connecting online with the mod installed may put your PD2 account at risk. Use a separate game copy and choose Single Player.

## Install v0.8.0 (S14 Patch #2)

1. On Windows, prepare an installed classic Diablo II: Lord of Destruction game with **PD2 Season 14 Patch #2**. You need your own game files; they are not included in this download. Other patches or game archive versions are not supported.
2. Close the game and make a separate copy of the entire Diablo II folder, including its `ProjectD2` folder. The launcher modifies the folder you select; don't select your original if you want to keep it unchanged.
3. Download the latest **PD2-Roguelike.exe** from [the latest release](https://github.com/epleaner/d2-roguelike/releases/latest) and place it in the copied Diablo II folder (the folder containing `ProjectD2`). An older launcher does not update itself and cannot install this release.
4. Run the new launcher, select the copied folder if needed, and choose **Play**. The first installation requires an internet connection. The executable is unsigned, so Windows may display a reputation warning.
5. Choose **Single Player**. In town, click **Enter the Abyss** to start a run; the entrance appears automatically.

To resume a saved run, load the same character in another Single Player game and click **Return to the Abyss** at the automatically opened entrance. An uncleared room must be fought again. If you die and return to town in the same game, type `run start` in game chat to open another entrance. Close the game completely before choosing Play again. Character saves and route snapshots are preserved across mod updates.

## Updates and troubleshooting

The launcher checks GitHub for the latest mod release when you choose Play. If the update service is unavailable, it reports the error and can start a valid existing installation; a first installation still needs a connection. New launcher executables must be downloaded separately; the previous launcher cannot update itself to v0.8.0.

The launcher verifies exact game files and rejects unsupported archives, including other PD2 patches. Use a short installation path with ASCII characters only.

## Source and notices

The [release](https://github.com/epleaner/d2-roguelike/releases/latest) includes corresponding launcher and native source archives. Third-party notices are available from **Licenses** in the launcher. The launcher artwork contains an image from Diablo II, copyright Blizzard Entertainment. This fan-made project is not affiliated with or endorsed by Blizzard.
