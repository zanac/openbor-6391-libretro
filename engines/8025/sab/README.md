# SAB: native script extensions (engine `8025_webm`)

SAB adds C functions that OpenBOR scripts can call, for script-driven mods
whose per-frame work is too heavy for the interpreter. It was written for the
*Slaps And Beans* conversion (zanac/S-B) but every function is generic.

* Built only into the **`8025_webm`** engine (`-DSAB` in
  `libretro/engine.mk`); plain `8025` and the older engines are unchanged.
* Every new builtin is named `sab_*`: existing mods (e.g. *Streets of Rage X*,
  which uses `8025_webm` for its videos) call none of them, so they behave as
  before.
* The core's auto-detection picks `8025_webm` for a pak whose scripts call
  `sab_*` functions (`common/detect.c`); a `[ob8025_webm]` file-name tag
  works too.

Sources: `sab.c` (included at the end of `openborscript.c`), `sab_prof.h`
(interpreter hooks), `sab_heap.h` (script heap bookkeeping). Hooks in stock
files are all inside `#ifdef SAB`: `openborscript.c` (include, `free()`,
`array()` and the other heap allocations), `source/openborscript/index.c`
(registration), `source/scriptlib/Interpreter.c` (profiler),
`source/scriptlib/ImportCache.c` (function names for the profiler),
`openbor.c` (camera, after `update_scroller()`).

## Engine change: O(1) `free()`

Stock 8025 finds a freed allocation with a linear scan of the script heap
list and removes it from the list's string-hash bucket, which every array
shares (name `"openbor_array"`): both cost O(live allocations). In a mod
with a few thousand live arrays `free()` took ~110 µs per call and two
thirds of the script time. With SAB, arrays are stored unnamed and a pointer
hash table finds the list node directly. Allocation, order and the shutdown
dump of leaked variants are unchanged.

Measured on the Slaps And Beans Western level (9000 frames, headless):
146 s → 44 s.

## Functions

### Profiler

| Function | Description |
| --- | --- |
| `sab_profile(on)` | `1`: reset the counters and start; `0`: stop |
| `sab_profile_dump(n)` | print to the log the `n` functions (script or native) with the most self time: self ms, total ms, calls, µs per call |

While the profiler is on, a native function that fails (the interpreter's
"Script function 'x' returned an exception") also prints the script call
stack, outermost first (`script stack (sab_profile): a > b > c`).

Script functions are named from the import cache (`script@<address>` for
functions of the calling script itself).

### Navigation map

A free walkable area as z intervals per x column, with a floor height per
interval (linear: slopes, stairs) and named patches that remove parts of it
(obstacles that appear or go away). File format:

```
grid <x0> <z0> <cell> <columns>
col <index> <n> <z0> <z1> <y0> <y1> [...]      n intervals of column index
patch <name> <box|disc> <cx> <cz> <a> <b> <cos> <sin> <ylo> <yhi> <on>
end
```

Column `i` covers `x0 + i·cell … x0 + (i+1)·cell`; an interval is walkable
from `z0` to `z1` (cell centres ± half a cell), floor `y0 → y1`. A box patch
covers `|u| ≤ a, |w| ≤ b` in the frame rotated by `(cos, sin)` about Y, a
disc patch the radius `a`; either removes the floor points whose height is
in `[ylo, yhi]` while it is on. Units are whatever the mod uses.

| Function | Description |
| --- | --- |
| `sab_nav_load(path)` | load a map from the pak; returns a handle (the same one for the same path), 0 on error |
| `sab_nav_free(h)` | forget a map (patches back to their file state on the next load) |
| `sab_nav_sample(h, x, y, z, maxd, climb, out)` | nearest walkable point within `maxd`; distance = plan distance plus the height difference beyond `climb`; writes `out[0..2]` = x, y, z; returns 1 or 0 |
| `sab_nav_floor(h, x, y, z)` | floor height under (x, z): the highest at most 1 above `y`; `y` when there is none |
| `sab_nav_raycast(h, ax, az, bx, bz, y, step, out)` | march from a to b in `step` units; returns 1 when the way is blocked; `out[0..1]` = last walkable point (b when clear) |
| `sab_nav_patch(h, name, on)` | switch every patch of that name; returns 1 if found |

### Camera

| Function | Description |
| --- | --- |
| `sab_camera(on, x, y)` | `1`: the engine's scroll position is set to (x, y) pixels every frame, after the engine's own scrolling; the level position follows x, so `at` spawns still work; `0`: back to the engine's scrolling |

### Post effects

A colour grade and a bloom applied to the 32-bit screen of a running level
(not during the pause menu). The sprite queue is drawn in two parts: what
lies below the split z is drawn, graded, then the rest (HUD, script texts)
is drawn on top unchanged. Nothing happens until a script enables it, so
other mods are not affected.

| Function | Description |
| --- | --- |
| `sab_postfx_load(lut, curves)` | `lut`: a file of N³ RGB bytes (any N up to 256), node (r, g, b) at byte `((b·N + g)·N + r)·3`, looked up trilinearly before the bloom (results cached per colour); `curves`: an optional file of 3 × 256 bytes (R, G, B tables) applied after the bloom, `""` for none. Returns 1 on success |
| `sab_postfx(on, curves)` | enable / disable; `curves` 0 skips the tables (e.g. a scene where the game disables its curves) |
| `sab_postfx_bloom(intensity, threshold)` | Unity Standard Assets `Bloom` (quality High, 2 blur iterations, spread 2.5, Add): 4×4 max to a quarter-size image, [1, 2, 1] tent, `max(c − threshold, 0)`, two cumulative 9-tap blurs combined with max, tent, two bilinear 2× upsamples, `+ intensity ·` the result. Intensity 0 turns it off |
| `sab_postfx_zsplit(z)` | sprite-queue z where the grading stops (default `HUD_Z`) |

Cost: about 3.5 ms per 640×360 frame on the build machine (one thread).
The Slaps And Beans mod uses it for the Western camera chain (its
`docs/postfx.md`); against that document's exact chain the engine output
differs by 0.5 levels on average (max 6).
