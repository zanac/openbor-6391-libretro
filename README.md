# OpenBOR multi-engine (libretro core and standalone program)

The **OpenBOR** beat 'em up engine with **several official OpenBOR releases**
built in, picking the right one for each game. It comes in two forms built
from the same code:

* a [libretro](https://www.libretro.com/) **core** (`openbor_libretro.so` /
  `.dll` / `.dylib`) for RetroArch and other libretro frontends;
* a **standalone program** (`openbor`, `openbor.exe`) that needs no frontend:
  a small SDL2 host around the same core, with the engine chosen on the
  command line.

The engines:

| Engine | Upstream source | Typical mods |
|---|---|---|
| **v3.0 Build 3711** (2012) | [DCurrent/openbor@6ec17fa4](https://github.com/DCurrent/openbor/commit/6ec17fa411b237f789c910e1234a33a618ab236d) | classic 3.x / Beats of Rage era mods |
| **v3.0 Build 4153** (2015) | [DCurrent/openbor@4a276c8b](https://github.com/DCurrent/openbor/commit/4a276c8b81fe0fc96d0f98b0e535da802c7417f7) | 2014–2016 mods, e.g. *Super Final Fight Gold Plus* |
| **v3.0 Build 6391** (2018) | [DCurrent/openbor tag v6391](https://github.com/DCurrent/openbor/tree/v6391) | the long-lived "stable" most mods were built with |
| **v4.0 Build 7555** (2024) | [DCurrent/openbor@b1676f97](https://github.com/DCurrent/openbor/commit/b1676f97) | recent mods, e.g. *Mighty Final Fight Forever* |
| **v4.0 Build 7735** (2025) | [DCurrent/openbor@f64d63c2](https://github.com/DCurrent/openbor/commit/f64d63c2fc4e0c6e8993a58fbce1816502fff63a) | 2025 mods, e.g. *Saint Seiya Sagas* (shipped with this build) |
| **v4.0 Build 8025** (2026) | [DCurrent/openbor@787b6770](https://github.com/DCurrent/openbor/commit/787b6770409935137579715febf80cf7a529b748) | latest upstream |
| **v4.0 Build 8025 + WebM** (`8025_webm`) | the same | mods with `.webm` videos (intros, logos, animated menus), e.g. *Streets of Rage X* (see below) |

`8025_webm` is build 8025 compiled with WebM video playback (VP8 video +
Vorbis audio, as upstream's WebM-enabled builds) and with **SAB**, native
script extensions for script-driven mods (`sab_*` functions: profiler,
navigation map, camera override) plus an O(1) script `free()`; existing mods
call no `sab_*` function and behave as on stock 8025 — see
[`engines/8025/sab/README.md`](engines/8025/sab/README.md). Paks whose
scripts call `sab_*` functions are started with `8025_webm` automatically. It decodes videos in
background threads; it is picked automatically for build 8025 mods whose pak
contains `.webm` files, while the plain `8025` engine stays single-threaded.

OpenBOR versions are not fully compatible with each other: newer mods use
commands, script functions and constants that older engines reject, and older
mods can behave differently on newer engines. That is why players keep
several OpenBOR executables around; this core does the choosing for them.

## Downloads

Releases (GitHub *Releases* page of this repository) hold one zip per build,
each with this README, the licence and the portable build script:

| File | Contents |
|---|---|
| `openbor_libretro-linux-x86_64.zip` | the core for Linux x86_64 (+ `openbor_libretro.info`) |
| `openbor_libretro-linux-aarch64.zip` | the core for Linux aarch64 (ARM 64-bit boards and handhelds) |
| `openbor-standalone-windows-x86_64.zip` | `openbor.exe`: one file, SDL2 built in |
| `openbor-standalone-linux-x86_64.zip` | `openbor`: needs the system's SDL2 (`libsdl2-2.0-0`) |

## Usage as a libretro core

Copy `openbor_libretro.so` (and `openbor_libretro.info` into the frontend's
info folder) and load a mod's `.pak` file as content.

### How the engine is chosen

1. **Core option** `Engine version` (`auto`, `8025_webm`, `8025`, `7735`,
   `7555`, `6391`, `4153`, `3711`). It can be saved per game with RetroArch's
   per-game core options. A forced version is used as is.
2. **Engine tag in the file name**, when the option is `auto`: a pak whose
   name ends with `[ob<engine>]` before `.pak` is started with that engine,
   used as is. The tag is case-insensitive and may be preceded by spaces:

   ```
   SORX [ob8025_webm].pak
   MFF4ever [ob7555].pak
   SFFG[ob4153].pak
   ```

   A tagged pak is still found under its original name (`Paks/SORX.pak`)
   by mods that open their own pak to check it.
3. **Auto-detection** (default). The `.pak` is scanned before starting: the
   commands of `models.txt`, `levels.txt` and of every model and level file
   they reference, plus the scripts' builtin functions, `openborconstant()`
   names and the names passed to property/variant functions
   (`getentityproperty(e, "name")`, `openborvariant("name")`, ...), and the
   `video WxH` syntax. Each
   engine's vocabulary is extracted from its own sources
   (`tools/gen_detect_tables.py` → `common/detect_tables.h`). The engine
   with the fewest unknown items wins; on a tie the **oldest** one is chosen,
   as it is the closest to what the mod was written for. Build 8025 is
   started as `8025_webm` when the pak contains `.webm` files.
4. **Fallback.** The chosen engine loads the whole game (models, levels,
   scripts) inside `retro_load_game()`. If it stops with an error — an
   unknown command, an invalid setting — the next engine is tried (newer
   ones first, then older ones), and an on-screen message says which engine
   is in use.

The frontend log shows the decision, e.g.
`Auto-detected engine 6391 (10 items need a newer engine than 3711, e.g. ...)`.

### Data files

Each engine keeps its writable data under the frontend's **save directory**
(falling back to the system directory, then to the content directory; the
standalone program: next to the executable, or `--save-dir`), in a
folder per engine version because settings, high scores and saved games are
raw structures whose layout changes between versions (`8025` and `8025_webm`
share the `8025` folder):

```
<save dir>/OpenBOR/<engine>/Saves/        settings (<mod>.cfg), high scores, saved games
<save dir>/OpenBOR/<engine>/Logs/         OpenBorLog.txt, ScriptLog.txt
<save dir>/OpenBOR/<engine>/ScreenShots/
```

Check `Logs/OpenBorLog.txt` first when a mod does not start; engine errors are
also shown as an on-screen message.

### Controls (RetroPad, up to 4 players)

| RetroPad | OpenBOR |
|---|---|
| D-Pad | Move |
| Y | Attack 1 |
| B | Jump |
| A | Special |
| X | Attack 2 |
| L / R | Attack 3 / Attack 4 |
| Start | Start / pause |
| Select (player 1) | Esc (back / exit menus) |
| R3 | Screenshot |
| Left stick | Move (core option `Analog stick movement`) |

Buttons are remapped the standard way, in the frontend (RetroArch: *Quick
Menu → Controls*). The engine's own *Control Options* menu is hidden: every
player always uses the layout above (bindings saved in a mod's settings by
another OpenBOR build are replaced), so all four players work out of the box.
Mods that show button prompts (e.g. SoRX) show the layout above, as the
frontend does not tell the core about its remaps.

### Core options

| Option | Values | |
|---|---|---|
| Engine version | `auto`, `8025_webm` … `3711` | applies on the next load |
| Analog stick movement | enabled / disabled | left stick moves like the D-Pad |
| Rumble | enabled / disabled | build 6391 and newer |
| Gamma | `0` (off), `32` … `192`, `-32` … `-192` | OpenBOR's gamma curve, applied to the core's output |

The engine menus keep what only the game can do: volumes (master, effects,
music), BGM, brightness, the HUD *Window Offset* and the system options.
Entries that the frontend handles or that have no effect in the core are
hidden: *Control Options*, *Gamma*, and the sample rate / bit depth menu of
builds 3711–6391 (*Sound Options → Advanced Options*; the output is always
44.1 kHz / 16-bit and the frontend resamples it). Window size, filters,
scaling and frame rate limits are the frontend's.

## Usage as a standalone program

```
openbor [options] <game.pak>
  --engine <name>   auto (default) or a version of --list-engines
  --list-engines    print the engine versions built in
  --fullscreen      start in fullscreen (F11 / Alt+Enter toggle)
  --scale <n>       window size: n x the game's resolution (default 2)
  --gamma <n>       OpenBOR gamma, -255 ... 255 (default 0)
  --no-analog       the left stick does not move like the D-pad
  --no-rumble       no controller rumble
  --save-dir <dir>  where OpenBOR/ is written (default: next to the program)
```

Examples:

```sh
openbor "Paks/SORX [ob8025_webm].pak"          # engine from the file-name tag
openbor --engine 6391 --fullscreen MyMod.pak   # force build 6391
openbor.exe --list-engines
```

`--engine` is the core's *Engine version* option: `auto` picks the engine
exactly as the core does (file-name tag, auto-detection, fallback); a forced
version is used as is. The other options are the core options. The program
is the core linked with `standalone/main.c`, which plays the frontend's part
(window, audio, input), so a game runs as in RetroArch.

Keyboard (player 1; the keys of RetroArch's default layout, then the
RetroPad mapping above):

| Key | RetroPad | OpenBOR |
|---|---|---|
| Arrows | D-Pad | Move |
| A | Y | Attack 1 |
| Z | B | Jump |
| X | A | Special |
| S | X | Attack 2 |
| Q / W | L / R | Attack 3 / Attack 4 |
| Enter | Start | Start / pause |
| Right Shift | Select | Esc (back / exit menus) |
| F11, Alt+Enter | | fullscreen on / off |
| Esc | | quit the program |

Game controllers (any controller SDL2 knows: Xbox, PlayStation, Switch Pro
and most others) are players 1–4 in connection order, with the RetroPad
layout by button position (bottom = B, right = A, left = Y, top = X,
shoulders = L / R, triggers = L2 / R2) and rumble. The keyboard drives
player 1 as well. The window keeps the game's aspect ratio (black borders)
and scales it with nearest-neighbour filtering.

The console window of `openbor.exe` shows the engine choice and the errors;
the engine's own log is in `OpenBOR/<engine>/Logs/` next to the program.

## Streets of Rage X

SoRX ships with its own engine (a fork of upstream OpenBOR from mid 2026)
and a pak whose 4-byte magic was changed from `PACK` to `1D 11 14 7F` so
that stock engines refuse it; the contents themselves are plain. Every
engine of the core that checks the magic (3711, 4153, 6391, 8025 and
8025_webm; 7555 and 7735 do not check it) and the engine detection accept
both magics, so the pak loads as shipped. To play it with the core or the
program:

1. Make the pak renamable: `python3 tools/sorx_renamable.py SORX.pak
   "SORX [ob8025_webm].pak"`. A startup script (`loadPak` in
   `data/scripts/loading.c`) opens `Paks/SORX.pak` or `Paks/1.0.0.pak` and
   shuts the game down when it finds neither; the tool removes that check
   (and writes the standard `PACK` magic), so the pak can have any name and
   live anywhere. Its other fixed names (`Saves/SORX.set`, `Saves/SORX.sco`)
   are the game's own data files and do not depend on the pak's name.
2. Without the tool, keep the game's layout and load `Paks/SORX.pak`.
   `python3 tools/pak_fix_magic.py SORX.pak --in-place` only restores the
   standard header, for other OpenBOR builds.

The logos, the intro and the animated title screen are WebM videos, so the
game runs on `8025_webm` (chosen automatically; the converted pak can also be
named `SORX [ob8025_webm].pak`). On the plain `8025` engine the videos are
skipped and the title screen stays black — press Start to continue.

Both build 8025 engines also provide the two
script extensions of the SoRX engine that the game uses:
`getplayerproperty(p, "device_name")` (reported as `"RetroPad"`) and
`getplayerproperty(p, "button_<action>")` (the bound button, with Xbox-style
names so the game's on-screen prompts pick the right icons).

Loose files that scripts open with relative paths (`Paks/...`, `Saves/...`)
are resolved against the game folder — the folder containing `Paks/`, or the
pak's own folder — like the original executable's working directory.

## Building

Everything the engines need is bundled in `deps/` (zlib, libpng, libogg,
libvorbis, libvpx, libretro-common/libco), so the core only needs a C
compiler, `make` and binutils (`ld -r`, `objcopy`). The standalone program
also needs SDL2 (development package, `sdl2-config`).

```sh
make                          # core: Linux/BSD -> openbor_libretro.so, macOS -> .dylib
make platform=win             # core: MinGW     -> openbor_libretro.dll
make standalone               # program: openbor (SDL2 from sdl2-config)
make ENGINES="6391 7555"      # choose the engine versions to include
make DEBUG=1                  # -O0 -g
make SYSTEM_ZLIB=1 SYSTEM_LIBPNG=1 SYSTEM_VORBIS=1   # use system libraries
make ENGINES="6391 8025"      # without 8025_webm: no threads, no libvpx
make SDL2_CONFIG=/path/to/sdl2-config standalone    # another SDL2
```

The `8025_webm` engine additionally builds the VP8 decoder of libvpx 1.15.2
from `deps/libvpx` (decoder only, portable C, no assembler needed) and links
pthreads (native Win32 threads on Windows). The bundled libpng is built
without its SIMD code (NEON, MSA, VSX), so every architecture builds the
same way. Objects go to `obj/<platform>/`, one subdirectory per engine; the
core and the program share them.

`build.sh` is the portable build script (the one shipped in the releases):

```sh
./build.sh                        # the core, native (Linux, macOS, MSYS2/MinGW)
./build.sh win                    # the core for Windows, MinGW cross build
./build.sh standalone             # the program, native (needs SDL2)
./build.sh win-standalone [dir]   # openbor.exe, MinGW cross build: SDL2's MinGW
                                  # development package (dir, or downloaded into
                                  # deps/sdl2-mingw) linked statically: one .exe,
                                  # a console program depending on system DLLs only
./build.sh test some.pak          # the core + a headless smoke test
```

Cross builds: `make CC=aarch64-linux-gnu-gcc OBJCOPY=aarch64-linux-gnu-objcopy`
builds the core for Linux aarch64; `CC` / `OBJCOPY` are taken from the
environment by `build.sh` too (e.g. `CC=x86_64-w64-mingw32-gcc`).

### Release pipeline

`.github/workflows/release.yml` runs only when started by hand (*Actions →
Release → Run workflow*, on the chosen branch, with an optional version name;
empty means `<date>-<commit>`, e.g. `2026.11.10-205bfcb`). It builds, in
parallel:

* the core for Linux x86_64 (Ubuntu 22.04, so it runs on glibc 2.35 and
  newer) and for Linux aarch64 (on an ARM runner);
* `openbor.exe` for Windows x86_64 (MinGW cross build, SDL2 static);
* `openbor` for Linux x86_64;

and publishes them as a GitHub release named after the version (the zips
of *Downloads* above).

Build numbers are upstream commit counts (`git rev-list --count`), which is
how each engine's commit was identified. Note that the upstream tag `v4153`
is misnamed: it points to build 4504.

## How it works

### Several engines in one binary

Every engine version lives in `engines/<build>/` (the unmodified upstream
`engine/` directory, imported in its own commit, plus a `libretro/` port
layer). Each one is compiled with its own include paths and flags, partially
linked into a single object (`ld -r`), and then every global symbol except its
`ob_engine_<build>()` accessor is made local (`objcopy --keep-global-symbol`,
`ld -exported_symbol` on macOS). The thousands of identically named globals
of the different versions therefore never clash, and each engine keeps its
own state — which is also what makes the in-process fallback possible.

The version-independent libretro layer (`common/libretro.c`) talks to the
engines through a small interface (`common/ob_api.h`).

### Running a blocking engine under libretro

OpenBOR is a blocking program: `openborMain()` never returns and presents
frames from deep inside its own loops (menus, scenes, fades, loading screens).
The engine runs on a **libco cooperative thread**:

* `video_copy_screen()` converts the engine's screen (8-bit paletted, 16 or
  32-bit, depending on the version and mod) to XRGB8888 and switches back to
  `retro_run()`, so each `retro_run()` produces exactly one engine frame.
  Everything runs on the frontend's thread — no locks, no audio thread
  (except for WebM playback, below).
* The engine clock is **virtual**: it advances by one 60 Hz frame per
  `retro_run()`, so game speed follows the frontend (fast-forward,
  slow-motion, frame advance). Loops that wait on the clock without drawing
  hand a frame back to the frontend instead of spinning.
* Audio: `retro_run()` calls the engine mixer for one frame's worth of
  44.1 kHz stereo samples (735 per frame).
* The mod's real resolution is reported from `retro_get_system_av_info()`;
  later changes use `SET_GEOMETRY` / `SET_SYSTEM_AV_INFO`.
* WebM playback (`8025_webm`): upstream's decoder runs unchanged
  in its own threads (demux, video, audio). The port supplies the thread
  primitives (`libretro/threads.c`), a real audio lock shared by the decoder
  threads and the mixer called from `retro_run()`, and the video output:
  each decoded frame is converted from YUV and scaled to the game's
  resolution, keeping the video's aspect ratio, and the playback loop
  presents one frame per `retro_run()`. Video timing follows the same virtual
  clock as the game.
* Closing content runs the engine's normal shutdown (settings saved, all
  resources freed); quitting from the engine menu asks the frontend to close
  the core.

Changes to upstream engine files are kept minimal and guarded by
`#ifdef LIBRETRO` (`git diff` against each engine's import commit shows all of
them): log/save paths, the loading-screen refresh clock, forced 16-bit/44.1 kHz
output where the engine still allowed other rates, the rumble entry in the
control menu, the shutdown/error hooks, and — for build 3711 — larger save
path buffers (builds 3711 and 4153; they were 128 bytes) and `exit()`
replaced by the port's exit (3711). Menu entries hidden in the core (see
*Core options*) and `apply_controls()` forcing the RetroPad layout. For WebM
(8025_webm): a libretro branch in
`threads.h` and the `playwebm()` loops yielding to the frontend instead of
sleeping.
Two upstream memory bugs found with valgrind are fixed the same way: build
6391 allocated the per-frame collision box lists for one entry but filled
`max_collisons` (2) of them, and build 3711 iterated one element past the
damage-over-time arrays.

## Limitations

* No save states and no reset (OpenBOR's global state cannot be reinitialised
  in place). Each engine can be started once per core load; to load another
  game, close the content (RetroArch reloads the core).
* The standalone program runs one game per start (the same reason) and has
  no in-program menu: options are command-line arguments. Its frame pacing
  follows the core's 60 Hz clock (with vsync off); the keyboard layout is
  fixed (the engine's control menu is hidden, as in the core).
* WebM videos play only on `8025_webm` (the other engines are built without
  WebM, so their videos are skipped, as on upstream builds without WebM;
  animated GIF scenes work everywhere).
  Videos are shown at the game's resolution (a 1080p video in a 480×272
  game is scaled down to 480×272). The VP8 decoder is portable C without
  SIMD: 1080p videos need a reasonably fast CPU, and when the frontend runs
  faster than real time (fast-forward) video frames are dropped.
* Engine 3711 only accepts the numeric `video N` modes of its time.
* When an engine fails while loading and the next one is tried, the failed
  engine cannot free what it had loaded (upstream only frees after a
  complete startup): that memory (typically a few MB to tens of MB) stays
  allocated until the content is closed.
* Auto-detection reads vocabulary, not behaviour: a mod that loads on an
  older engine but relies on a newer engine's behaviour may need the
  `Engine version` option.

## Testing without a frontend

* `test/frontend.c` — minimal headless libretro frontend (scripted pad input,
  frame dumps to PPM, audio to WAV; core options from environment variables,
  e.g. `OPENBOR_ENGINE=3711`; `REALTIME=1` paces frames at 60 Hz like a real
  frontend, needed to see WebM videos; `VIDEO=out.mp4` pipes every frame to
  ffmpeg).
* `test/make_test_mod.py` — generates a tiny synthetic mod (fonts,
  backgrounds, sounds, Vorbis music; `--video-mode N` for 3.x engines).
* `test/detect_cli.c` — prints the engine auto-detection would pick
  (`OB_DETECT_DEBUG=1` lists every item that differs between engines).

```sh
python3 test/make_test_mod.py /tmp/mod      # needs Pillow and ffmpeg
(cd /tmp/mod && borpak -b -d . ../test.pak) # borpak: upstream tools/borpak
./build.sh test /tmp/test.pak
```

## License

OpenBOR is distributed under the BSD-style license in [LICENSE](LICENSE)
(the upstream readme is kept as [README-OpenBOR.md](README-OpenBOR.md)).
Bundled dependencies keep their own licenses (see each directory in `deps/`).
