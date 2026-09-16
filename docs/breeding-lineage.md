# AM-008 deterministic breeding, lineage, and recipe diff

AM-008 adds conservative two-parent visual breeding and durable ancestry records without changing the meaning of `.amr` recipe fingerprints.

## Semantic boundary

A recipe remains the render program. Its semantic fingerprint is still computed from canonical semantic recipe text and excludes all `meta` records. Lineage is provenance: removing every `artminer.crossover.*` metadata hint or deleting every external lineage record must not change the recipe fingerprint or canonical rendered pixels.

The durable lineage format is separate from `.amr` and is stored as `.aml` files. This keeps ancestry, UI navigation, and missing historical files from becoming render semantics.

## Crossover operator v1

`kCrossoverOperatorVersion == 1` is intentionally conservative. It accepts two independently valid parents only when they have identical:

- recipe schema and evaluator versions;
- render width, height, and quality;
- node ID set, node type IDs, and node semantic versions;
- parameter name/type layout for every node;
- edge set;
- output bindings.

Parameter values and root seeds may differ. Unsupported topology crossing is rejected rather than repaired, coerced, or silently simplified. Structural/topology mutation remains deferred work.

Parent roles are ordered. `A × B` is not defined as equivalent to `B × A`. The ordered semantic fingerprints, operator version, and explicit crossover seed form the deterministic crossover domain.

The child root seed is deterministically derived from that ordered domain. For mutable parameters:

- parameters with no logical mutation group receive an independent deterministic A/B inheritance decision;
- parameters in the same logical mutation group on the same node share one inheritance decision, so related coordinates or controls travel together;
- an individual parameter lock or logical-group lock preserves Parent A's value;
- metadata-marked immutable parameters and `MutationScale::none` parameters preserve Parent A's value;
- no topology, ports, node versions, or parameter types are changed.

The finished child is validated by the normal recipe validator before it is returned.

## Immediate recipe provenance hints

A crossover child receives these non-semantic `meta` hints:

```text
artminer.crossover.parent_a
artminer.crossover.parent_b
artminer.crossover.seed
artminer.crossover.operator
artminer.crossover.locks
```

They describe the immediate operation and can be converted to a `LineageRecord`. They are convenience provenance only, not the durable ancestry store and not part of semantic identity.

AM-008's canonical lock encoding is ordered and version-independent at the record layer:

```text
p/<node-id>/<parameter>;g/<node-id>/<group>
```

Parameter locks sort before group locks because both underlying lock sets are ordered. `/` and `;` are not legal schema-1 identifiers, so the encoding is unambiguous for current node/parameter/group identifiers.

## Durable `.aml` record

Lineage format version 1 is line-oriented UTF-8 text. A crossover example is:

```text
aml 1
kind crossover
child <semantic-fingerprint>
parent_a <semantic-fingerprint>
parent_b <semantic-fingerprint>
operator 1
seed 12345
locks -
```

A parameter-mutation record additionally contains `strength` and its exact canonical lock set. A seed-only variation has one parent and no strength/locks. Records reject unknown fields, duplicate fields, unsupported format versions, malformed numbers, and invalid operation-specific field combinations.

The record contains everything needed to replay the operation once the referenced semantic parent recipes are available. Replay verifies parent fingerprints before executing and verifies the produced child's fingerprint afterward. A mismatch is an error; replay never substitutes a similarly named or newer recipe.

For a chained ancestry, each generated child can also be stored as a specimen snapshot and can itself become the parent referenced by a later mutation or crossover record. Parameter-mutation records explicitly carry strength and lock state so a mutation after a crossover remains reproducible.

## Portable workspace layout

AM-008 stores ancestry below the existing portable recipes directory:

```text
recipes/
  lineage/
    records/
      <child-fingerprint>.aml
    specimens/
      <semantic-fingerprint>.amr
```

Both record and specimen writes use same-directory temporary files followed by write-through atomic replacement on Windows.

A specimen snapshot is named by and verified against its semantic fingerprint. A record is named by its child fingerprint. The loader validates the child recipe independently of its ancestry.

Missing ancestry is therefore a provenance degradation, not a recipe failure. If a parent snapshot is deleted, the child `.amr` still parses, validates, fingerprints, and renders normally; the lineage browser shows the parent reference as unavailable. The `.aml` record remains inspectable.

## Native lineage browser

Run:

```bat
ArtMiner.exe lineage
```

or preload ordered parents:

```bat
ArtMiner.exe lineage parent-a.amr parent-b.amr
```

The native window provides:

- explicit Parent A and Parent B loading;
- an unsigned 64-bit crossover seed;
- deterministic `Breed` using crossover operator v1;
- stable semantic/provenance `Diff A/B` output;
- a lineage-neighbourhood list containing the current specimen's parents and children;
- `[snapshot missing]` markers for unresolved historical parents/children;
- double-click or `Use as A` navigation to an available stored specimen;
- `Open Browser` to return an available stored specimen to the normal 4×4 specimen browser.

After a successful breed, both parents, the child, and the `.aml` operation record are persisted before the child becomes Parent A. If persistence fails, the visible parents are left unchanged so the UI does not imply durable ancestry that was not actually recorded.

The native breeding window currently supplies no crossover locks of its own; the core crossover API accepts the same `ParameterLocks` model used by the specimen browser, and regression coverage verifies parameter/group lock semantics. This avoids creating a second incompatible lock model in the UI.

## Recipe diff contract

`diff_recipes` produces two independently ordered sections:

- `semantic` — schema/evaluator, seed, render settings, node type/version/parameter values, edges, and outputs;
- `provenance` — non-semantic metadata.

Entries are flattened to stable lexical paths and sorted by path. Missing values are written as `<absent>`. `format_recipe_diff` preserves that order.

This separation is deliberate: a provenance-only edit must produce an empty semantic section, while a parameter or topology change must be visible in the semantic section. Diff output is diagnostic/review material, not a parser or patch format.

## Determinism and failure rules

For fixed Parent A, Parent B, crossover seed, operator version, and locks, crossover v1 must produce exactly the same semantic child recipe and fingerprint.

Breeding/replay fail explicitly when:

- either parent is invalid;
- parent topology/render contracts are incompatible;
- an operator or lineage version is unsupported;
- a stored lineage record is malformed;
- replay is given a parent whose semantic fingerprint does not match the record;
- replay produces a child fingerprint different from the recorded child.

No timestamp, UI order, file enumeration order, worker scheduling, cache contents, or wall-clock state participates in crossover or replay.

## Regression coverage

`ArtMinerBreedingTests` covers:

- deterministic repeated crossover;
- ordered parent-role semantics;
- parameter and logical-group locks;
- grouped inheritance decisions;
- rejection of valid-but-incompatible parents;
- child graph validation;
- semantic/render invariance when crossover provenance is removed;
- canonical `.aml` round-trip and crossover replay;
- replay of a post-crossover parameter mutation including its lock set;
- stable semantic-versus-provenance recipe diff;
- portable record/specimen persistence;
- continued child loading/rendering after a parent snapshot is deleted.
