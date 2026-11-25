# PD2 Roguelike

An experimental **single-player** Roguelike mod for classic Diablo II: Lord of Destruction and Project Diablo 2. This is not a Diablo II: Resurrected mod.

> **Compatibility:** The current public release, v0.7.0, requires the exact **Project Diablo 2 Season 13 patch #2** files. It does not support Season 14. The newer S14 development build is not yet available through this launcher.
>
> **Offline only:** The mod does not block PD2 Online. Connecting online with the mod installed may put your PD2 account at risk. Use a separate game copy and choose Single Player.

## Install v0.7.0

1. On Windows, prepare an installed classic Diablo II: Lord of Destruction game with PD2 Season 13 patch #2. You need your own game files; they are not included in this download.
2. Close the game and make a separate copy of the entire Diablo II folder, including its `ProjectD2` folder. The launcher modifies the folder you select; don't select your original if you want to keep it unchanged.
3. Download **PD2-Roguelike.exe** from [the latest release](https://github.com/epleaner/d2-roguelike/releases/latest) and place it in the copied Diablo II folder (the folder containing `ProjectD2`).
4. Run the launcher, select the copied folder if needed, and choose **Play**. The first installation requires an internet connection. The executable is unsigned, so Windows may display a reputation warning.
5. Choose **Single Player**. In town, type `run start` in game chat and click the entrance portal. This command is required by the public v0.7.0 build.

To resume a saved run, load the same character in another Single Player game, type `run start`, and select **Resume Run**. An uncleared room must be fought again. Close the game completely before choosing Play again.

## Updates and troubleshooting

The launcher checks GitHub for the latest mod release when you choose Play. If the update service is unavailable, it reports the error and can start a valid existing installation; a first installation still needs a connection. Updates preserve character saves and route snapshots. New launcher executables must be downloaded separately; the launcher does not update itself.

The launcher verifies exact game files and rejects unsupported versions. PD2 updates can therefore make an installation incompatible. This release does not install or downgrade PD2 for you. Use a short installation path with ASCII characters only.

## Source and notices

The [release](https://github.com/epleaner/d2-roguelike/releases/latest) includes corresponding launcher and native source archives. Third-party notices are available from **Licenses** in the launcher. The launcher artwork contains an image from Diablo II, copyright Blizzard Entertainment. This fan-made project is not affiliated with or endorsed by Blizzard.
