# AM-014 deterministic topology mutation

AM-014 adds a deliberately conservative structural-search layer on top of ArtMiner's typed recipe graph. It does not replace parameter mutation: topology search is an explicit, separately versioned operation whose seed, strength, edit budget, locks, and accepted edit sequence are durable provenance.

## Operator contract

Topology operator version `1` has five metadata-derived operator families:

- **safe node insertion** — inserts a stateless CPU-reference node only where one required input and a compatible output can preserve the existing typed edge;
- **compatible-node replacement** — replaces only stateless nodes whose complete named input/output interface exactly matches the replacement;
- **deletion/bypass** — removes a non-output stateless node only when its single incoming producer can unambiguously satisfy every outgoing target;
- **branch duplication** — clones one stateless node and redirects one outgoing branch while cloning its existing inputs;
- **type-compatible rewiring** — substitutes a producer output of the required `DataKind`; the normal graph validator rejects cycles, duplicate edges, missing inputs, and other invalid candidates.

Legality is derived exclusively from `NodeMetadata`, `PortSpec`, `DataKind`, normal recipe validation, structural locks, and state class. There is no parallel UI compatibility table.

## Determinism and bounded search

The operation root is derived from the parent semantic fingerprint, user topology seed, operator version, strength, edit budget, structural-lock encoding, and the fixed resource-limit contract. Each step enumerates a canonical, lexicographically sorted opportunity set and selects a deterministic start position. Invalid candidates are scanned in canonical circular order; the scan is finite and bounded by the opportunity set.

Strength maps to a requested edit count `ceil(strength * budget)` (zero strength requests zero edits). The public edit budget is `1..16`. Hard safety bounds are 256 nodes, 512 edges, dependency depth 128, and 65,536 enumerated opportunities per step. Limits fail with explicit diagnostics rather than silently truncating semantics.

New node IDs are deterministic derivatives of the operation root, step index, and canonical opportunity key. Canonical `.amr` serialization already sorts nodes, parameters, edges, outputs, and metadata, so UI/in-memory order cannot change the resulting semantic fingerprint.

## Protected structure and locks

`StructuralLocks` freeze individual node definitions and every incident edge. `set_upstream_subgraph()` freezes a selected root plus its complete upstream dependency closure. Lock serialization is canonical `n/<node-id>` tokens in sorted order and malformed/noncanonical provenance is rejected.

All non-stateless nodes are protected from topology operators. In particular, `state_boundary` nodes and every edge incident to them are immutable to AM-014. This preserves the explicit AM-007 feedback/state-boundary contract rather than attempting to infer new feedback semantics.

## Provenance and lineage

A topology-mutated recipe records non-semantic `artminer.topology.*` metadata:

- immediate parent fingerprint;
- topology seed and operator version;
- strength and budget;
- canonical structural locks;
- requested/accepted edit counts and exhaustion state;
- the ordered accepted operator/description sequence.

AML is versioned to v2 for topology lineage. The common lineage API can extract, serialize, parse, and replay topology mutations. The replay path re-runs the versioned operator and requires the exact stored child fingerprint. AML v1 remains readable for existing parameter/seed/crossover records.

Recipe diff needs no topology-specific side channel: its existing semantic flattening already reports node, edge, parameter, and output changes, while topology provenance remains in the provenance section.

## Specimens, Quarry, render, and export

`generate_topology_specimen_grid()` exposes the normal 4x4 specimen model with explicit structural locks and budget. It is intentionally separate from parameter/seed specimen generation.

Quarry exposes `CandidateGenerationSettings` with an explicit `parameter` or `topology` mode. The existing AM-010 manifest/run path remains parameter-mutation compatible; opt-in topology candidate reconstruction uses the same deterministic candidate seed domain and includes mode/operator/budget/structural locks in a separate generation identity. The headless `topology quarry-candidate` command materializes that candidate without conflating it with an AM-010 parameter job.

Topology children are ordinary valid `Recipe` values. They therefore use the existing canonical renderer, transactional AM-009 exporter, favourites/history persistence, recipe diff, and semantic identity without special rendering/export code.

## Headless commands

```
ArtMiner topology mutate <input.amr> <seed> <strength> <budget> <output.amr> [locks]
ArtMiner topology grid <input.amr> <seed> <strength> <budget> <output-dir> [locks]
ArtMiner topology quarry-candidate <job.qjob> <candidate-index> <budget> <output.amr> [locks]
```

Locks are repeatable `--lock-node <id>` or `--lock-upstream <root-id>` options.

## Regression contract

`ArtMinerTopologyTests` covers deterministic replay, canonical ordering, normal-validator validity over a large seed sample, all five catalogued operator families, explicit graph-growth bounds, node/subgraph locks, state-boundary preservation, AML replay and malformed provenance rejection, stable structural recipe diff, 4x4 specimen generation, canonical rendering, transactional export, Quarry topology identity/reconstruction, and resource-limit rejection.
