# Stereo 3D display for Source games (`sourcevr_display`)

A VR module (`sourcevr.so`) that presents a stereoscopic display (a 3D TV, a projector, any screen with anaglyph glasses) to the engine as its headset. Source games from the 2013 SteamVR era still carry Valve's VR render path; with this module the engine itself renders both eyes, with real stereo geometry, into one frame the display unpacks. No per-game shader fixes, no wrapper around the renderer.

**This branch (`stereo3d-full-res-wip`) is the version played on a 3D TV** (module `ed82a30e`, rebuilt byte-identically by `build32-hl2.sh`). It adds the full formats: each eye rendered at the whole 2D size (1920x1080) and shrunk into the frame the screen already has, supersampled, sharper, distant things less pixelated and no texture popping (run q24). Nothing is resized: no video mode, no gamescope screen change, no `-S stretch` (which breaks the relative mouse, run q23). Full top and bottom is the recommended default. Every format and output has run on the TV: natively (run q26) and through gamescope, anaglyph included (runs q24 and q27). The mouse regression of run q21 is fixed (run q22, `2bc521b`). The branch `stereo3d-sbs-native` holds the earlier played version (`5db63a08`, half formats) and the Windows port; the two meet before the pull request.

Status: work in progress, being prepared as a pull request to Valve. Where it fails is listed below, plainly; we are already chasing those fixes. The story, measurements and dated test runs are in [sony-bravia-linux](https://github.com/danielcamposramos/sony-bravia-linux/tree/main/tools/vr-stereo-spectator); the reports to Valve are [Source-1-Games#8297](https://github.com/ValveSoftware/Source-1-Games/issues/8297) and [SteamVR-for-Linux#961](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/961).

## What works

Played on a Sony KDL-46HX855 over HDMI, Half-Life 2 (native Linux, Vulkan), NVIDIA RTX 3060 (driver 615.71.09), Debian testing, KDE Plasma on Wayland, 2026-09-26 to 2026-09-28.

- A 3D section in the game's own options (GamepadUI, Options > Video): Stereo 3D, 3D format, 3D output, Swap eyes. Nothing changes until Apply, which reloads 3D once with all four. The choice is saved; the next launch starts in it.
- Native, the game straight to the 3D display: top and bottom, full (recommended, the default), side by side, full, and the half formats, the lighter option for weaker machines. All four switch from the menu on Apply (run q26).
- Through gamescope: the same four formats (runs q24 and q27), with the outputs 3D display, red/cyan anaglyph (CRT and modern-screen profiles), row-interleaved and checkerboard, switched from the menu with no relaunch.
- Swap eyes; the menus and HUD at full size, at zero depth; the launch option `-stereo3d`.
- While 3D is on, the module turns motion blur off (its previous-frame state is shared by both eyes and smears differently in each) and sets anisotropic filtering to 16x; the player's own values come back when 3D ends, or at the next start if the game quit in 3D.

## Spectating in 3D (SourceTV and the observer cameras)

Stereo is the 2D game with a second camera. Every view the game already has goes through the same VR path, so every one gets its second eye with no map or game changes: the player's own view, the spectator's in-eye, chase and free cameras, the cameras mappers place (`info_observer_point`), SourceTV's director, scripted cameras (`point_viewcontrol`). The game's own rules still decide what is drawn: with this branch's client the crosshair is placed as in 2D, so it shows in first person and in-eye and not in chase or fixed cameras, and the module hands the crosshair back to any client that asks for its display interface (with Valve's shipped clients it keeps drawing its own).

SourceTV transmits the game's state, not video: every viewer's game renders the picture, so one broadcast serves 2D and 3D viewers alike, each in the format they choose.

Tested 2026-09-28 (runs r04-r06, Half-Life 2: Deathmatch built from this branch on Source SDK Base 2013 Multiplayer, 64-bit, native to a 3D TV): the crosshair centred while playing; the spectator's free camera in 3D; a SourceTV demo recorded on a listen server, played back in 3D. That demo stops at the same point in 2D and in 3D with Source's own `Host_Error: CL_PreserveExistingEntity: missing client entity`, a known class of Source demo error, not the stereo. Mapper-placed spectator cameras are not tried yet (the installed map has none).

## Known failures (and what we are doing about them)

