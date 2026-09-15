# ArtMiner — RAG / authoritative implementation plan

> **Status:** authoritative project plan for the initial implementation series.  
> **Product name:** **ArtMiner**. “Quarry” is the batch-search/prospecting mode inside the application.  
> **Repository:** `techrote/artminer`.  
> **Primary platform:** Windows 10/11 x64.  
> **Core principle:** a standalone, deterministic procedural-art laboratory that explores, breeds, searches, reproduces, and exports algorithmic visual systems without depending on another game, editor, browser, service, runtime, or AI model.

This file is intended to be sufficient retrieval context for autonomous implementation agents. `AGENTS.md`, current `main`, the active GitHub issue, accepted predecessor implementations, and this file are authoritative. If they conflict, resolve the conflict explicitly in the implementation PR rather than silently choosing an interpretation.

---

## 1. Product definition

ArtMiner is a portable native application for **prospecting procedural visual spaces**.

The primary workflow is:

1. construct or select a deterministic visual recipe;
2. render a grid of related specimens;
3. select interesting specimens;
4. mutate parameters while locking dimensions that are already correct;
5. cross compatible parents when useful;
6. search large candidate populations in **Quarry mode** using deterministic image/animation metrics;
7. inspect neighbourhoods around discoveries;
8. export images, textures, sprite sheets, palettes/LUTs, masks, glyph/ANSI forms, or compact recipes;
9. later reopen any saved recipe and reproduce the same semantics for the evaluator version recorded by that recipe.

The application is intended to make useful assets, but is deliberately **not integrated into any consuming project**. Exported files and recipes are the interface.

### Product character

ArtMiner should feel like a laboratory/instrument rather than a traditional paint package. Fast iteration, reproducibility, visual comparison, lineage, search, and useful accidents matter more than manual drawing tools.

### Explicit non-goals for the initial series

- no Electron, browser shell, Node, Python, JVM, .NET runtime requirement, or web service;
- no cloud requirement, account system, telemetry, or network dependency;
- no AI image model or model download;
- no plugin architecture before the built-in typed graph and recipe contracts are stable;
- no attempt to be a Photoshop/GIMP replacement;
- no early cross-platform abstraction exercise: Windows is the first-class target;
- no hidden global RNG, frame-rate-dependent simulation, or opaque generated result that cannot identify its recipe/provenance;
- no arbitrary graph cycles: feedback/state must cross an explicit deterministic state boundary.

---

## 2. Distribution and dependency contract

The target user distribution is a folder such as:

```text
ArtMiner/
├── ArtMiner.exe
├── recipes/
├── palettes/
├── output/
└── cache/
```

A single executable plus data folders is the preferred result. Defaults/resources may be embedded where sensible.

### Runtime dependencies

ArtMiner must have **zero third-party runtime installation requirements**. Windows system components are allowed. The baseline implementation should use:

- C++20;
- Win32 for process/window/input/platform integration;
- Direct3D 11 for GPU rendering/compute where useful;
- DXGI;
- Direct2D/DirectWrite for native UI/text integration where appropriate;
- Windows Imaging Component (WIC) for image I/O;
- Windows SDK facilities only unless a future decision explicitly changes this contract.

CMake is allowed as a build-system dependency for developers/CI. The shipped executable must not require CMake.

Do not add a third-party library merely for convenience when the required subset is small enough to implement safely in-project. Any proposed dependency must be justified against portability, binary size, licensing, deterministic behaviour, and maintenance cost before introduction.

### Portable state

By default, writable project state belongs under the ArtMiner folder/workspace selected by the user. Do not silently write recipes, settings, or generated art into the registry or unrelated profile directories. If the selected portable location is not writable, surface the condition clearly rather than silently changing persistence location.

---

## 3. Determinism, provenance, and compatibility

“Deterministic” must be defined precisely rather than promised loosely.

### 3.1 Deterministic state

Every generated result must derive from explicit state, including as applicable:

- recipe schema version;
- evaluator/node semantic versions;
- root seed;
- deterministic per-node/per-purpose derived seeds;
- complete parameter values;
- graph topology;
- simulation tick/index;
- input frame/event stream for interactive deterministic simulations;
- explicit render/export dimensions and quality mode.

