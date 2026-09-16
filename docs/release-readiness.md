# ArtMiner 1.0 release readiness

This document is the release record for AM-015. It distinguishes deterministic/correctness contracts from performance observations and hardware-dependent UI goals.

## Release identity

The product release is `ArtMiner 1.0.0`, channel `stable`. `ArtMiner --version` prints the product version plus the recipe, evaluator, exporter, mutation and Quarry semantic-version matrix. GUI browser and lineage titles also display `1.0.0`.

The recipe schema and canonical evaluator remain version 1. The v1 release does **not** change canonical rendering semantics established by earlier milestones; AM-015 hardens boundaries, persistence and packaging around those semantics.

## Portable package

`scripts/0Package.cmd` is the authoritative packaging path. From a Release build it creates a clean `dist/ArtMiner-1.0.0-win-x64/` directory and a matching ZIP. The staged package contains the native `ArtMiner.exe`, project documentation, example recipes and portable workspace directories. It deliberately does not copy compiler outputs, caches or developer debris.

The release uses the static MSVC runtime (`/MT` in Release), Windows system APIs/libraries already required by ArtMiner, and no third-party runtime or installer. Package creation smoke-tests the exact staged executable with:

- `ArtMiner --version`
- portable workspace creation/check
- recipe validation
- a canonical headless PNG render

GitHub Actions builds/tests Release, runs the representative benchmark, runs the packaging/smoke script, and publishes the ZIP as a workflow artifact.

## Local input and resource limits

Local recipe text is treated as untrusted input. File-backed recipe loading is bounded before allocation and canonical parsing enforces the same contract for in-memory callers.

| Boundary | v1 limit / behaviour |
| --- | --- |
| Recipe file/source | 8 MiB maximum |
| Single recipe source line | 64 KiB maximum |
| Encoding | strict UTF-8; malformed UTF-8 and raw NUL rejected |
| Nodes | 4,096 |
| Parameters | 65,536 total |
| Edges | 16,384 |
| Outputs | 1,024 |
| Metadata records | 4,096 |
| Render dimension | 1..16,384 each, additionally checked for byte-count overflow |
| Persisted favourites | 4,096 entries |
| Persisted lineage records/specimens | 4,096 entries per lineage directory |
| Export frame count | 256 |
| Quarry candidates | existing manifest limit of 1,000,000 |
| Quarry workers | existing scheduler limit of 64 |
| Quarry render dimension | existing manifest limit of 4,096 |

Recipe, export and topology/evaluator allocation math continues to use checked byte/count helpers where dimensions can multiply. Programmatically constructed oversized recipes are rejected by `validate_recipe` before graph maps/adjacency structures are allocated.

Export filename stems are a restricted portable token and reject path separators, `.`/`..`, absolute/path-like content and traversal. Export sets are created transactionally in same-parent staging and published only after completion. Existing destinations are never silently overwritten.

Quarry manifests/checkpoints/caches keep their earlier bounded-read, identity and checksum contracts. Cache corruption is disposable and never authoritative. Checkpoint/resume failures are explicit; a malformed or mismatched checkpoint is not silently interpreted as another job.

## Recovery and authoritative state

The portable workspace remains authoritative. Caches remain disposable derived data.

The specimen browser now atomically writes `recipes/recovery/current.amr` whenever the selected authoritative recipe changes. On launch without an explicit `--open`, startup order is:

1. a valid recovery snapshot;
2. a persisted favourite;
3. an empty browser.

An explicit `--open` always wins. Recovery is validated as a bounded UTF-8 recipe and must pass normal graph validation. Corrupt recovery is reported and ignored rather than guessed or repaired semantically. The next valid selection can atomically replace it.

Favourites and lineage records/specimens use same-volume temporary files and `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` for durable replacement. Lineage filenames remain fingerprint-bound. Missing ancestry snapshots continue to degrade provenance browsing without changing the valid child recipe.

