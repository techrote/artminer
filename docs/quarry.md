# Quarry deterministic batch engine (AM-010)

Quarry is ArtMiner's reproducible batch-prospecting engine. A Quarry job is not a UI session: it is a versioned manifest whose result-affecting state is explicit and whose candidates can be regenerated from the manifest alone.

## Job identity and candidate enumeration

`.amq` manifests embed the canonical base `.amr` recipe and record its semantic fingerprint, manifest/enumerator/mutation/metric/evaluator semantic versions, job seed, candidate range, mutation strength, thumbnail/metric render dimensions, animation sampling, and ordered metric selection. The manifest identity is an FNV-1a digest of that semantic payload. Execution policy such as worker count and progress-display timing is deliberately excluded.

Candidate `i` derives its mutation seed only from the manifest root seed, candidate index, and candidate-enumeration contract. It then uses the accepted AM-005 parameter mutation operator. The candidate ID hashes the immutable job identity, absolute candidate index, and resulting recipe fingerprint. Worker scheduling, cache state, completion order, and cancellation timing are therefore not candidate inputs.

Results are committed strictly in ascending candidate-index order. Parallel work happens only inside a bounded batch of at most the requested worker count. Full rendered images exist only inside those active worker evaluations and are discarded after metric extraction. The authoritative population is streamed to the results file rather than retained as a vector of full-resolution images.

## Checkpoint and resume contract

A job uses sibling files derived from the manifest path:

- `<job>.amq.checkpoint` — manifest identity, committed-prefix length, results checksum, completion flag;
- `<job>.amq.results.tsv` — append-only canonical candidate rows in index order;
- `<job>.amq.cache/` — disposable per-candidate metric cache entries for the thumbnail/metric render contract.

Resume verifies the checkpoint identity, completion state, committed count, results header, every committed candidate index/metric field, and an FNV checksum of the complete committed results prefix before any new work begins. A checkpoint without results, results without a checkpoint, a mismatched identity, malformed fields, non-contiguous indices, or checksum disagreement fails with an actionable error. ArtMiner does not guess how to repair an ambiguous prefix.

Cancellation is observed before a new bounded worker batch and after a completed batch. A batch already executing is allowed to finish and is committed in index order before the job stops. This keeps the disk state as a simple contiguous prefix. Resuming with any legal worker count continues at exactly the next uncommitted candidate.

Cache entries include metric semantic version, evaluator version, candidate recipe fingerprint, thumbnail/metric render dimensions, animation sampling, and ordered metric selection. Missing, stale, malformed, or incompatible cache entries are safe misses. Cache write failure does not invalidate a job. Deleting the cache can change runtime only; it cannot change candidate identity or metric values. The rendered image used to extract metrics is bounded by the manifest render dimensions and is not authoritative population state; a future consumer may regenerate that deterministic thumbnail from the same candidate identity if the disposable cache is absent.

## Metric semantics

All metrics consume canonical CPU RGBA8 images. Unless stated otherwise they are normalized to `[0,1]`. Alpha is straight alpha as defined by the canonical renderer. Visible-pixel occupancy uses alpha `>= 8`; transparent pixels below that threshold are empty. Luminance is the deterministic integer approximation `(54R + 183G + 19B + 128) >> 8`, multiplied by alpha and rounded back to 8-bit so fully transparent colour does not influence luminance metrics.

The initial still-image metric set is:

- `entropy` — 256-bin alpha-weighted luminance Shannon entropy divided by 8 bits. Empty or solid images return 0.
- `edge_density` — fraction of horizontal/vertical neighbour pairs whose luminance difference is at least 32. A 1×1 image returns 0.
- `connected_components` — **raw, resolution-dependent** count of four-connected visible-alpha components. Empty images return 0.
- `component_mean` — occupied pixels divided by `(component count × total pixels)`; 0 for no components. This is mean component area normalized by canvas area.
- `component_max` — largest visible component area divided by total pixels.
- `symmetry_bilateral` — 1 minus mean normalized luminance error between left/right mirrored samples. Width 1 is perfectly symmetric.
- `symmetry_rotational` — 1 minus mean normalized luminance error under 180-degree rotation.
- `dominant_frequency` — reciprocal pixel period of the best horizontal/vertical autocorrelation shift among 1..32 pixels; solid/nearly-solid images return 0. This is intentionally resolution dependent because the reported period is in pixels.
- `palette_utilisation` — number of occupied 4-bit-per-channel RGB bins divided by `min(4096, visible pixels)`; empty images return 0.
- `empty_space_ratio` — fraction of pixels with alpha below 8. Opaque black is content, not empty space.
- `repetition` — best normalized luminance similarity over horizontal/vertical shifts 1..32 pixels. Uniform images therefore score 1.
- `tile_seam_error` — mean normalized luminance mismatch between opposite left/right and top/bottom borders. Dimensions of one pixel simply omit that axis.
- `directional_bias` — absolute imbalance between horizontal and vertical threshold-edge densities divided by their sum. No-edge images return 0. The value reports strength, not signed orientation.
- `region_diversity` — standard deviation of populated 4×4 region mean luminances divided by 127.5 and clamped to 1. Tiny images use only regions that contain pixels.

Animation adds:

- `motion_energy` — mean per-pixel absolute luminance difference between consecutive sampled frames, normalized by 255;
- `temporal_flicker` — mean absolute change in whole-frame mean luminance between consecutive samples, normalized by 255. Lower values therefore mean greater global temporal stability.

Animation metrics require at least two frames. The sampling contract is explicit `{first_tick, frame_count, tick_stride}` with at most 16 samples; each frame is reconstructed through the AM-007 canonical fixed-tick evaluator. Sampling is tick-addressed, never wall-clock-addressed. `motion_energy` can be high for spatial movement even if global brightness is stable; `temporal_flicker` isolates that global brightness instability.

Metric semantic version `1` owns all definitions above. A future definition change that can change values requires a new metric semantic version/cache identity rather than silent reinterpretation.

## Commands and UI

The shared engine is exposed through both headless commands and the native window:

```text
ArtMiner quarry create <base.amr> <job.amq> <count> [seed] [width] [height]
    [--metrics name,name,...] [--animation first-tick frame-count tick-stride]
ArtMiner quarry run <job.amq> [workers]
ArtMiner quarry resume <job.amq> [workers]
ArtMiner quarry inspect <job.amq>
ArtMiner quarry ui <job.amq>
```

`create` defaults to the documented still-image metric set, seed `1`, and a 128×128 thumbnail/metric render. `--metrics` records an explicit ordered subset of supported metric names. Animated jobs opt into `motion_energy` and/or `temporal_flicker` and provide at least two fixed-tick samples using `--animation`; the manifest validator rejects animation metrics without sufficient samples. These selections are part of job identity, while worker count is not.

`run` and `resume` intentionally call the same engine: a new job has a zero-length committed prefix, while an existing job is verified and continued. `inspect` validates the manifest/checkpoint/results relationship before reporting progress and the fixed-tick sampling contract. The native Quarry window uses the same engine and exposes Run/Resume, Cancel, Refresh, progress, and raw metric-result browsing; it does not maintain a separate search implementation.

AM-011 owns clustering, deduplication, unusual-distance ranking, and neighbourhood search. AM-010 raw metrics are intentionally inspectable rather than collapsed into a taste/novelty score.
