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

AM-001 established the native deterministic foundation, AM-002 established the versioned `.amr` recipe/typed-graph substrate, AM-003 added the canonical CPU/reference renderer and deterministic PNG/provenance export, AM-004 added the first interactive native D3D11/DXGI preview while keeping the AM-003 CPU evaluator normative, and AM-005 added the primary 4×4 specimen-browsing loop. AM-006 adds the first deterministic stateful growth families: reaction diffusion, configurable multi-state cellular automata, keyed walkers/deposition, and bounded stable-ID branching growth.

The AM-003 reference node set covers normalized coordinates, radial/angular fields, deterministic value/gradient/Worley/fBm noise, scalar domain warp, basic SDF composition, threshold/quantisation, repeat/symmetry transforms, palette mapping, channel/luminance extraction, ordered dithering, and final RGBA8 image composition. AM-006 growth nodes produce `ScalarField` values that compose with the same downstream palette/transform/image graph. PNG export uses Windows Imaging Component and writes a deterministic adjacent provenance sidecar containing the complete canonical recipe and semantic fingerprint.

The AM-004 preview accelerates an explicitly supported pointwise subset with generated HLSL. Recipes containing unsupported GPU operations remain interactive by falling back clearly to the unchanged canonical CPU evaluator and uploading that exact result for D3D11 presentation; unsupported nodes are never silently approximated. AM-005 reuses that preview for the currently selected specimen while rendering grid thumbnails progressively through the canonical CPU path. AM-006 intentionally advertises CPU-only canonical support for growth nodes, so the same explicit fallback path is used rather than approximating stateful semantics on the GPU.

AM-005 mutation is driven by `NodeMetadata`: declared domains, integer/enumerated discreteness, logarithmic and periodic scales, mutability, and logical groups are authoritative. Per-parameter deterministic streams are derived from stable node/parameter identity, so serialization/traversal order does not alter the child. Generation returns 16 stable row-major recipe identities before thumbnail completion; workers may finish visually out of order without changing slot identity. AM-006 uses that same metadata contract; each growth node's explicit `tick` is semantic but deliberately excluded from automatic mutation, while its growth parameters remain normal mutation axes.

Read [`RAG.md`](RAG.md) for the authoritative architecture, product contract, reviewed implementation plan, and milestone sequence. Read [`AGENTS.md`](AGENTS.md) before autonomous implementation work. The `.amr` grammar and compatibility rules are documented in [`docs/recipe-format.md`](docs/recipe-format.md); canonical raster/evaluator semantics are documented in [`docs/static-evaluator.md`](docs/static-evaluator.md); D3D11 capability, fallback, equivalence, resize, and device-loss behaviour are documented in [`docs/gpu-preview.md`](docs/gpu-preview.md); specimen mutation/history/persistence are documented in [`docs/specimen-browser.md`](docs/specimen-browser.md); and fixed-tick growth semantics are documented in [`docs/growth-systems.md`](docs/growth-systems.md).

## Target stack

- C++20
- Windows 10/11 x64
- Win32
- Direct3D 11 / DXGI
- Windows SDK D3D shader compiler facilities
- Direct2D / DirectWrite where useful for later native UI
- Windows Imaging Component
- CMake for developer/CI builds

The shipped application must have no third-party runtime installation requirement. The native targets use Windows platform facilities and statically link the MSVC C/C++ runtime.

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
build\Release\ArtMiner.exe --open examples\am006-reaction-diffusion.amr
build\Release\ArtMiner.exe recipe validate examples\am002-minimal.amr
build\Release\ArtMiner.exe recipe inspect examples\am006-cellular-automaton.amr
build\Release\ArtMiner.exe render examples\am003-fbm-warp.amr output\fbm-warp.png
build\Release\ArtMiner.exe render examples\am006-reaction-diffusion.amr output\reaction.png
```

Recipe and render commands are headless and do not require the GUI or a workspace. `recipe validate` prints the semantic fingerprint; `recipe inspect` also prints schema/evaluator version, seed, render settings, and graph counts. `render` evaluates output `main` through the canonical CPU path, writes the requested PNG, prints an RGBA8 image hash, and writes `<output>.artminer.txt` provenance beside the image.

AM-003 example families are available under `examples/am003-fbm-warp.amr`, `examples/am003-worley.amr`, `examples/am003-sdf.amr`, and `examples/am003-angular-repeat.amr`. AM-006 adds `examples/am006-reaction-diffusion.amr`, `examples/am006-cellular-automaton.amr`, `examples/am006-walkers.amr`, and `examples/am006-branching.amr`.

With no headless command, ArtMiner validates its workspace and opens the 4×4 native specimen browser. Use **File → Open Recipe…**, **Ctrl+O**, or `--open <file.amr>` to establish a parent. `M` generates parameter mutations, `N` generates seed-only variants, arrows move the active grid slot, `Enter` selects that recipe as the current parent, `F` toggles favourite status, `Alt+Left/Right` navigates recipe history, `Ctrl+S` saves a selected recipe copy, and `Ctrl+Shift+F` advances through persisted favourites. The seed and mutation strength controls are explicit and reproducible. The parameter panel supports validated edits plus individual and logical-group mutation locks.

For an AM-006 growth recipe, the same parameter panel exposes the node's explicit fixed `tick`. Set a non-negative tick and Apply to reconstruct that simulation state; set it back to `0` to reset. Tick edits are normal semantic recipe edits and participate in history/save/fingerprinting. Display refresh cadence and thumbnail completion timing are never simulation time.

The selected specimen uses the AM-004 preview path: supported pointwise graphs use GPU preview and unsupported graphs use explicit canonical CPU fallback. Window/layout changes, thumbnail completion timing, history navigation and lock toggles do not change recipe semantics unless an explicit recipe edit/mutation is performed.

By default the workspace root is the directory containing `ArtMiner.exe`; an explicit `--workspace` path replaces it. ArtMiner does not silently fall back to the registry or an unrelated profile directory if that location is unwritable.

The portable workspace contains/creates:

```text
<workspace>/
├── recipes/
│   ├── saved/                 # created on first explicit save
│   └── favourites/            # created on first favourite
├── palettes/
├── output/
└── cache/
```

Saved/favourite filenames use the semantic recipe fingerprint. Recipe writes are performed through same-directory temporary files and atomic Windows replacement. Favourites are validated and reloaded on the next normal application start; UI-only history is intentionally not persisted into recipe semantics.

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

AM-004 preserves that contract: GPU preview is explicitly non-canonical where floating point can differ, WARP-backed equivalence tests compare supported preview graphs against the CPU oracle within documented tolerances, and canonical fallback returns the CPU reference image rather than changing graph meaning.

AM-005 extends the deterministic contract to visual browsing. A mutation is identified by parent semantic fingerprint, explicit mutation seed, operator version and strength; each parameter gets a stable local PRNG stream. Seed-only variation is a separate operation. Grid candidate enumeration is independent of worker scheduling, and history/favourite UI state cannot silently alter semantic recipe identity.

AM-006 extends deterministic state into fixed-tick growth. Each self-contained growth node reconstructs tick `N` from its seeded initial state and exactly `N` ordered updates. Grid systems commit synchronously; walkers use stable agent order and keyed `(tick, walker)` decisions; branching uses stable tip IDs and activates children only on the following tick. Work is rejected under explicit checked limits before an excessive simulation loop. See `docs/growth-systems.md` for the normative AM-006 details.
