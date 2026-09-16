# ArtMiner

ArtMiner is a standalone Windows-native procedural-art laboratory for exploring, searching, reproducing, and exporting deterministic visual systems.

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

AM-001 established the native deterministic foundation and AM-002 established the versioned `.amr` recipe/typed-graph substrate. AM-003 adds the first canonical CPU/reference renderer and makes ArtMiner useful headlessly for deterministic static procedural art.

The AM-003 reference node set covers normalized coordinates, radial/angular fields, deterministic value/gradient/Worley/fBm noise, scalar domain warp, basic SDF composition, threshold/quantisation, repeat/symmetry transforms, palette mapping, channel/luminance extraction, ordered dithering, and final RGBA8 image composition. PNG export uses Windows Imaging Component and writes a deterministic adjacent provenance sidecar containing the complete canonical recipe and semantic fingerprint.

Read [`RAG.md`](RAG.md) for the authoritative architecture, product contract, reviewed implementation plan, and milestone sequence. Read [`AGENTS.md`](AGENTS.md) before autonomous implementation work. The `.amr` grammar and compatibility rules are documented in [`docs/recipe-format.md`](docs/recipe-format.md); canonical raster/evaluator semantics are documented in [`docs/static-evaluator.md`](docs/static-evaluator.md).

## Target stack

- C++20
- Windows 10/11 x64
- Win32
- Direct3D 11 / DXGI (introduced by AM-004)
- Direct2D / DirectWrite where useful for native UI
- Windows Imaging Component
- CMake for developer/CI builds

The shipped application must have no third-party runtime installation requirement. The native targets use the Windows SDK and statically link the MSVC C/C++ runtime.

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

Useful command-line paths:

```bat
build\Release\ArtMiner.exe --version
build\Release\ArtMiner.exe --help
build\Release\ArtMiner.exe --check-workspace
build\Release\ArtMiner.exe --workspace D:\ArtMinerWorkspace --check-workspace
build\Release\ArtMiner.exe recipe validate examples\am002-minimal.amr
build\Release\ArtMiner.exe recipe inspect examples\am002-minimal.amr
build\Release\ArtMiner.exe render examples\am003-fbm-warp.amr output\fbm-warp.png
```

Recipe and render commands are headless and do not require the GUI or a workspace. `recipe validate` prints the semantic fingerprint; `recipe inspect` also prints schema/evaluator version, seed, render settings, and graph counts. `render` evaluates output `main` through the canonical CPU path, writes the requested PNG, prints an RGBA8 image hash, and writes `<output>.artminer.txt` provenance beside the image.

Additional AM-003 example families are available under `examples/am003-worley.amr`, `examples/am003-sdf.amr`, and `examples/am003-angular-repeat.amr`.

With no command-line action, ArtMiner validates its workspace and opens the minimal native Win32 shell. By default the workspace root is the directory containing `ArtMiner.exe`; an explicit `--workspace` path replaces it. ArtMiner does not silently fall back to the registry or an unrelated profile directory if that location is unwritable.

The portable workspace contains/creates:

```text
<workspace>/
├── recipes/
├── palettes/
├── output/
└── cache/
```

## Recipe and graph baseline

Schema/evaluator version 1 currently fixes these semantic contracts:

- stable node type IDs plus per-node semantic versions;
- typed graph data kinds: `ScalarField`, `VectorField`, `ColourField`, `Mask`, `ParticleSet`, `Palette`, and `Image`;
- node-owned port, parameter-domain, mutation, state-classification, and evaluator-capability metadata;
- all parameters explicit in a recipe rather than silently inheriting defaults;
- ordinary graph cycles rejected; an explicit state-boundary class remains reserved for later fixed-tick feedback work;
- canonical ordering independent of source node/edge/output order;
- a 128-bit textual semantic fingerprint made from two domain-separated FNV-1a 64-bit hashes of canonical semantic recipe text;
- `meta` records explicitly non-semantic and excluded from the semantic fingerprint;
- canonical static images are tightly packed row-major RGBA8 with straight alpha; pixel/raster and floating semantics are specified in `docs/static-evaluator.md`.

The recipe fingerprint and image hash are deterministic identity/checking mechanisms, not cryptographic integrity primitives. Any future change that alters established recipe/evaluator meaning must use schema/evaluator/node versioning rather than silently changing existing semantics.

## Determinism baseline

AM-001 fixed the low-level deterministic contracts and protects them with committed reference vectors:

- SplitMix64 as the seed-mixing transform;
- `derive_seed(root_seed, domain) = splitmix64(root_seed ^ splitmix64(domain))`;
- PCG32 (XSH-RR 64/32) for model-owned random streams;
- FNV-1a 64-bit as the initial stable non-cryptographic hashing primitive.

AM-003 static noise nodes derive local seeds from the recipe root seed and stable node identity instead of consuming shared RNG state. Canonical evaluation therefore does not depend on graph traversal order, worker scheduling, wall-clock time, locale, or pointer identity.
