# AM-005 specimen browser and deterministic mutation

AM-005 makes the 4×4 specimen browser the primary interactive ArtMiner surface. A specimen is always a complete `.amr` recipe instance. Thumbnails, selection highlights, history position and lock state are presentation/exploration state; they are not substitutes for recipe semantics.

## Deterministic parameter mutation

Parameter mutation operator version 1 takes these explicit inputs:

- the parent recipe semantic fingerprint/content;
- a 64-bit mutation seed;
- operator version `1`;
- mutation strength in `[0,1]`;
- the current parameter/group lock set.

The operator validates the parent before mutation and validates every generated child afterwards. Random streams are derived independently per stable `node-id.parameter-name`, so source serialization order and traversal order do not change the mutated values. No wall-clock or process-global random state participates.

Mutation behaviour comes from the existing node metadata:

- `linear` real parameters receive a bounded additive perturbation and are clamped to their declared domain;
- `logarithmic` positive real parameters move in log space and remain inside the declared positive domain;
- `periodic` real parameters wrap in their declared interval rather than accumulating an out-of-domain value;
- integer/discrete parameters change by bounded integral steps;
- enumerations select only declared values;
- Boolean parameters may flip according to the deterministic stream and strength;
- `none`/non-mutable parameters are not changed.

An individual parameter lock suppresses that parameter. A logical-group lock is node-scoped and suppresses every parameter on that node carrying the matching mutation group from `NodeMetadata`. Locks are browser state: applying or removing a lock does not change the recipe fingerprint until a model edit/mutation actually creates a different recipe.

The generated recipe may carry `artminer.mutation.*` metadata describing the operation. Recipe metadata is explicitly non-semantic under the AM-002 contract, so provenance annotations do not change the child semantic fingerprint.

## Seed-only variation

Seed-only generation is a separate action and operator mode. It derives a new recipe root seed deterministically while leaving graph topology and every parameter unchanged. The UI labels seed variants separately from parameter mutation so the distinction is explicit.

## Stable 4×4 generation

A grid generation enumerates exactly 16 row-major slots. Each slot receives an operation seed derived from the explicit generation seed, generation mode and slot index. The returned vector is always stored in slot order.

Canonical thumbnail rendering is progressive. A small bounded worker pool may complete slot renders in any order, but completion messages carry both the grid generation identifier and slot index. The UI places each result into its predetermined slot and discards stale results from an older generation. Worker completion timing therefore changes only when pixels appear, never specimen identity or ordering.

The selected specimen uses the AM-004 D3D11 preview path. GPU-supported recipes are previewed on the accelerated path; unsupported graphs continue to use the explicit canonical CPU fallback.

## History, editing, persistence, and recovery

Selecting a specimen or applying an explicit parameter edit pushes a complete recipe onto reversible in-memory history. Back/forward navigation only changes the history cursor; it never edits an entry. Creating a new child after navigating backward truncates the stale forward branch in conventional undo-history fashion.

The parameter list is derived from node metadata and the selected recipe. Text edits are parsed against the declared parameter kind/domain and the entire edited recipe is validated before becoming history state.

Selected recipes, favourites, and the current recovery snapshot are persisted under the portable workspace:

```text
<workspace>/recipes/
├── saved/<semantic-fingerprint>.amr
├── favourites/<semantic-fingerprint>.amr
└── recovery/current.amr
```

Writes use a same-directory temporary file followed by `MoveFileExW` replacement with write-through. Favourites are reloaded and validated on startup. AM-015 also snapshots the current selected recipe after adoption or history movement. This recovery file contains only a normal canonical recipe; thumbnail state, locks, UI layout, and history branches are not made semantic or authoritative.

Normal startup precedence is explicit `--open`, then a valid recovery snapshot, then the first persisted favourite in fingerprint order, then an empty browser. Malformed, oversized, invalid-UTF-8, or graph-invalid recovery data is reported and ignored rather than guessed. A subsequent valid selection can replace it atomically. History itself remains in-memory only.

Browser shutdown joins the bounded thumbnail worker pool and drains already-posted completion objects. This closes the shutdown lifetime window without changing grid ordering or recipe identity.

## Interaction

Pointer controls cover specimen selection, mutate, seed variants, favourite, save, back/forward, parameter edit, and parameter/group locks. Keyboard navigation covers the fast browsing loop: `M` mutate, `N` seed variants, `F` favourite, arrows move the active 4×4 slot, `Enter` selects it, `Alt+Left/Right` traverses history, `Ctrl+O` opens a recipe, `Ctrl+S` saves the selected recipe, and `Ctrl+Shift+F` advances through persisted favourites.

The seed and strength controls are always explicit. Repeating a generation with the same parent and controls reproduces the same 16 recipe identities.
