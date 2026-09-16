# ArtMiner

ArtMiner 1.0 is a standalone Windows-native procedural-art laboratory for exploring, mutating, breeding, searching, reproducing, inspecting, and exporting deterministic visual systems.

It is intentionally self-contained: C++20, Win32, D3D11/DXGI, Windows Imaging Component, CMake, and no third-party runtime installation. Canonical recipe meaning and canonical pixels come from the deterministic CPU evaluator; GPU preview is an explicitly noncanonical interactive acceleration/fallback surface.

## v1 capability map

AM-001 through AM-015 are implemented. The delivered v1 includes:

- versioned `.amr` recipes, typed graphs, explicit semantic versions, deterministic fingerprints, and model-owned seeded PRNG streams;
- canonical static procedural fields/noise/SDF/palette/dither/image rendering plus D3D11 interactive preview with honest CPU fallback;
- a native deterministic 4×4 specimen browser with parameter mutation, seed-only variants, locks, editing, history, favourites, and progressive bounded thumbnail workers;
- fixed-tick reaction-diffusion, cellular automata, walkers/deposition, stable-ID branching, particles, explicit feedback/history and palette cycling;
- deterministic two-parent breeding, durable lineage records, ancestry browsing and semantic/provenance recipe diffing;
- transactional PNG/BMP/raw RGBA, frame sequences, sprite sheets/atlases, palettes/CSV/conservative `.cube` LUT export with deterministic provenance;
- bounded Quarry batch jobs, metrics, checkpoint/resume, disposable caches, dedupe, clustering, unusual-distance ranking and neighbourhood exploration;
- structural glyph/ANSI synthesis and exact terminal-text export;
- seamless material diagnostics, height/mask/normal workflows and validated analytic loops;
- deterministic type-safe topology mutation with structural locks, provenance and bounded topology search;
- release hardening: bounded UTF-8/local-input handling, graph/resource limits, session recovery, coherent 1.0.0 metadata, representative benchmarks, clean portable packaging and packaged smoke tests.

Read [`RAG.md`](RAG.md) for the product/architecture contract and [`AGENTS.md`](AGENTS.md) before autonomous implementation work. The AM-015 release audit, limits, recovery semantics, performance methodology and packaging contract are in [`docs/release-readiness.md`](docs/release-readiness.md).

## Build, test, run

Developer prerequisites are Windows 10/11 x64, Visual Studio 2022 or Build Tools with Desktop C++/Windows SDK, and CMake 3.24+ on `PATH`.

```bat
scripts\0Build.cmd
scripts\0Test.cmd
scripts\0Run.cmd
```

Equivalent manual commands:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The Release target statically links the MSVC runtime. No project package manager or third-party runtime install is required.

## Portable release package

After a successful Release build:

```bat
scripts\0Package.cmd
```