- The full formats reach the TV as supersampled half frames, the only frames a 1080p 3D TV unpacks. A display that takes a larger frame (a 4K passive set, a headset through gamescope's VR overlay) needs the game's screen at that size, which is the next step, with no resize at run time.
- Frame packing (`-stereo3d fp1080` / `fp720`) needs a video mode of the frame's size, which the display driver must offer as an HDMI 3D mode; the proprietary NVIDIA driver used here offers none (our report: [NVIDIA/open-gpu-kernel-modules#1382](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/1382)). It is not in the menu; the launch option exists, and inside gamescope it looked like top and bottom (1080p) or showed a black bar (720p).
- Loading screens, natively: drawn once across the stereo frame, so each eye sees half of one flat image (the spinner and progress bar as ghosts in top and bottom). The engine draws them while no view renders and the module receives no frame to compose: with `SVRTV_CALLTRACE=1` the log shows the calls stopping for the whole load (3.6 s and 1.6 s in run q28), only `ShouldRunInVR` queries while the loading image is drawn. The loading screen lives in closed code (the engine, GameUI, GamepadUI), so natively this is an ask to Valve: in VR mode, draw it through the UI sheet and the module's composite, like the HUD. **Inside gamescope it is solved** (run q33): the module marks every frame it builds (its two bottom-right pixels), and the effect puts any frame without the mark, the engine's own loading screen with its spinner and progress bar, whole into both halves; Valve's pictures stay untouched.
- Row-interleaved and checkerboard exist only through gamescope; there is no native version. They render, but no passive or DLP screen has been tried yet.
- Spectator mode (a real headset, with the PC's window showing its two eyes in stereo) is not built. Today the display is the headset: play mode only. It is the ask in SteamVR-for-Linux#961.
- The OpenGL renderer crashes in VR mode (reported in #8297); use `-vulkan`.
- Half-Life 2's shipped client is 32-bit and cannot be rebuilt from this SDK, so the client fixes in this branch (muzzle-flash position, menu size, HUD overlay) reach 64-bit SDK games only (tested on Half-Life 2: Deathmatch through Source SDK Base 2013 Multiplayer). On Half-Life 2 the muzzle flash needs the workaround below.
- Multiplayer: the 64-bit launcher loads the module only with `-vr`, and the engine warns that an unsigned `sourcevr` module blocks secure servers.
- Windows: the module is ported (2026-09-27) but not yet built with Microsoft's compiler, nor run. Its own code compiles against real Windows headers (MinGW syntax check: 0 errors in `sourcevr_display.cpp`; Valve's `tier0` headers need MSVC, as always on Windows), and the Linux build is unchanged, byte for byte (`5db63a08`). Next: build it in Visual Studio, try it under Proton, then on Windows itself. The gamescope outputs stay Linux only, as gamescope is; on Windows the menu offers the native formats.

## How to make it work (Half-Life 2 on Linux)

You need a 3D display that accepts half top and bottom or half side by side over HDMI (or any colour screen plus red/cyan glasses, through gamescope), Docker for the build, and Half-Life 2 from Steam (native Linux version).

### 1. Build the module

```sh
git clone -b stereo3d-full-res-wip https://github.com/danielcamposramos/source-sdk-2013.git
cd source-sdk-2013/src/sourcevr_display
./build32-hl2.sh "$HOME/.steam/steam/steamapps/common/Half-Life 2/bin" out
```

Use your own library path for `Half-Life 2/bin`. The script builds in Valve's Steam Runtime SDK image and prints the module's SHA-256; `stereo3d-full-res-wip` gives `ed82a30e…`, the played build.

### 2. Install it

```sh
HL2="$HOME/.steam/steam/steamapps/common/Half-Life 2"
cp -n "$HL2/bin/sourcevr.so" "$HL2/bin/sourcevr.so.valve"   # keep Valve's module
cp out/sourcevr.so out/svrtv-anaglyph.fx "$HL2/bin/"
```

A Steam file verification puts Valve's module back; copy ours again afterwards.

### 3. Add the 3D menu

Extract `gamepadui/options.res` from `hl2/hl2_pak_dir.vpk` with any VPK tool (the Python `vpk` package, for example), insert [content/gamepadui-options-3d.res](content/gamepadui-options-3d.res) into the Video tab's items right after `DisplayMode`, and save it as `Half-Life 2/hl2/custom/stereo3d-menu/gamepadui/options.res`.

### 4. Launch options (Steam, Half-Life 2, Properties)

```
-vulkan -gamepadui
```

3D follows the menu's saved choice. `-stereo3d` starts in 3D whatever was saved (in the saved format); `-stereo3d tab` or `-stereo3d sbs` picks the format.

### The muzzle flash, cheats and achievements

Half-Life 2's muzzle flash sits beside the gun in 3D (the 32-bit client rescales it for a 2D field of view). The fix is one setting, `viewmodel_fov 90`, which Half-Life 2 treats as a cheat:

```
-vulkan -gamepadui +sv_cheats 1 +viewmodel_fov 90
```

- With cheats: the muzzle flash sits on the gun, and Steam achievements are off while `sv_cheats 1` is set.
- Without cheats: achievements work, and the muzzle flash shows beside the gun. Everything else is the same.

This lasts until Valve accepts this use as not cheating. The client fix in this branch (`c_baseviewmodel.cpp`, gated on the display module, `viewmodel_fov` still a cheat) does it for the games Valve builds from this SDK; for Half-Life 2 it needs Valve's build.

### Can this be used to cheat?

Yes, and we say so plainly. Stereo 3D needs a second camera: the engine draws the world twice, once from each eye, a few centimetres apart. A camera the game did not plan for is exactly what a cheat needs: moved further away, it could show what the player should not see from where they stand. This module keeps the eyes at a normal human distance (2.5 units, about 64 mm) and its source is here for anyone to check. In multiplayer the engine warns that an unsigned `sourcevr` module blocks secure (VAC) servers: this is for single player.

### 5. Play

Options > Video > Stereo 3D: Enabled, 3D format: Top and bottom (recommended), Apply. Then set the display to its matching 3D mode by hand: the PC sends no HDMI 3D signal here, so the TV does not switch on its own.

### 6. Optional: through gamescope (anaglyph, rows, checkerboard)

Inside gamescope the menu offers every output. Two things make it smooth:

- gamescope-3dtv: gamescope with its nested output off FIFO. Stock gamescope works, but the game and gamescope race for frames and the depth swims during mouse look. The fix is [ValveSoftware/gamescope#2438](https://github.com/ValveSoftware/gamescope/pull/2438); until it is merged, [build it from our patch](https://github.com/danielcamposramos/sony-bravia-linux/tree/main/tools/vr-stereo-spectator/gamescope) (`build.sh` makes a Debian package; put its `usr/games/gamescope` in `~/.local/opt/gamescope-3dtv/` and the `gamescope-3dtv` wrapper in `~/.local/bin/`; keep your distribution's gamescope of the same version installed for `gamescopereaper`).
- A free-running game: a file `dxvk.conf` in the `Half-Life 2` folder containing `d3d9.presentInterval = 0`. The game's DXVK 2.0 reads it through `DXVK_CONFIG_FILE`, not through `DXVK_CONFIG`.

Launch options, with your own paths:

```
DXVK_CONFIG_FILE="/path/to/Half-Life 2/dxvk.conf" SDL_VIDEODRIVER=x11 gamescope-3dtv --backend sdl -f -W 1920 -H 1080 -w 1920 -h 1080 -- env -u WAYLAND_DISPLAY %command% -vulkan -gamepadui
```

The module installs its anaglyph effect into gamescope's ReShade folder when needed and switches it from the menu. On our machine (desktop on an AMD iGPU, game on the NVIDIA card) the game also needed `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia __VK_LAYER_NV_optimus=NVIDIA_only` and gamescope `--prefer-vk-device 10de:2504`; a single-GPU machine should not need them. Our exact working launch is [play.env.example](https://github.com/danielcamposramos/sony-bravia-linux/blob/main/tools/hl2-bench/play.env.example), read by our Steam wrapper [game-wrap.sh](https://github.com/danielcamposramos/sony-bravia-linux/blob/main/tools/hl2-bench/game-wrap.sh).

### Windows

Today's Half-Life 2 on Windows is 32-bit: Steam launches `hl2.exe` and lists no 64-bit Half-Life 2 (Half-Life 2: Deathmatch, by contrast, has `hl2mp_win64.exe`). So Half-Life 2 takes the 32-bit `sourcevr.dll` in its `bin` folder; a 64-bit Source engine takes the 64-bit one next to its own `engine.dll`.

- Valve's route: run `createallprojects.bat` in `src`, open the generated `everything.sln` in Visual Studio 2022 (the SDK's README lists the workload and Windows SDK) and build the `sourcevr_display` project (the SDK builds 64-bit).
- Our cross-build (2026-09-28): both architectures from Linux with clang-cl and lld-link against Microsoft's static CRT and Windows SDK, with the defines of Valve's `source_dll_win32_*.vpc`. Both load and pass the geometry test under Wine (four formats), with a stand-in `tier0.dll`: [the scripts](https://github.com/danielcamposramos/sony-bravia-linux/tree/main/tools/vr-stereo-spectator/sourcevr/windows).

Install: keep Valve's `bin\sourcevr.dll` (if the game has one) as `sourcevr.dll.valve`, copy the module in, and put the menu file (step 3) at `hl2\custom\stereo3d-menu\gamepadui\options.res`. Launch options: as on Linux, without `-vulkan` (the game's own Direct3D 9). The anaglyph, rows and checkerboard outputs are gamescope's, so they are Linux only; on Windows the menu offers the display formats. The first run inside the game on Windows is being tested.

### Back to stock

Stereo 3D: Disabled and Apply for 2D. To remove it, first turn Stereo 3D off in the game and quit (if the game quit in 3D, the module still holds your crosshair, motion blur and filtering values, in `svrtv-*` files next to it, to put back at the next start; Valve's module would not), then `mv "$HL2/bin/sourcevr.so.valve" "$HL2/bin/sourcevr.so"` and delete `hl2/custom/stereo3d-menu`.

### Troubleshooting

`SVRTV_LOG=svrtv.log %command% …` writes the module's log next to it in `bin/`.
