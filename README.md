> **Versão em português do Brasil** do launcher de God of War III para Xbox, feita a partir do trabalho de **Cosmo** ([Cosmo/gow3_xbox](https://github.com/Cosmo/gow3_xbox)), que por sua vez se baseia no port para PC **SpartanPort**, de Felipe Prado e colaboradores.
>
> O que esta versão muda: launcher e menu do jogo (RS + LT) em português do Brasil, textos do menu em verde e uma imagem de fundo opcional no launcher (a imagem não está neste repositório; veja `xbox/scripts/background.ps1`).
>
> **Nenhum arquivo do jogo está incluído**: é preciso um dump do seu próprio God of War III Remastered. Este projeto não é afiliado à Sony nem à Microsoft. Licença GPL-2.0, como o projeto original.
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
   - **Start game** first loads every shader the game has used so far ("Loading shader cache").
     This takes longer the further you play (about 3 minutes with 7,000 shaders), but then
     nothing has to be built during play.
   - **Start game without loading shaders** starts at once and builds each shader the first time
     the game needs it. After loading a save or entering a new area, some objects can appear a
     moment late.

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

<img width="1280" height="720" alt="Unknown-2026_10_26-22_58_02" src="https://github.com/user-attachments/assets/a44898aa-fb19-42fd-b1a2-e226a6bd3629" />
<img width="1280" height="720" alt="Unknown-2026_10_26-00_33_03" src="https://github.com/user-attachments/assets/aaf9e21d-4c97-4839-8f16-69c9dcefafda" />
<img width="1280" height="720" alt="Unknown-2026_10_26-23_08_05" src="https://github.com/user-attachments/assets/de6915df-b0d3-4b1c-91ea-7704e79a41bf" />
<img width="1280" height="720" alt="Unknown-2026_10_27-00_30_05" src="https://github.com/user-attachments/assets/aed3da77-1404-49ef-ac4b-3a0f49b2e619" />
<img width="1280" height="720" alt="Unknown-2026_10_27-00_20_14" src="https://github.com/user-attachments/assets/619fd95a-9386-4937-8808-77934bc9210f" />



## Credits and license

[xbox/CREDITS.md](xbox/CREDITS.md) lists everything this builds on. GPL-2.0, like SpartanPort.