There must be no hidden calls to process-global randomness.

Use a small model-owned PRNG contract with stable reference vectors. A recommended baseline is SplitMix64 for deterministic seed derivation plus PCG32 for streams, but the accepted implementation and test vectors become authoritative once AM-001 lands.

### 3.2 Reproducibility tiers

Do not conflate GPU preview repeatability with bit-exact archival reproduction.

- **Recipe determinism:** same recipe means the same graph, parameters, seed derivation, and simulation semantics for the recorded evaluator version.
- **Canonical/reference path:** CPU reference evaluators are the normative path for golden tests and archival export where exactness is required. Integer/fixed-point operations should be preferred where practical for primitives whose exact output is part of the contract.
- **GPU preview path:** GPU evaluators may use floating-point hardware and are required to remain within documented equivalence tolerances rather than pretending to be bit-identical across all GPUs/drivers.
- **Version compatibility:** recipe files record schema and semantic versions. A future evaluator change that changes output must either preserve the previous evaluator semantics for old recipes or perform an explicit, user-visible migration. Never silently reinterpret an old recipe.

### 3.3 Provenance

Every exported artifact should be able to identify the recipe that produced it. PNG metadata embedding is preferred when the format supports it; deterministic sidecar recipes are acceptable where embedding is not suitable. The embedded/sidecar data must be sufficient to reconstruct the recipe, not merely contain a display name.

Caches are disposable accelerators, never authoritative state.

---

## 4. Core architecture

Keep the architecture deliberately small and testable.

Suggested target structure:

```text
src/
  core/          deterministic model, PRNG, graph, recipes, parameters
  nodes/         field/image/simulation node implementations
  quarry/        batch generation, metrics, clustering/search
  export/        image/recipe/material/text exporters
  gpu/           D3D11 evaluator/preview implementation
  app/           Win32 application shell and UI
  platform/      narrow Windows-specific helpers
  cli/           headless commands hosted by the same product/core

tests/
  core/
  nodes/
  recipes/
  quarry/
  golden/

assets/          small source-controlled defaults only
scripts/         build/test/run helpers
```

The exact tree may evolve, but dependency direction must remain clear: deterministic core code must not depend on UI state.

### 4.1 One core, multiple execution surfaces

The native GUI and headless/CI commands must use the same model/evaluator contracts. Do not create a separate “test implementation” of recipe evaluation.

A single shipped `ArtMiner.exe` may expose headless subcommands/flags, e.g. recipe validation, deterministic reference rendering, metric calculation, or Quarry batches. A separate internal test binary is fine.

### 4.2 Typed graph

The graph operates on a small set of explicit data kinds such as:

- `ScalarField`;
- `VectorField`;
- `ColourField`;
- `Mask`;
- `ParticleSet`;
- `Palette`;
- `Image`.

Node metadata owns:

- stable node type identifier;
- semantic version;
- typed ports;
- parameter schema and defaults;
- legal parameter domain;
- mutation hints/domain/scale;
- deterministic/stateful classification;
- CPU evaluator capability;
- GPU evaluator capability where implemented.

Graph validation must reject incompatible edges and ordinary cycles. Stateful feedback is represented by an explicit delay/state node or equivalent tick boundary whose semantics are testable.

### 4.3 Recipe format

Avoid introducing a general-purpose serialization dependency solely for recipes. Use a small, human-inspectable, versioned format whose parser/writer is owned by the project. Requirements:

- deterministic canonical serialization;
- stable node IDs independent of display order;
- explicit graph edges;
- explicit parameter values;
- root seed and evaluator/schema versioning;
- extension-tolerant parsing where safe;
- clear rejection of malformed or unsupported data;
- canonical hash/fingerprint over semantic recipe content;
- tests for round-trip stability and malformed inputs.

Preferred extension: `.amr` (ArtMiner Recipe), unless AM-002 establishes a better documented name before public files exist.

---

## 5. Initial algorithm library

The early library should be intentionally broad enough to generate useful art but small enough that every primitive is well tested.

### Field and structural primitives

