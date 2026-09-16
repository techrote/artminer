# ArtMiner recipe format (`.amr`) — schema 1

This document defines the first accepted ArtMiner recipe file contract introduced by AM-002 and the explicit fixed-tick state-boundary semantics activated by AM-007.

A recipe is a **semantic program description**, not a UI document. Its graph, versions, parameters, seed, render settings, and outputs determine meaning. Window layout, editor positions, history/favourites, and descriptive notes do not.

## Compatibility model

Three independent version layers protect old recipes:

1. `amr <schema-version>` protects the file/model schema;
2. `evaluator <semantic-version>` protects application-wide evaluator semantics;
3. every `node` record carries that node type's semantic version.

Schema 1 accepts only recipe schema `1` and evaluator semantic version `1`. A node is accepted only when its type ID exists in the built-in registry and the requested semantic version exactly matches the registry entry.

Unsupported semantic versions are rejected. They are never silently interpreted as the current version.

## Encoding and lexical rules

Files are UTF-8 text. A UTF-8 BOM is accepted but canonical serialization never emits one.

- one logical record per line;
- spaces and tabs separate tokens;
- blank lines are ignored;
- `#` begins a comment outside a quoted token;
- strings may be quoted with `"..."`;
- quoted strings support `\\`, `\"`, `\n`, `\r`, `\t`, and `\xHH` escapes;
- canonical serialization quotes all string-valued identifiers/text fields;
- unknown semantic record names are errors.

The parser intentionally does **not** ignore arbitrary unknown records. That would allow a future semantic feature to be silently lost by an older ArtMiner build.

## Required header records

Every recipe contains exactly one of each:

```text
amr 1
evaluator 1
seed <uint64>
render <width> <height> <quality>
```

Valid render dimensions are `1..16384` in each axis and the only accepted quality token is `reference`. Render settings are semantic even when a particular execution surface imposes a tighter resource limit.

## Nodes

```text
node <node-id> <type-id> <node-semantic-version>
```

Example:

```text
node "source" "core.scalar.constant" 1
```

Node IDs and type IDs are stable semantic identifiers. Schema-1 identifiers are 1–96 ASCII letters, digits, `.`, `_`, or `-`.

The built-in registry is the single source of truth for:

- typed input/output ports;
- parameter names/types/defaults/domains;
- mutation metadata and logical groups;
- stateless/stateful/state-boundary classification;
- CPU/GPU evaluator capability flags;
- node semantic version.

Recipe validation does not maintain a duplicate compatibility table.

## Parameters

Every declared parameter in a node's metadata must appear explicitly in a recipe. Defaults exist in metadata for authoring/mutation support, but schema-1 serialized recipes do not silently rely on them.

```text
param <node-id> <parameter-name> <type-tag> <value>
```

Type tags:

- `i64` — signed 64-bit integer;
- `f64` — finite IEEE-754 double;
- `bool` — `true` or `false`;
- `enum` — string constrained to the node metadata's allowed values.

Examples:

```text
param "source" "value" f64 0.5
param "img" "palette" enum "grayscale"
```

Parameter type and legal domain come from node metadata. Unknown, duplicate, missing, type-mismatched, non-finite, or out-of-domain values are validation errors.

## Edges

```text
edge <from-node> <from-output-port> <to-node> <to-input-port>
```

Example:

```text
edge "source" "value" "img" "source"
```

Validation requires:

- both nodes exist;
- both named ports exist in the correct input/output direction;
- source and destination `DataKind` match exactly;
- required inputs are connected;
- non-multiple inputs receive at most one edge;
- duplicate edges are rejected;
- the **same-tick dependency graph** is acyclic.

AM-007 activates the previously reserved `state_boundary` node class. An edge whose **destination node** is a state boundary does not form a same-tick dependency: when that boundary is observed at tick `N > 0`, its `next` input is evaluated at tick `N - 1`; at tick `0` it emits the boundary's explicit initial state. That temporal edge is therefore omitted from same-tick cycle detection.

This is the only legal feedback mechanism. Any cycle that remains after temporal edges entering explicit state-boundary nodes are removed is an ordinary same-tick cycle and is rejected. A stateful node by itself does not legalize a cycle. See `docs/motion-feedback.md` for the fixed-tick execution contract.

## Outputs

```text
output <name> <node-id> <output-port>
```

Example:

```text
output "main" "img" "value"
```

Output names are stable identifiers. Duplicate names, missing nodes, and unknown output ports are rejected.

## Non-semantic metadata / extension space

Schema 1 provides one deliberately safe extension mechanism:

```text
meta <namespaced-key> <value>
```

Example:

```text
meta "example.note" "non-semantic metadata"
```

`meta` is **defined to be non-semantic**. ArtMiner preserves and canonically sorts these records, but excludes them from the semantic fingerprint. It is suitable for notes, UI annotations, provenance display hints, or other information that must not change render meaning.

Future semantic extensions require an explicit schema/evaluator/node version change. Do not put render-affecting information in `meta`. AM-007 PNG sidecars may include `meta "render.tick" "N"` as a provenance annotation because the requested observation tick is deliberately a separate execution coordinate, not part of the recipe semantic fingerprint.

## Canonical serialization

Canonical serialization normalizes incidental file differences:

- fixed header order;
- nodes sorted by stable node ID/type/version;
- parameters sorted by name/value within each node;
- edges sorted by source/destination tuple;
- outputs sorted by output name/node/port;
- metadata sorted by key/value;
- canonical decimal integers;
- canonical finite floating-point text using sufficient round-trip precision;
- normalized `-0.0`/`0.0` representation as `0`;
- canonical quoting/escaping;
- one newline after every record.

Thus comments, whitespace, original node/edge/output order, and equivalent quoted/unquoted token presentation do not survive canonicalization.

## Semantic fingerprint

The semantic fingerprint is computed from canonical serialization **without `meta` records**.

Schema-1 algorithm:

1. `primary = FNV1a64(canonical-semantic-text)`;
2. `secondary = FNV1a64("ArtMiner.SemanticFingerprint.v1\n" + canonical-semantic-text)`;
3. fingerprint text is the two 16-hex-digit values concatenated, primary first.

The AM-002 reference recipe in `examples/am002-minimal.amr` has fingerprint:

```text
b9aed1288926e59c8c40115733cd32bf
```

This is a deterministic semantic identity/checking mechanism, **not a cryptographic integrity or security primitive**.

## Reference recipe

```text
amr 1
evaluator 1
seed 42
render 64 64 "reference"
node "source" "core.scalar.constant" 1
param "source" "value" f64 0.5
node "img" "core.image.from_scalar" 1
param "img" "palette" enum "grayscale"
edge "source" "value" "img" "source"
output "main" "img" "value"
meta "example.note" "non-semantic metadata"
```

It describes a typed graph and remains a valid stateless recipe. AM-003 provides its canonical static pixels; later fixed-tick evaluators consume the same parser, registry, validator and semantic identity.

## CLI

After building:

```bat
ArtMiner.exe recipe validate path\to\recipe.amr
ArtMiner.exe recipe inspect path\to\recipe.amr
```

`validate` parses, validates, and prints the semantic fingerprint. `inspect` additionally prints version, seed, render, and graph-count information.

Both paths are headless and use the same core parser/registry/validator as the application.
