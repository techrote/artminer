# Deterministic growth systems (AM-006)

ArtMiner's first stateful nodes are canonical CPU systems whose state is a pure consequence of the semantic recipe and an explicitly requested simulation tick. They do **not** free-run with the display refresh rate, and no hidden in-memory simulation object is authoritative.

## Fixed-tick contract

For every AM-006 growth node, canonical state is identified by:

- recipe schema and evaluator versions;
- node type and semantic version;
- complete node parameters and graph/output binding;
- recipe root seed and the existing deterministic per-node seed derivation;
- render dimensions;
- requested unsigned simulation tick.

Tick **0** is the deterministically initialized state. A request for tick **N** reconstructs tick 0 and applies exactly N ordered state transitions. Rendering tick 40, then tick 7, then tick 40 again must reproduce the same canonical tick-40 image. "Reset" therefore means selecting tick 0; it is not a mutation of the recipe.

AM-006 deliberately does not persist snapshots. Future snapshots/checkpoints may accelerate seeking, but they are disposable caches and must be keyed by the complete semantic identity above. Deleting them must not alter results.

## Canonical systems

### `core.growth.reaction_diffusion`

A two-species Gray-Scott-style reaction/diffusion lattice. Initialization is derived independently for each cell from the node seed. Each tick reads only the previous complete A/B lattices and writes separate next lattices, so traversal or worker completion order cannot affect results.

Parameters expose A/B diffusion, feed, kill, fixed `dt`, initial B density, palette, and either `wrap` or `clamp` boundaries. Clamp boundaries repeat the nearest edge cell; wrap boundaries are toroidal.

### `core.growth.cellular_automaton`

A configurable multi-state, eight-neighbour generations automaton. State 0 is empty, state 1 is live, and states 2..N-1 are deterministic decay/refractory states. Birth and survival use explicit inclusive neighbour-count ranges, so this is not limited to a single hard-coded Life rule.

Initialization is seeded per cell. Updates are synchronous into a separate next-state lattice. Boundary mode is `wrap` or `clamp`.

### `core.growth.walkers`

A fixed population of stable-ID lattice walkers deposits into a scalar trail field. Initial position/direction and every turn decision derive from `(node seed, walker ID, tick, substep, purpose)`. Walkers are processed in ascending stable ID order and each tick has an explicit fixed number of substeps. Optional deposition decay is applied once at the start of each tick.

Boundary mode is toroidal `wrap` or deterministic `reflect`. Preview/thread scheduling is not an input to walker order.

### `core.growth.branching`

A stable-tip branching growth primitive. It begins at the image centre with a deterministic orientation phase. Tips retain monotonically assigned IDs; each tick visits existing tips in stable order, tries neighbouring cells in a fixed deterministic sequence, deposits accepted cells, and appends newly spawned tips after the current pass. `max_tips` bounds population growth.

This is a branching/DLA-like visual primitive rather than a claim of physically exact diffusion-limited aggregation.

## Resource and cancellation behaviour

Canonical evaluation rejects requested ticks above 100,000 and also applies explicit tick×cell or tick×agent work ceilings before simulation. The existing reference-render pixel ceiling remains 4,194,304 pixels. Allocation failures become `resource_limit` errors rather than uncontrolled termination.

Growth evaluation accepts an optional atomic cancellation flag. It is checked before work and at deterministic safe points. Cancellation changes only whether a render finishes; a later render of the same recipe/tick starts again from canonical tick 0 and is unaffected by where cancellation occurred.

## Recipe, mutation, and provenance behaviour

All growth parameters are registered in the central node registry with type/domain, logical mutation group, semantic version, stateful classification, and CPU/GPU capability. AM-006 advertises only the canonical CPU implementation; no GPU equivalence is claimed yet.

Parameters and root seed participate in the normal semantic recipe fingerprint and specimen mutation system. The requested tick is deliberately an execution selection, not hidden recipe metadata and not part of the recipe fingerprint. Export provenance therefore identifies the recipe exactly, while the render command also reports the selected tick. AM-009 will generalize export manifests/frame ranges.

Growth nodes currently produce `Image` directly. They can be loaded by the normal specimen browser and safely mutated through existing metadata-driven controls. The browser's normal specimen thumbnails show canonical tick 0. Use the growth inspector when choosing another tick.

## UI and headless use

Render a requested canonical tick without opening the GUI:

```text
ArtMiner render examples/am006-walkers.amr output.png --tick 60
```

Open the native tick inspector:

```text
ArtMiner growth inspect examples/am006-reaction-diffusion.amr
```

The inspector exposes an explicit tick edit, **Render tick**, and **Reset to 0**. Tick selection is transient UI state and never edits or saves into the source recipe. Wall-clock time between button presses has no simulation meaning.

## Example families

The source tree includes four recipes with recommended inspection ticks:

- `am006-reaction-diffusion.amr` — tick 80;
- `am006-cellular-automaton.amr` — tick 28;
- `am006-walkers.amr` — tick 60;
- `am006-branching.amr` — tick 90.

Tests include exact multi-tick cellular-automaton image goldens, reset/replay identity, boundary-mode differentiation, distinct-family fixtures, semantic serialization/version checks, cancellation/resource-limit checks, and mutation/fingerprint checks. Earlier stateless reference-render tests remain unchanged and continue to own the AM-003 semantics.
