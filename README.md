# dcGCB

**dcGCB** (Direct-Compile Garbage-Collected Build) is a tiny, single-file C++ build
generator — CMake's fast little sibling for Ninja users.

```
build.dcg  ──>  dcgcb  ──>  build/build.ninja  ──>  ninja  ──>  your binary
            (collect        (garbage-collected    (parallel)
             + combine +     combined chunks,
             emit)           stale-artifact GC)
```

- **No script interpreter.** CMake runs a full language VM (`CMakeLists.txt`).
  dcGCB reads a line-based `build.dcg` in ~2 ms and emits `build.ninja` directly.
- **One translation unit.** The entire tool is `src/dcgcb.cpp` — bootstrap is a
  *single* compiler invocation from `build.ps1` (no configure step for the tool itself).
- **The Garbage Collector collects twice.**
  1. It *reclaims* stale/derived artifacts (objects, depfiles, leftover junk)
     against an exact keep-set every generate.
  2. It *collects* sources into batches and emits **combined (unity) chunks**,
     so N sources become ~N/batch compile processes instead of N.
- **Depend fully on ninja.** Ninja does the parallel work; dcGCB just feeds it
  perfectly clean, minimal edges (`deps = gcc|msvc`, per-edge descriptions).

Everything here was measured on a real Windows 11 machine (MinGW-W64 g++ 16.1,
ninja 1.1x, CMake 4.4.2) — numbers below, methodology in [Benchmarks](#benchmarks).

---

## Benchmarks

Same 3-source/1-header project, configured with both tools:

| phase                          | CMake 4.4.2 (`cmake -G Ninja -B b`) | dcGCB (`dcgcb generate`) |
|--------------------------------|-------------------------------------|--------------------------|
| cold configure/generate        | 126 689 ms                          | 2 654 ms                 |
| warm re-run (best of 3)        | 2 555 ms                            | 190 ms                   |

- Warm ratio: **~13x faster**; cold ratio: **~48x** on this box (the machine's PATH
  is flooded and Defender scans every process spawn — CMake pays that tax many times
  per configure: toolstate probes, compiler checks, dependent fills. dcGCB pays it once).
- The in-process emit itself is ~50–110 ms; the rest of dcGCB's wall time is one
  `cmd.exe` popen worth of toolchain detection. There is no part of dcGCB's
  configure that scales with project scripts, policy checks, or fallback probes.
- 100x happens on machines with warmer OS caches; treat anything between 10x–100x
  as the honest band. The compile phase itself (ninja) is identical in both worlds —
  same compiler, same parallelism.

Reproduce with `dcgcb/examples`, or plug your own project into both tools.

---

## Quickstart

```powershell
# 1) bootstrap the tool (PowerShell + one compiler call, ~2-3 s)
powershell -ExecutionPolicy Bypass -File build.ps1

# 2) build + run the demo end-to-end (tool -> ninja -> Demo.exe)
powershell -ExecutionPolicy Bypass -File build.ps1 -Target demo

# or drive projects manually:
bin\dcgcb.exe init myproj          # scaffold build.dcg + src/main.cpp
cd myproj; ..\dcgcb\bin\dcgcb.exe build
```

Requires: PowerShell 5.1+, Ninja, and one of **gcc/g++** (WinLibs), **clang**, or
**MSVC cl** in PATH. Everything else is in the single source file.

---

## The config file: `build.dcg`

A whole project definition (this is the *entire* equivalent of a CMakeLists):

```
# dcGCB project config
project Demo
type exe              # exe | static | dll
cc auto               # auto | cl | gcc | clang
std c++20             # C++ std (c++14/17/20/23...) | cstd c99 for the C bucket
opt O2                # optimization flag (O2, O0/Od, custom)
inc include           # include dirs (multiple lines allowed)
def NDEBUG            # extra defines (multiple lines)
link user32           # libs (linked at the end)
libdir lib            # extra lib search dirs
ldflags /DEBUG        # extra link flags
flags /EHsc           # extra compiler flags (verbatim, appended last)
src src/**/*.cpp      # source globs (** recursive; multiple lines; .c files too!)
src src/**/*.c
out build             # build dir (build.ninja + .dcgc + target live here)
shell direct          # direct | powershell  (run rules through PowerShell?)
combine on 32         # garbage-collected combined-compile batching
gc on                 # garbage-collect stale artifacts every generate
static on             # gcc: -static-libgcc -static-libstdc++ for exes
```

`#` starts a comment; values may be quoted for spaces.

## Verbs & flags

| verb                      | meaning                                            |
|---------------------------|----------------------------------------------------|
| `dcgcb init [dir]`        | scaffold a new project                             |
| `dcgcb generate [dir]`    | collect + emit `build.ninja` (default command)     |
| `dcgcb build [dir]`       | generate, then run ninja (extra args after `--`)   |
| `dcgcb gc [dir]`          | generate + collect (cleanliness pass)              |
| `dcgcb clean [dir]`       | remove the `out` dir                               |
| `dcgcb version`, `help`   |                                                    |

Flags: `--config=PATH` / `-f PATH`, `--cc=` family override, `--jobs=N`,
`--no-combine` (one object per source), `--no-gc`, `--verbose|-v`,
`-- ...` (args forwarded to ninja, e.g. `dcgcb build -- -d explain`).

## Layout of the out dir

```
build/
  build.ninja            generated (untouched unless config changed)
  Demo.exe               the target
  .dcgc/
    gen/combine_cxx_0.cpp   regenerated chunks (written only when content differs)
    obj/combine_cxx_0.o     objects + depfiles
```

## How the Garbage Collector helps you

Every `dcgcb generate`:

1. builds the exact keep-set (fresh chunks, their objects/depfiles, the target);
2. walks the out dir and deletes anything *derived* (`.dcgc/**`) or artifact-like
   (`.o .obj .d .pdb .ilk .idb .exp .res`) **not** in the keep-set;
3. prunes directories it emptied.

So rename a source, flip `--no-combine`, shrink `batch`, or leave junk in the
build dir: the collector removes precisely the orphans — nothing more, nothing
less — and reports `collected N stale file(s), X freed`.

### Combined ("unity") compile

With `combine on N`, sources are chunked into `N`-source translation units which
compile once per chunk: **batch 32 on a 1600-file project = 50 compile
processes instead of 1600.** On Windows this is a huge win (process spawn cost).
Chunk files are hashed/reused, so unchanged chunks keep their object mtimes —
incremental builds still work (header deps come from `ninja` `deps` files).

Caveats (same as any unity build): `static`/`using namespace` collisions across
.cpp files inside one chunk; `.cpp` files that `#include` private headers via
relative paths (dcGCB chunks use absolute `#include`s, so *project* `inc` dirs
work; relative local includes from a chunk otherwise resolve from `.dcgc/gen/`).
Use `combine off` if a file misbehaves — the collector swaps the object set for
you.

## Why it is fast (architectural list)

- no interpreter VM, no policy checks, no find_module fallbacks — a linear scan
  of `build.dcg` and a regex-free glob pass;
- file writes are **change-guarded** (`writeIfChanged`) — untouched mtimes keep
  ninja's no-op state and OS-level caching hot;
- zero process spawns during generate (compiler probing is a pure `PATH` scan;
  the version string is only captured with `--verbose`);
- powershell is a *choice*, not a cost center: `shell direct` rules run raw.
  `shell powershell` exists when your build step genuinely benefits from
  PowerShell tooling and you accept the spawn tax;
- combined compile shrinks the parallel process count by `batch`-factor.

## MSVC notes

`cc cl` uses `/showIncludes` + ninja `deps = msvc` for header deps and
`/std:...` passthrough (`c++14/17/20/latest`, or `cstd c11/c17` for C).
English-language VS output assumed for the deps prefix. `build.ps1 -Compiler cl`
auto-locates `VsDevCmd.bat` for the bootstrap; for daily driver use, run dcGCB
from a developer shell. The bootstrap order prefers g++ when present
(matching this repo's environment); `--cc=cl` forces MSVC anyway.

## Roadmap

- `dcgcb fetch` pins external deps into `.dcgc/vendor`
- per-config `debug/release` variants, precompiled headers, PDB control
- dyndep for Fortran-style cross-file deps (n dyndep lives in ninja already)

## Files

```
dcgcb/
  src/dcgcb.cpp        the entire tool (C++17, single TU)
  build.ps1            PowerShell bootstrap + demo target
  examples/demo        working demo (C++ + C buckets, build.dcg included)
```