- constant/coordinate fields;
- deterministic value/gradient noise;
- Worley/cellular noise;
- fBm/fractal composition;
- domain warping;
- curl/vector fields;
- radial/angular fields;
- signed-distance primitives and combinations;
- interference/wave fields;
- Voronoi/Truchet-style structural patterns;
- symmetry, tiling, repetition, transforms.

### Image/colour stages

- threshold and quantisation;
- palette/LUT mapping;
- gradients;
- channel/luminance remapping;
- ordered dithering;
- blur/sharpen where deterministic reference semantics are practical;
- edge detection;
- displacement/warp;
- posterisation.

### Growth/stateful systems

- reaction-diffusion;
- multi-state cellular automata;
- diffusion-limited/branching growth where practical;
- fixed-step walkers/deposition.

### Particle/motion systems

- deterministic fixed-tick particle spawn/update;
- flow following;
- attraction/repulsion;
- orbiting;
- trails/deposition;
- merge/split behaviours where deterministic ordering is specified;
- palette cycling and explicit feedback.

---

## 6. Visual breeding and lineage

The first-class browsing surface is a specimen grid, initially 4×4.

A specimen is a complete recipe instance, not just a bitmap.

Required concepts:

- select a specimen as parent;
- deterministic regeneration from seed;
- mutate unlocked parameters;
- lock individual parameters or logical parameter groups;
- mutation strength/radius;
- mutation metadata supplied by node/parameter definitions rather than ad-hoc UI code;
- undo/redo and parent/child history;
- favourites/bookmarks;
- two-parent crossing with deterministic conflict handling;
- lineage identifiers and recipe-diff inspection.

Mutation must be reproducible: the parent fingerprint, mutation seed, mutation operator version, and strength must be sufficient to regenerate the same child.

Crossing must never silently connect incompatible graph ports. Initial crossing may operate conservatively on shared topology/parameters; topology recombination should wait until the graph-mutation issue.

---

## 7. Quarry mode

Quarry mode turns procedural generation into a searchable parameter/program space.

### 7.1 Batch generation

Quarry must support large deterministic candidate populations without storing full-resolution images for every rejected candidate.

The batch engine must therefore support:

- deterministic candidate enumeration;
- bounded memory;
- cancellation;
- resumable/checkpointable jobs;
- thumbnail/metric caching keyed by recipe/evaluator/render fingerprint;
- stable result ordering independent of worker scheduling;
- headless operation;
- explicit job manifests so a search can be reproduced.

### 7.2 Initial metrics

Implement transparent, deterministic metrics before considering any learned model:

- luminance/colour entropy;
- edge density;
- connected-component count/size statistics;
- bilateral/rotational symmetry estimates;
- dominant spatial scale/frequency;
- palette utilisation;
- empty-space ratio;
- repetition/periodicity estimate;
- tile seam error;
- directional bias;
- region diversity;
- for animation: motion energy and temporal stability/flicker.

Metrics must document resolution dependence and normalize where appropriate.

### 7.3 Diversity, clustering, and “unusual”

“Unusual” must be an explicit distance/diversity calculation, not a magic score. Later Quarry stages should:

- normalize selected metric vectors;
- cluster or deduplicate near-identical results;
- choose representative specimens;
- rank distance from population/cluster centres when the UI asks for unusual results;
- expose the contributing metrics so the ordering is inspectable.

### 7.4 Neighbour search

Given a selected specimen, ArtMiner should search local parameter space and present nearby but diverse results. A 2D neighbourhood map may project higher-dimensional metric/parameter distances, but the projection must be described rather than presented as literal geometry.

---

## 8. Outputs

The export layer must be intentionally decoupled from consuming applications.

Planned outputs include:

- PNG;
- BMP where useful through WIC;
- raw RGBA;
- image sequences;
- sprite sheets/atlases;
- seamless textures;
- masks;
- height maps;
- normal maps derived from explicit height semantics;
- indexed/paletted images where supported;
- palette CSV/text;
- `.cube` LUT where representable;
- glyph grids;
- ANSI/text output;
- `.amr` recipes and deterministic job manifests.

Exports must state dimensions, colour-space assumptions, alpha semantics, and any lossy conversion. Never infer that an arbitrary colour image is a physically meaningful height map without an explicit conversion node/setting.

