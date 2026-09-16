# AM-006 deterministic growth systems

AM-006 adds four self-contained stateful `ScalarField` generators. Their canonical implementation is the CPU/reference evaluator. Each evaluation reconstructs the requested state from recipe data; there is no persistent hidden simulation object, wall-clock input, display-frame input, thread-order dependency, or global random stream.

## Fixed-tick contract

Every AM-006 growth node has an explicit integer `tick` parameter. Tick `0` is the initial/reset state. Tick `N` means: construct the deterministic initial state from the recipe root seed and stable node identity, then apply exactly `N` synchronous/fixed update steps. Re-evaluating the same recipe at the same tick starts from tick zero again and must produce the same field.

`tick` is semantic recipe state and therefore participates in canonical serialization/fingerprinting. It is deliberately marked non-mutable in `NodeMetadata`: automatic specimen mutation explores system parameters without silently changing the observation time. In the specimen browser, select the node's `tick` parameter and use the existing validated parameter editor to inspect a chosen tick; entering `0` is reset. This creates a normal recipe-history entry, so no UI-only simulation position is hidden from saved semantics.

AM-006 does not persist snapshots. A later snapshot/cache may accelerate reconstruction only if keyed by all recipe/evaluator semantics; deleting it must not alter results. AM-007 owns general playback, external fixed-tick animation state and explicit graph feedback.

Growth randomness is keyed rather than shared. Initial cells, walker decisions and branching decisions are derived from `root_seed`, stable node identity, cell/agent identity and tick/event identity. Update/commit order is explicitly row-major for grid systems and stable ascending agent/tip order for agent systems. A future parallel implementation must preserve these results.

## Reaction diffusion

`core.growth.reaction_diffusion` implements a Gray-Scott-style two-species system. The output is species B in `[0,1]`.

Parameters are `feed`, `kill`, `diffusion_a`, `diffusion_b`, fixed `dt`, `seed_radius`, `seed_noise`, and `boundary`. Initialization starts with A=1/B=0, then seeds a deterministic jittered central disc plus optional deterministic sparse B seeds. Each tick computes every destination cell from the complete previous tick using a 9-sample Laplacian (`-1` centre, `0.2` orthogonal, `0.05` diagonal) and clamps concentrations to `[0,1]`.

`boundary=repeat` wraps both axes. `boundary=clamp` samples the nearest edge cell outside the raster.

## Multi-state cellular automaton

`core.growth.cellular_automaton` stores integer states `0..states-1` and outputs the state normalized to `[0,1]`. Initial live cells are selected independently by stable cell hashes and `initial_fill`.

Rules are synchronous. State zero becomes the maximum state when live-neighbour count is in the inclusive range `birth_min .. min(8, birth_min + birth_span)`. A non-zero cell is refreshed to maximum state when the count is in `survive_min .. min(8, survive_min + survive_span)`; otherwise it decays by one state. The span representation keeps every metadata-valid mutation executable without cross-parameter `min > max` failures.

`neighbourhood=moore` uses eight neighbours; `von_neumann` uses four cardinal neighbours. Boundary modes have the same wrap/clamp definition as reaction diffusion.

## Walkers and deposition

`core.growth.walkers` creates `walkers` agents in stable index order. `spawn=center` starts them at the raster centre; `spawn=seeded` derives each initial coordinate from its stable walker identity. On each tick every walker deposits `deposit` at its current cell (saturating at 1), obtains one of eight directions from a keyed `(root/node seed, tick, walker id)` decision, and moves according to the selected boundary mode.

Because decisions are keyed by explicit identities rather than by consumption of a shared RNG stream, worker scheduling cannot change the canonical path sequence.

## Branching growth

`core.growth.branching` starts `initial_tips` stable-ID tips at the raster centre with evenly distributed eight-way directions. Each active tip deposits, may turn by one eighth-turn, moves, and may create one child rotated by a quarter-turn. Children are collected in parent order and become active only on the next tick. Existing tips retain stable IDs; child IDs increase monotonically in deterministic parent/event order. `max_tips` bounds population growth and its declared minimum guarantees it cannot be smaller than `initial_tips`.

This is a lightweight deterministic branching/deposition primitive, not a physical diffusion-limited aggregation solver.

## Resource bounds and failure behaviour

The canonical evaluator rejects work before entering an excessive simulation loop:

- reaction diffusion and cellular automata: at most 67,108,864 `pixels × ticks`;
- walkers: at most 16,777,216 `walkers × ticks`;
- branching: at most 16,777,216 `max_tips × ticks`.

These are in addition to the existing canonical renderer limits of 4,194,304 pixels, 1,024 nodes and a 768 MiB estimated working set. Checked integer multiplication is used for work estimates. Allocation failure is converted to `resource_limit`. AM-006 evaluation is finite and reconstructive rather than a long-running background simulation; general interactive cancellation/playback belongs to AM-007.

## Mutation and preview

All non-tick growth parameters declare legal domains and logical mutation groups in the central node registry. `tick` has `MutationScale::none`; the other parameters use the normal AM-005 deterministic parameter mutation machinery. The chosen domains/rule representation ensure any metadata-valid mutation remains executable without hidden cross-parameter repair.

Growth nodes advertise canonical CPU support and no GPU evaluator. Therefore the AM-004 selected-specimen preview and AM-005 thumbnails use the existing explicit CPU fallback rather than approximating stateful semantics on D3D11.

## Examples

- `examples/am006-reaction-diffusion.amr`
- `examples/am006-cellular-automaton.amr`
- `examples/am006-walkers.amr`
- `examples/am006-branching.amr`

All can be rendered with the normal headless command, for example:

```bat
build\Release\ArtMiner.exe render examples\am006-reaction-diffusion.amr output\reaction.png
```

Changing only `tick`, saving the recipe, and rendering again is the canonical AM-006 way to request another fixed simulation state.
