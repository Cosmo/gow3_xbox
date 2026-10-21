# God of War 3 Launcher for Xbox

God of War III Remastered on the **Xbox Series X|S in Developer Mode** (not the Xbox One). This
fork of [SpartanPort](https://github.com/FelipePrado0/SpartanPort), the PC port by Felipe Prado and
its contributors, adds the Xbox app (`xbox/`); everything about the port itself is in
SpartanPort's README.

> **No game files are included.** You need a dump of God of War III Remastered from your own PS4
> (tested: CUSA01715). This project is not affiliated with or endorsed by Sony Interactive
> Entertainment or Santa Monica Studio. God of War is a trademark of Sony Interactive Entertainment.

## Install

1. Download the zip from [Releases](https://github.com/Cosmo/gow3_xbox/releases) and unzip it on a PC.
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
   then **Start game**. **Choose game folder** in the same menu switches to another dump.

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

## Credits and license

[xbox/CREDITS.md](xbox/CREDITS.md) lists everything this builds on. GPL-2.0, like SpartanPort.