Perfect-loop helpers belong in the animation/material phase. Loop claims require first/last state continuity tests, not merely a repeated image sequence.

---

## 9. UI and interaction principles

The UI exists to accelerate visual exploration, not to become a generic IDE.

Initial layout priorities:

- large specimen grid;
- clear selected parent/favourite state;
- parameter controls with locks;
- seed and mutation controls;
- history/lineage access;
- recipe graph/summary inspection;
- export controls;
- Quarry job/result controls;
- visible status for canonical CPU versus GPU preview paths.

The graph should become editable only as far as required for composing recipes; avoid spending early milestones on elaborate node-editor ergonomics before mutation/search/rendering is useful.

Keyboard shortcuts should cover rapid parent selection, mutate, favourite, back/forward history, and export. UI state must not affect deterministic recipe semantics unless an explicit user action modifies the recipe.

---

## 10. Performance and correctness targets

ArtMiner is exploratory software: responsiveness matters, but correctness and provenance must not be traded away invisibly.

### Baseline performance goals

- responsive 60 Hz UI/preview interaction on a representative mid-range Windows system at normal preview sizes;
- specimen thumbnails generated progressively rather than blocking the entire UI;
- GPU path for interactive preview and parallel-friendly nodes where useful;
- reference CPU path optimized enough for tests and archival exports without requiring real-time speed;
- Quarry jobs scale across CPU workers and/or GPU batches only where stable deterministic ordering is preserved.

Do not prematurely introduce complex task schedulers. Start with explicit bounded worker pools/job queues whose determinism is testable.

### Correctness gates

Each implementation issue must add tests appropriate to its contracts. The repository should maintain:

- deterministic PRNG vectors;
- recipe round-trip and validation tests;
- node golden/reference vectors;
- CPU/GPU equivalence tests with documented tolerances;
- malformed-input tests;
- Quarry ordering/resume/cache-key tests;
- exporter metadata/provenance tests;
- regression fixtures for accepted bugs.

A failing deterministic/golden test must not be “fixed” by casually replacing expected output. Explain and review any intentional semantic change.

---

## 11. Plan review: risks found and changes made

The initial concept was reviewed before decomposing implementation. The following refinements are part of the plan, not optional commentary:

1. **Exactness is tiered.** GPU output is not promised to be bit-identical across hardware; a canonical CPU/reference path owns archival/golden semantics.
2. **Feedback is explicit.** Ordinary graph cycles are forbidden; state crosses a named tick/delay boundary so evaluation order cannot become accidental.
3. **Node metadata is central.** Port types, parameter ranges, mutation behaviour, and semantic versions live with node definitions, preventing UI, breeder, and Quarry logic from inventing conflicting rules.
4. **Headless capability arrives early.** Core evaluation and later Quarry work are usable from CI/headless commands rather than being trapped behind UI automation.
5. **Recipes are versioned before public assets exist.** Schema, node semantics, canonical serialization, and fingerprints are foundational rather than retrofitted after exports proliferate.
6. **Preview caches are non-authoritative.** Cache keys include recipe/evaluator/render semantics; deleting cache must never lose a recipe or change meaning.
7. **Large Quarry runs are resumable and bounded.** Cancellation, checkpoints, stable enumeration, and bounded memory are contractual because “generate 50,000” is otherwise a demo rather than a reliable tool.
8. **Mutation starts conservatively.** Parameter mutation/crossing stabilizes first; topology mutation is delayed until typed graph validation, lineage, and deterministic operators are mature.
9. **Windows-native scope is deliberate.** Do not burden the first implementation with a cross-platform renderer/UI abstraction. Keep deterministic core code reasonably portable, but optimize the shipped application for Windows.
10. **No opaque novelty score.** “Unusual” is derived from exposed deterministic metrics/distances. This keeps Quarry inspectable and avoids creating a black-box taste engine.
11. **Export semantics are explicit.** Material-derived outputs such as normal maps require declared source semantics; the exporter must not silently reinterpret arbitrary images.
12. **Usefulness is front-loaded.** The implementation sequence produces deterministic static art, preview, and breeding before advanced Quarry, materials, and topology mutation.

---

## 12. Ordered implementation series

