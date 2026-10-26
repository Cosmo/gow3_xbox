# God of War 3 Launcher for Xbox

God of War III Remastered on the **Xbox Series X|S in Developer Mode** (not the Xbox One). This
fork of [SpartanPort](https://github.com/FelipePrado0/SpartanPort), the PC port by Felipe Prado and
its contributors, adds the Xbox app (`xbox/`); everything about the port itself is in
SpartanPort's README.

> **No game files are included.** You need a dump of God of War III Remastered from your own PS4
> (tested: CUSA01715). This project is not affiliated with or endorsed by Sony Interactive
> Entertainment or Santa Monica Studio. God of War is a trademark of Sony Interactive Entertainment.

## Install

1. Download the zip from [Releases](https://github.com/Marco2000XboxBrasil/gow3_xbox_PTBR/releases/tag/Xbox_UWP) and unzip it on a PC.
2. Device Portal → **Add** → the `.appx` and the package in `Dependencies`. If Dev Home lists the
   app as an App, set its type to **Game**.
3. Copy your dump (the folder with `eboot.bin`, `sce_module`, `sce_sys` and `dvdroot_ps4`) to one of:
   - **a USB drive**, in any folder. The app looks in `\dumps\GodOfWar3` by itself and otherwise
     asks you to choose the folder once.
   - **the console's internal storage**, anywhere under `D:\DevelopmentFiles` (for example
     `D:\DevelopmentFiles\dumps\GodOfWar3`). The app finds it there by itself, up to three folders
     deep. This is the Dev Mode folder that apps may read; other places on the internal storage
     are closed to apps.
4. Start **God of War 3 Launcher**: **Prepare** (once), **Precompile shaders** (once, recommended),
   then **Start game**. The menu shows the game folder it uses; **Choose game folder** lists the
   dumps the app found (internal storage and USB) and can browse a USB drive.

In the game, **RS + LT** (click the right stick while holding the left trigger) opens the menu:
render resolution (720p by default), overlay, Automatic QTE, vibration and more.

New versions install over earlier ones and keep your saves and settings.

## Known issues

- **4K needs more memory than the Xbox allows.** In Developer Mode an app gets at most 5 GiB. At
  4K the app moves about 450 MiB of the game's memory to disk and runs at about 45 fps; heavy
  scenes can stutter or run out of memory. At 720p (the default) nothing goes to disk.
- **Occasional ghosting.** Now and then moving parts of the picture leave strong trails, for
  example during the opening camera move on Gaia's back. It doesn't happen on every start; it
  seems to come with frame rate drops, and its cause isn't known yet.

## Prints de Tela:
<img width="1280" height="720" alt="Unknown-2026_10_26-00_33_01" src="https://github.com/user-attachments/assets/39d5451d-0835-4d89-8b3c-fc63e16a5987" />
<img width="1280" height="720" alt="Unknown-2026_10_26-00_32_56" src="https://github.com/user-attachments/assets/86295cec-a4be-4ea5-9eb6-8662d4f5c43c" />
<img width="1280" height="720" alt="Unknown-2026_10_26-00_32_53" src="https://github.com/user-attachments/assets/77ea9307-da50-4198-911b-5f41e8ecf6c8" />
<img width="1280" height="720" alt="Unknown-2026_10_26-00_30_58" src="https://github.com/user-attachments/assets/2f592e1e-92e1-4544-88b0-672720af4edc" />
<img width="1280" height="720" alt="Unknown-2026_10_26-00_30_49" src="https://github.com/user-attachments/assets/16fa62e6-f874-4719-89b9-bfcb5eb2de65" />
<img width="1280" height="720" alt="Unknown-2026_10_26-00_33_03" src="https://github.com/user-attachments/assets/d921e66b-5dbd-43a8-92b8-f716f8f3b424" />


## Credits and license

[xbox/CREDITS.md](xbox/CREDITS.md) lists everything this builds on. GPL-2.0, like SpartanPort.
