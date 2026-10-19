# Credits

God of War 3 Launcher (gow3_xbox) runs God of War III Remastered on the Xbox Series X|S in
Developer Mode from the player's own dump. It contains no game code or game assets.

## Built on

- **gow3_pc** by Felipe Prado and its contributors
  (https://github.com/FelipePrado0/gow3_pc): the PC port and launcher of God of War III
  Remastered. Its native loader, runtime, renderer integration and game patches are the base
  of this project, which is a fork of it. GPL-2.0.
- **bloodborne_pc** by Supermedo (https://github.com/Supermedo/bloodborne_pc), which gow3_pc
  started from. GPL-2.0.
- **shadPS4** (https://github.com/shadps4-emu/shadPS4): the PS4 graphics emulation, its shader
  recompiler and Vulkan renderer (`gpu/shadps4`). GPL-2.0.
- **Mesa / Dozen**, Vulkan on Direct3D 12, from the fork
  https://github.com/Cosmo/mesa_ai_slop_version (`vulkan_dzn.dll`). MIT.
- **DirectX Shader Compiler** (`dxil.dll`) by Microsoft. Its licenses are in `ThirdParty`.

## Game patches

`patches/God_of_War_III_Remastered.xml` holds shadPS4/GoldHEN-format patches by **cuesta4**
and **kvicken**, ported and extended by gow3_pc (Felipe Prado). Skip Intro and Skip Any Video
build on the PS3 patches by **illusion0001** (illusionyy).

## Libraries in the package

| Library | License |
|---|---|
| FFmpeg 7.1.1 (static, decoders the game uses) | LGPL-2.1 |
| SDL 3 | zlib |
| fmt | MIT |
| Zydis, Zycore | MIT |
| xxHash | BSD-2-Clause |
| Vulkan Memory Allocator | MIT |
| Vulkan-Headers | Apache-2.0 / MIT |
| robin-map | MIT |
| Boost (headers) | BSL-1.0 |
| LibAtrac9 | MIT |
| Dear ImGui | MIT |
| miniz | MIT |
| magic_enum | MIT |
| half | MIT |
| sirit | BSD-3-Clause |
| xbyak | BSD-3-Clause |
| FSR Vulkan | MIT |
| AMD GCN register headers | MIT (in the headers) |
| DejaVu fonts | Bitstream Vera / public domain |
| winpthreads (MSYS2 UCRT64) | MIT / ZPL |

Each license text is in the package's `ThirdParty` folder.

## License

The launcher is GPL-2.0 (`LICENSE.txt`). FFmpeg is linked statically under the LGPL-2.1: it is
ffmpeg-7.1.1 from https://ffmpeg.org, built by `xbox/scripts/build-deps.ps1` with the
configuration shown there. With this project's source you can rebuild the launcher against a
modified FFmpeg.