Issues are intended to be implemented **serially in the listed order** unless the issue explicitly states that it is parallel-safe. Each issue should leave `main` coherent and usable.

| ID | Milestone | Primary result |
|---|---|---|
| AM-001 | Native foundation | C++20/Win32 build, CI, tests, deterministic seed/PRNG contract, portable app skeleton |
| AM-002 | Recipe + typed graph | versioned `.amr`, node metadata, graph validation, canonical fingerprinting, headless validate/inspect |
| AM-003 | Static art core | canonical CPU field/image primitives, palettes/dither, reference renderer, PNG export |
| AM-004 | Interactive preview | Win32/D3D11 native UI/preview, GPU evaluators for supported primitives, CPU/GPU equivalence |
| AM-005 | Mutation browser | 4×4 specimens, seed navigation, parameter locks, deterministic mutation, history/favourites |
| AM-006 | Growth systems | reaction-diffusion, cellular/growth systems, explicit deterministic state semantics |
| AM-007 | Motion systems | fixed-tick particles, trails/deposition, feedback boundary, animation playback/stepping |
| AM-008 | Breeding + lineage | two-parent crossing, lineage graph, recipe diff, reproducible ancestry |
| AM-009 | Export + provenance | richer image/material-neutral exports, metadata/sidecars, sequences/sprite sheets, LUT/palette formats |
| AM-010 | Quarry engine | bounded/resumable deterministic batch jobs, metric extraction, headless job manifests |
| AM-011 | Quarry diversity search | dedupe/clustering, representative selection, exposed unusual-distance ranking, neighbour search/map |
| AM-012 | Glyph/ANSI synthesis | structural glyph mapping, terminal/text exports, palette-aware ANSI output |
| AM-013 | Material + loop workflows | seamless tiling diagnostics, masks/height/normal workflows, loop construction/validation |
| AM-014 | Topology mutation | type-safe deterministic graph insertion/removal/replacement/rewrites with lineage provenance |
| AM-015 | Release hardening | profiling, crash/recovery validation, portable packaging, documentation and v1 readiness audit |

Issue bodies are the immediate scope contract; this RAG remains the architectural contract.

---

## 13. Autonomous implementation protocol

Every implementation issue must be completable by an autonomous agent using the repository and GitHub issue as its context.

Unless an issue explicitly overrides a point, the agent must:

1. read the active issue in full;
2. read this `RAG.md`, `AGENTS.md`, current `README.md`, current `main`, and any documents explicitly referenced by the issue;
3. inspect accepted predecessor implementations rather than assuming their exact shape from this plan;
4. reconcile ambiguity in favour of existing tested contracts and record any material interpretation in the PR;
5. implement the complete issue rather than only scaffolding it;
6. add/update tests and documentation needed to keep the repository internally consistent;
7. run the relevant local build/test commands where available;
8. create a focused branch and PR linked to the issue;
9. inspect automated checks and repair implementation/CI failures instead of bypassing or weakening them;
10. merge only after all required automated checks pass and the acceptance criteria are genuinely satisfied;
11. verify the merge actually landed on `main` (do not infer success merely because a merge call returned);
12. close the issue only after verifying the merged `main` satisfies it;
13. leave a concise issue/PR record of any intentionally deferred work rather than silently omitting it.

### Prohibited completion shortcuts

- do not lower assertions, remove coverage, or weaken CI merely to obtain green checks;
- do not close an issue with known unmet acceptance criteria;
- do not merge a PR whose relevant required checks are failing or still pending;
- do not silently change deterministic semantics, recipe compatibility, or provenance contracts;
- do not add third-party runtime dependencies without an explicit repository decision changing the dependency contract;
- do not bundle unrelated cleanup unless it is necessary to complete the issue safely.

---

## 14. Decision log / unresolved product choices

The following are intentionally not guessed by the initial plan:

- repository/software licence;
- icon/branding/visual theme beyond the product name ArtMiner;
- exact maximum supported canvas/output dimensions;
- whether a later release should support non-Windows platforms;
- whether an optional learned similarity/novelty model is ever desirable.

These choices are not blockers for AM-001 through the core implementation series. Do not invent a licence or introduce AI/network dependencies to fill an unspecified choice.
