# ArtMiner

ArtMiner is a standalone Windows-native procedural-art laboratory for exploring, breeding, searching, reproducing, and exporting deterministic visual systems.

The project is intentionally dependency-light and self-contained. It is not tied to any game, editor, browser, cloud service, or AI model.

## Concept

ArtMiner treats procedural art as a space to **prospect**:

- generate related deterministic specimens;
- select interesting results;
- lock dimensions that are already right;
- mutate the rest;
- cross compatible parents;
- run large reproducible **Quarry** searches using transparent image/animation metrics;
- explore neighbourhoods around discoveries;
- export useful images, textures, sprite sheets, LUTs/palettes, masks, glyph/ANSI output, and compact recipes with provenance.

Every useful result should remain traceable to the recipe and deterministic state that created it.

## Current state

AM-001 establishes the native deterministic foundation: C++20 core/platform/app boundaries, a minimal Win32 shell, fixed deterministic RNG/hash contracts, checked allocation arithmetic, portable workspace handling, tests, and Windows CI. Later `AM-###` issues build the recipe, rendering, breeding, Quarry, and export systems on top of these contracts.

Read [`RAG.md`](RAG.md) for the authoritative architecture, product contract, reviewed implementation plan, and milestone sequence. Read [`AGENTS.md`](AGENTS.md) before autonomous implementation work.

## Target stack

- C++20
- Windows 10/11 x64
- Win32
- Direct3D 11 / DXGI (introduced by later milestones)
- Direct2D / DirectWrite where useful for native UI
- Windows Imaging Component
- CMake for developer/CI builds

The shipped application must have no third-party runtime installation requirement. AM-001 uses the Windows SDK and statically links the MSVC C/C++ runtime for the native targets.

## Developer prerequisites

For the baseline MSVC workflow:

- Windows 10/11 x64;
- Visual Studio 2022 or Visual Studio Build Tools with **Desktop development with C++** / the Windows SDK;
- CMake 3.24 or newer available on `PATH`.

No project package manager or runtime dependency install is required.

## Build, test, and run

From Explorer or a Windows terminal, the shortest paths are:

```bat
scripts\0Build.cmd
scripts\0Test.cmd
scripts\0Run.cmd
```

The scripts configure an x64 build under `build\`, compile the Release targets, run CTest, or launch `build\Release\ArtMiner.exe` respectively. Arguments supplied to `0Run.cmd` are forwarded to the executable.

Equivalent manual commands are:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Useful bootstrap command-line paths:

```bat
build\Release\ArtMiner.exe --version
build\Release\ArtMiner.exe --help
build\Release\ArtMiner.exe --check-workspace
build\Release\ArtMiner.exe --workspace D:\ArtMinerWorkspace --check-workspace
```

With no command-line action, the AM-001 executable validates its workspace and opens a minimal native Win32 window. By default the workspace root is the directory containing `ArtMiner.exe`; an explicit `--workspace` path replaces it. ArtMiner does not silently fall back to the registry or an unrelated profile directory if that location is unwritable.

The bootstrap workspace contains/creates:

```text
<workspace>/
├── recipes/
├── palettes/
├── output/
└── cache/
```

## Determinism baseline

AM-001 fixes the first low-level deterministic contracts and protects them with committed reference vectors:

- SplitMix64 as the seed-mixing transform;
- `derive_seed(root_seed, domain) = splitmix64(root_seed ^ splitmix64(domain))`;
- PCG32 (XSH-RR 64/32) for model-owned random streams;
- FNV-1a 64-bit as the initial stable non-cryptographic hashing primitive.

These are implementation semantics, not calls to process-global randomness. Future milestones may introduce stronger semantic fingerprints where required, but must not silently alter accepted deterministic output contracts.
