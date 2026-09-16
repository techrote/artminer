# Fork readiness and reusable engine boundary

ArtMiner 1.0 remains a procedural-art laboratory. AM-016 does not turn it into a second product, add a plugin system, or implement a synthetic-data application. It makes the deterministic substrate that ArtMiner already depends on explicit enough that a downstream project can inherit it without also inheriting ArtMiner's domain assumptions.

## Fork checkpoint

The fork-ready checkpoint is the merged AM-016 commit on `main`. A downstream project should record that exact commit SHA as its ancestry point. The SHA is preferable to a moving branch name because it is immutable and identifies the precise recipe/validation/build contracts inherited by the fork.

The reusable seam is the CMake target `artminer_engine`:

```text
artminer_engine
    graph contracts and generic validation
    node-registry mechanics
    canonical recipe parse/serialization/fingerprints
    deterministic PRNG and hashing
        ↓
artminer_core
    ArtMiner built-in node catalog
    mutation/breeding/lineage/specimen semantics
    ArtMiner growth/material/glyph metadata
        ↓
artminer_nodes / quarry / export / gpu / app / platform
```

`artminer_engine` deliberately contains no Win32 UI, D3D preview, Quarry search policy, WIC export, ArtMiner built-in node catalog, or domain evaluator implementation. The current repository as a whole is still Windows-first; AM-016 does not make a cross-platform support claim.

## Contracts worth preserving in a downstream fork

Preserve these unless the fork deliberately versions a semantic break:

- explicit root seeds and model-owned deterministic randomness;
- stable seed derivation and PRNG reference vectors;
- versioned recipe schema and node semantic versions;
- canonical recipe serialization and semantic fingerprints;
- typed ports and ordinary-cycle rejection with explicit state boundaries;
- bounded resource validation before expensive execution;
- deterministic ordering where concurrency is introduced;
- canonical/reference execution for reproducibility claims;
- provenance sufficient to identify the generating recipe and execution semantics;
- regression fixtures that make semantic changes visible rather than silently updating expected output.

Existing ArtMiner `.amr` files remain ArtMiner recipes. A fork may retain the format during early development, but should introduce its own product/file identity before publishing incompatible semantics. Do not silently reinterpret ArtMiner schema/evaluator version `1` to mean something different.

## What a downstream domain should replace or add

A downstream project is expected to supply its own catalog and execution layers rather than modify the generic validator for each new domain. Typical additions include:

- domain-specific data kinds or typed payloads when the existing field/mask/image kinds are insufficient;
- node/operator metadata and evaluator implementations;
- latent-state representations;
- observation/sensor models;
- annotation/ground-truth extraction;
- parameter samplers, joint distributions and constraints;
- dataset manifests, partitioning and coverage/calibration metrics;
- domain-specific export formats and metadata.

Cross-parameter legality belongs in node metadata. AM-016 introduces generic declarative parameter relations specifically so the graph validator does not need checks such as `if node.type_id == ...`. More relation kinds can be added when a real downstream requirement justifies them.

For synthetic-data work, proprietary or customer-specific process knowledge should normally be represented as configuration/profile data consumed by the domain layer, not embedded as assumptions in `artminer_engine`.

## What is intentionally not generalized yet

AM-016 intentionally does **not** provide:

- a dynamic plugin ABI or runtime DLL loading contract;
- a generic evaluator-dispatch interface for arbitrary external node implementations;
- a universal tensor/volume/mesh data model;
- cross-platform application support;
- dataset storage or ML framework integration;
- calibration against real-world datasets;
- semiconductor-specific parameters, process physics, defect taxonomy or instrument simulation;
- network services, cloud execution or learned models.

Those should be introduced only when a downstream use case demonstrates the required contract. Prematurely adding them to ArtMiner would make the fork boundary less stable, not more reusable.

## Engine-only proof

`ArtMinerEngineForkTests` links **only** `artminer_engine`. It constructs a custom non-ArtMiner node registry, parses and validates a custom recipe, round-trips its canonical representation/fingerprint, and verifies a catalog-declared cross-parameter relation. This is the regression gate that prevents the reusable layer from accidentally acquiring a dependency on the ArtMiner built-in catalog.

ArtMiner's existing test suite remains the compatibility gate on the other side of the seam: the refactor must not change accepted recipe fingerprints, canonical renders, mutation behavior, exports, Quarry semantics, or release-hardening contracts.
