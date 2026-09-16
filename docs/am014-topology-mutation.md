# AM-014 deterministic topology mutation contracts

AM-014 extends ArtMiner from numeric parameter search into bounded structural search over the existing typed recipe graph. Structural mutation remains an explicit mode: ordinary parameter mutation, seed-only variation, and AM-008 shared-topology crossover keep their established semantics.

## Versioned operator catalog

Topology operator semantic version 1 contains five conservative operator families:

| Operator | Legal action |
| --- | --- |
| `insert-node` | Split an existing edge with a stateless node having exactly one required compatible input and a compatible output. |
| `compatible-replacement` | Replace a non-boundary node while preserving its stable ID and every connected/input/output port name and type required by the existing graph. |
| `delete-bypass` | Remove a stateless, non-output node only when it has exactly one incoming and one outgoing edge and the bypassed graph validates. |
| `duplicate-branch` | Duplicate a stateless node, copy its incoming dependencies, and move one outgoing branch to the duplicate. |
| `typed-edge-rewire` | Move one input dependency to another existing output of the required `DataKind`, subject to normal graph validation. |

Operator opportunities are derived from `NodeRegistry`, `NodeMetadata`, typed ports, state classification, and the current recipe. There is no GUI-maintained compatibility matrix. Every opportunity is applied to a candidate recipe and accepted only if the ordinary `validate_recipe()` contract succeeds.

## Determinism and bounded selection

The selection domain includes the parent semantic fingerprint, topology seed, topology operator version, requested edit budget, structural limits, and canonical structural-lock set. Candidate opportunities are enumerated in canonical order and selected by project-owned deterministic seed derivation. UI ordering, pointer values, hash-table iteration, worker order, wall-clock time, and random retry counts do not participate.

`budget` is a maximum accepted-edit count in `1..16`. Mutation stops early only when no further legal edit exists. There are no unbounded retries. Generated node IDs use a deterministic hash domain with at most eight collision attempts.

The hard implementation ceilings are 512 nodes, 1024 edges, graph depth 128, and 65,536 legal opportunities at one edit step. A mutation may request lower limits. A parent that already exceeds the requested limits is rejected before editing; every accepted intermediate and final graph is checked against the requested bounds.

## Structural locks and state boundaries

`StructuralLocks` freezes individual stable node IDs. `lock_subgraph()` freezes a root and all graph descendants reachable from it. Lock serialization is canonical (`n/<node-id>` tokens sorted lexically), so locks participate deterministically in mutation and replay.

An explicit `NodeStateClass::state_boundary` is protected even when the user supplied no lock. Topology operators do not replace/delete/duplicate a boundary or alter an edge incident to one. Ordinary cycle validation continues to treat state boundaries exactly as the accepted AM-007 contract does; topology mutation never bypasses the validator to manufacture feedback.

Recipe output bindings are retained. A node directly bound as a recipe output cannot be deleted; replacement is accepted only when the bound output port remains compatible.

## Provenance and lineage

A successful structural mutation writes non-semantic `artminer.topology.*` provenance containing:

- parent semantic fingerprint;
- operation seed;
- operator version;
- requested budget;
- node/edge/depth limits;
- canonical structural locks;
- canonical accepted-op trace.

`replay_topology_mutation()` reconstructs from those fields and requires both the semantic child fingerprint and accepted trace to match.

AM-014 also defines the versioned `aml-topology 1` lineage record. It stores child/parent fingerprints and the complete structural replay state. Parsing is strict: missing, duplicate, unknown, malformed, unsupported, or inconsistent fields fail instead of being guessed. This is deliberately separate from AM-008's parameter/shared-topology AML records so existing lineage semantics are not silently reinterpreted.

Ordinary `diff_recipes()` already flattens nodes, edges, outputs, and parameters into stable semantic paths, so topology changes appear in the existing recipe-diff surface without a second diff algorithm.

## Specimen-browser integration

`generate_topology_specimen_grid()` returns the same complete `GeneratedSpecimen` model used by the 4x4 browser, but has a deliberately separate entry point from ordinary parameter/seed generation. Each cell derives its topology operation seed deterministically from the grid seed and cell index. This prevents a UI action labelled as numeric mutation from silently changing program structure.

Structural locking is model state supplied explicitly to this operation; UI layout or selection order is not recipe semantics.

## Quarry integration

Quarry's established parameter candidate enumeration remains unchanged. AM-014 adds an explicit `TopologySearchConfig` and `reconstruct_topology_candidate()` path. The topology config has its own deterministic text serialization and carries operator version, budget, structural limits, and locks.

Topology-search identity hashes the ordinary Quarry job identity/base recipe and the complete topology config. Candidate identity additionally includes candidate index and resulting semantic recipe fingerprint. Therefore a structural candidate cannot collide conceptually with an ordinary parameter-search candidate simply because both used the same Quarry root seed/index.

The reconstructed result is a normal `Recipe`; existing rendering, metrics, export, favourite/save, and recipe-diff paths consume it without topology-specific shortcuts.

## Canonical identity

AM-014 does not add UI position to `Recipe`. Existing canonical recipe serialization sorts semantic node/edge records, so changing storage/display order leaves the parent fingerprint unchanged. Topology opportunity enumeration separately sorts node IDs, edges, metadata types and ports before deterministic selection. The same canonical parent plus replay inputs therefore yields the same semantic child independently of presentation order.

## Failure model

Structural mutation fails explicitly for invalid parents, unsupported operator versions, invalid budgets/limits, parents already beyond limits, an exhausted bounded opportunity set, absence of any legal mutation under current locks/types/boundaries, malformed provenance, parent mismatch, or a candidate that unexpectedly fails normal validation. An invalid opportunity is never accepted and then repaired after the fact.