Quarry cancellation remains at deterministic batch boundaries and resume revalidates manifest/checkpoint identity and contiguous result state. Export interruption cannot publish a partial export as completed output.

## Shutdown and device-loss audit

AM-015 reviewed the UI/model boundaries rather than adding new semantic state to rendering code. The preview remains derived from immutable recipe snapshots. Thumbnail work remains bounded by the fixed worker pool; specimen ordering is independent of completion order. Browser shutdown now joins the pool and drains any already-posted thumbnail completion objects before process exit.

The D3D11 preview remains noncanonical and already supports device recreation/fallback. Present/recovery errors are surfaced in the status line; the canonical CPU evaluator is still the reference result. Playback and Quarry workers retain their existing explicit stop/cancellation ownership and regression coverage.

## Performance methodology

`ArtMinerReleaseBenchmark.exe` is a repeatable throughput characterization, not a deterministic timing test. It reports wall-clock milliseconds for:

- canonical static reference preview;
- a deterministic 16-specimen 96×96 mutation grid render;
- representative reaction-diffusion growth rendering;
- representative particle/feedback rendering at tick 12;
- transactional PNG export;
- eight 96×96 Quarry candidate evaluations.

CI executes the benchmark after all tests. Timings vary with runner load, CPU generation, storage and security software, so they are observations rather than pass/fail thresholds. The first accepted AM-015 CI measurements are recorded below after final-head CI.

### Accepted final-head CI measurements

Pending final-head CI measurement before merge.

## 60 Hz UI responsiveness target

The 60 Hz target applies to interactive UI/preview responsiveness where hardware permits; it is not a promise that canonical CPU evaluation of every recipe completes in 16.67 ms. The browser keeps preview presentation on the UI timer while mutation thumbnails render in a bounded background pool. The status line exposes preview path, presented frame count and last-present milliseconds so a physical Windows system can distinguish GPU presentation latency from canonical CPU generation throughput.

GitHub-hosted Windows CI is useful for correctness and CPU throughput but is not a representative physical-GPU qualification environment. The release therefore does not claim a hardware-specific 60 Hz qualification from CI alone. A narrow post-v1 characterization issue should track measurements on named physical GPUs without changing canonical semantics.

## Release audit disposition

AM-015 audited the requested release surfaces with these dispositions:

| Surface | Disposition |
| --- | --- |
| Recipe parsing / evaluator dispatch | Hardened source/structure limits; unsupported versions remain explicit errors; canonical semantics unchanged. |
| UI/model separation | Recipe remains authoritative; preview/thumbnails derived; recovery contains canonical recipe only. |
| Stateful replay | Existing fixed-tick reset/replay/cache-disposal regressions retained. |
| Quarry resume/cache | Existing checksummed bounded checkpoint/cache contracts retained; cache non-authoritative. |
| Export/provenance | Existing transactional/checksummed provenance retained; traversal regression added. |
| Glyph/ANSI and material workflows | Their recipe CLI loaders now use the common bounded UTF-8 path; existing exact-byte/material tests retained. |
| Topology mutation | Existing deterministic typed limits/locks/provenance tests retained; recipe validator now also supplies outer structural bounds. |
| Persistence/recovery | Atomic favourites/lineage retained and session recovery added. |
| Shutdown/device loss | Thumbnail shutdown leak window closed; D3D loss/fallback remains explicit and noncanonical. |
| Packaging/versioning | 1.0.0 metadata, clean portable package, staged smoke tests and CI artifact added. |

No known release-blocking defect from this audit is intentionally left undocumented. The repository still has an unresolved project-licence decision; AM-015 does not invent or silently choose a licence.

## Verification

Release acceptance requires the strict Windows `/W4 /WX` build and complete CTest suite to pass on the final PR head, followed by successful benchmark and package smoke steps. AM-015 adds regression coverage for malformed UTF-8, raw NUL, oversized source/files, direct parser resource-limit classification, programmatic graph limits, recovery round-trip/corruption handling and export traversal rejection.
