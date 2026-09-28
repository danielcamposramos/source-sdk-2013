# Stereo 3D display for Source games (`sourcevr_display`)

A VR module (`sourcevr.so`) that presents a stereoscopic display (a 3D TV, a projector, any screen with anaglyph glasses) to the engine as its headset. Source games from the 2013 SteamVR era still carry Valve's VR render path; with this module the engine itself renders both eyes, with real stereo geometry, into one frame the display unpacks. No per-game shader fixes, no wrapper around the renderer.

**This branch (`stereo3d-full-res-wip`) is the fix being chased, not the version to play.** It adds the full-resolution formats inside gamescope: the module resizes gamescope's nested screen at run time through gamescope's own `GAMESCOPE_XWAYLAND_MODE_CONTROL` property, the pointer fence comes back for windows larger than the UI (never inside gamescope), and the effect gains row and checkerboard variants for the full formats (techniques 9 to 11). The mouse regression of run q21 (the pointer pinned to the centre inside gamescope) is fixed: run q22 (2026-09-27) traced it to an X connection the module opened at start only to log gamescope's screen size, and that call is gone (`2bc521b`). The full formats themselves have not been validated here yet; they also need gamescope's `-S stretch`, because its scale math keeps the nested size given at start (see [the gamescope resolution map](https://github.com/danielcamposramos/sony-bravia-linux/blob/main/tools/vr-stereo-spectator/gamescope/RESOLUTIONS.md)). To play, use [`stereo3d-sbs-native`](https://github.com/danielcamposramos/source-sdk-2013/tree/stereo3d-sbs-native/src/sourcevr_display).

Status: work in progress, being prepared as a pull request to Valve. The branch `stereo3d-sbs-native` is the version played on a 3D TV (module `5db63a08`, rebuilt byte-identically by `build32-hl2.sh`). Where it fails is listed below, plainly; we are already chasing those fixes. The story, measurements and dated test runs are in [sony-bravia-linux](https://github.com/danielcamposramos/sony-bravia-linux/tree/main/tools/vr-stereo-spectator); the reports to Valve are [Source-1-Games#8297](https://github.com/ValveSoftware/Source-1-Games/issues/8297) and [SteamVR-for-Linux#961](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/961).

## What works

Played on a Sony KDL-46HX855 over HDMI, Half-Life 2 (native Linux, Vulkan), NVIDIA RTX 3060 (driver 615.71.09), Debian testing, KDE Plasma on Wayland, 2026-09-26.

- A 3D section in the game's own options (GamepadUI, Options > Video): Stereo 3D, 3D format, 3D output, Swap eyes. Nothing changes until Apply, which reloads 3D once with all four. The choice is saved; the next launch starts in it.
- Native, the game straight to the 3D display: half top and bottom (recommended, the default) and half side by side.
- Through gamescope: the same half formats, with the outputs 3D display, red/cyan anaglyph (CRT and modern-screen profiles), row-interleaved and checkerboard, switched from the menu with no relaunch.
- Swap eyes; the menus and HUD at full size, at zero depth; the launch option `-stereo3d`.
- While 3D is on, the module turns motion blur off (its previous-frame state is shared by both eyes and smears differently in each) and sets anisotropic filtering to 16x; the player's own values come back when 3D ends, or at the next start if the game quit in 3D.

## Known failures (and what we are doing about them)

- Full-resolution formats inside gamescope ("Top and bottom, full" and "Side by side, full" in the gamescope menu) do not work yet. The game's screen is gamescope's nested one, fixed at start, so the doubled video mode is never granted: full top and bottom shows one eye with the aim displaced, and full side by side with anaglyph goes all red. We are chasing it on the branch `stereo3d-full-res-wip`: resizing gamescope's nested screen at run time, then mapping the pointer. That branch currently has its own regression, listed there.
- Frame packing (`-stereo3d fp1080` / `fp720`) needs a video mode of the frame's size, which the display driver must offer as an HDMI 3D mode; the proprietary NVIDIA driver used here offers none (our report: [NVIDIA/open-gpu-kernel-modules#1382](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/1382)). It is not in the menu; the launch option exists, and inside gamescope it looked like top and bottom (1080p) or showed a black bar (720p).
- Row-interleaved and checkerboard exist only through gamescope; there is no native version. They render, but no passive or DLP screen has been tried yet.
- Spectator mode (a real headset, with the PC's window showing its two eyes in stereo) is not built. Today the display is the headset: play mode only. It is the ask in SteamVR-for-Linux#961.
- The OpenGL renderer crashes in VR mode (reported in #8297); use `-vulkan`.
- Half-Life 2's shipped client is 32-bit and cannot be rebuilt from this SDK, so the client fixes in this branch (muzzle-flash position, menu size, HUD overlay) reach 64-bit SDK games only (tested on Half-Life 2: Deathmatch through Source SDK Base 2013 Multiplayer). On Half-Life 2 the muzzle flash needs the workaround below.
- Multiplayer: the 64-bit launcher loads the module only with `-vr`, and the engine warns that an unsigned `sourcevr` module blocks secure servers.

## How to make it work (Half-Life 2 on Linux)

You need a 3D display that accepts half top and bottom or half side by side over HDMI (or any colour screen plus red/cyan glasses, through gamescope), Docker for the build, and Half-Life 2 from Steam (native Linux version).

### 1. Build the module

```sh
git clone -b stereo3d-sbs-native https://github.com/danielcamposramos/source-sdk-2013.git
cd source-sdk-2013/src/sourcevr_display
./build32-hl2.sh "$HOME/.steam/steam/steamapps/common/Half-Life 2/bin" out
```

Use your own library path for `Half-Life 2/bin`. The script builds in Valve's Steam Runtime SDK image and prints the module's SHA-256; `stereo3d-sbs-native` gives `5db63a08…`, the played build.

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

Half-Life 2's muzzle flash sits off the gun in 3D (the 32-bit client rescales it for a 2D field of view). Until Valve ships the client fix, this puts it back:

```
-vulkan -gamepadui +sv_cheats 1 +viewmodel_fov 90
```

`sv_cheats 1` turns achievements off while it is set.

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

### Back to stock

Stereo 3D: Disabled and Apply for 2D. To remove it: `mv "$HL2/bin/sourcevr.so.valve" "$HL2/bin/sourcevr.so"` and delete `hl2/custom/stereo3d-menu`.

### Troubleshooting

`SVRTV_LOG=svrtv.log %command% …` writes the module's log next to it in `bin/`.