This stages and smoke-tests `dist\ArtMiner-1.0.0-win-x64\`, then creates `dist\ArtMiner-1.0.0-win-x64.zip`. The script tests the exact staged executable before archiving. CI runs the same package path and publishes the ZIP as a workflow artifact.

The project licence remains an explicit unresolved repository decision; AM-015 does not invent one.

## Core commands

```bat
build\Release\ArtMiner.exe --version
build\Release\ArtMiner.exe --help
build\Release\ArtMiner.exe --check-workspace
build\Release\ArtMiner.exe --workspace D:\ArtMinerWorkspace --check-workspace
build\Release\ArtMiner.exe --open examples\am006-reaction-diffusion.amr
build\Release\ArtMiner.exe recipe validate examples\am002-minimal.amr
build\Release\ArtMiner.exe recipe inspect examples\am007-feedback-trails.amr
build\Release\ArtMiner.exe render examples\am003-fbm-warp.amr output\fbm-warp.png
build\Release\ArtMiner.exe render-tick examples\am007-particle-flow.amr 120 output\flow-120.png
build\Release\ArtMiner.exe render-range examples\am007-particle-flow.amr 0 120 output\flow-frames
build\Release\ArtMiner.exe animate examples\am007-feedback-trails.amr
build\Release\ArtMiner.exe lineage
build\Release\ArtMiner.exe export ui examples\am003-fbm-warp.amr
build\Release\ArtMiner.exe material seam examples\am013-seamless-texture.amr main 0.000001
build\Release\ArtMiner.exe material loop examples\am013-validated-loop.amr 32 main 0.000001
```

`--version` reports the product version and the coherent recipe/evaluator/exporter/mutation/Quarry semantic-version matrix. Headless recipe/render/export/material/Quarry commands use the same validated recipe semantics as the UI.

## Native browser

With no headless command, ArtMiner opens the 4×4 specimen browser. Use **File → Open Recipe…**, `Ctrl+O`, or `--open <file.amr>` to establish a parent. `M` generates deterministic parameter mutations, `N` seed-only variants, arrows select a slot, `Enter` adopts it, `F` toggles favourite status, `Alt+Left/Right` navigates history, `Ctrl+S` saves a copy, and `Ctrl+Shift+F` advances through persisted favourites.

The parameter panel provides validated edits plus individual/logical-group locks. Growth recipes expose their explicit fixed tick as ordinary semantic state. Animation playback uses a dedicated worker: requested integer tick is authoritative; display refresh speed is not simulation time.

The selected specimen uses D3D11 preview where the graph is explicitly supported. Unsupported preview graphs fall back to the unchanged canonical CPU evaluator and upload that exact image for presentation; ArtMiner never silently changes graph meaning to keep a preview fast.

## Portable workspace and recovery

By default the workspace is the directory containing `ArtMiner.exe`; `--workspace` replaces it explicitly. ArtMiner does not silently redirect an unwritable workspace into the registry or user profile.

```text
<workspace>/
├── recipes/
│   ├── saved/
│   ├── favourites/
│   ├── recovery/current.amr
│   └── lineage/
│       ├── records/
│       └── specimens/
├── palettes/
├── output/
└── cache/
```

The selected browser recipe is atomically snapshotted to `recipes/recovery/current.amr`. On normal startup without `--open`, a valid recovery snapshot is preferred, then a persisted favourite. Corrupt recovery is reported and ignored rather than guessed. Favourites and lineage snapshots/records use atomic same-directory replacement. Cache content is disposable derived state and never authoritative.

## Release input/resource contract

Local recipe files are bounded and validated before allocation: maximum 8 MiB, maximum 64 KiB per source line, strict UTF-8, no raw NUL. Schema/evaluator/node versions are explicit and unsupported versions fail rather than being reinterpreted. v1 also imposes outer limits of 4,096 nodes, 65,536 parameters, 16,384 edges, 1,024 outputs and 4,096 metadata records; render dimensions remain checked and bounded.

Export stems are restricted portable tokens and cannot be path traversal. Export publication is transactional and existing completed destinations are not silently overwritten. Quarry checkpoints/caches remain bounded, identity/checksum validated, and cache corruption cannot become authoritative state. Full details are in `docs/release-readiness.md`.

## Determinism and versioning

Recipe schema/evaluator version 1 fixes stable node type IDs, typed data kinds, explicit parameter metadata, ordinary-cycle rejection, explicit previous-state boundaries, canonical ordering, deterministic recipe fingerprints and canonical RGBA8 output semantics. Changes that alter established meaning must advance an applicable schema/evaluator/node/operator version instead of silently changing v1 behaviour.

Static/random systems derive local streams from root seed plus stable identities rather than shared RNG traversal order. Fixed-tick growth/motion reconstructs tick `N` from deterministic state transitions. Breeding and topology mutation derive their choices from explicit parent identity, seed/operator versions, locks and canonical metadata. Worker ordering, wall-clock time, pointer identity, file serialization order and disposable caches are not allowed to change canonical results.

## Documentation

Key subsystem documents include:

- [`docs/recipe-format.md`](docs/recipe-format.md) — `.amr` grammar and compatibility;
- [`docs/static-evaluator.md`](docs/static-evaluator.md) — canonical raster/evaluator semantics;
- [`docs/gpu-preview.md`](docs/gpu-preview.md) — supported D3D11 preview and fallback;
- [`docs/specimen-browser.md`](docs/specimen-browser.md) — mutation/history/persistence;
- [`docs/growth-systems.md`](docs/growth-systems.md) — fixed-tick growth;
- [`docs/motion-feedback.md`](docs/motion-feedback.md) — particles, feedback, playback and snapshots;
- [`docs/breeding-lineage.md`](docs/breeding-lineage.md) — crossover, lineage and diff;
- [`docs/quarry.md`](docs/quarry.md) and related Quarry docs — deterministic batch/search semantics;
- [`docs/glyph-synthesis.md`](docs/glyph-synthesis.md) — glyph/ANSI mapping/export;
- [`docs/material-workflows.md`](docs/material-workflows.md) — material outputs, seams and loops;
- [`docs/topology-mutation.md`](docs/topology-mutation.md) — deterministic structural mutation;
- [`docs/release-readiness.md`](docs/release-readiness.md) — AM-015 v1 audit, hard limits, recovery, packaging and benchmark method.
