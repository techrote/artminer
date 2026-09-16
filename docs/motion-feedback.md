# AM-007 fixed-tick motion and feedback

AM-007 adds deterministic particle motion, explicit previous-state feedback, palette cycling, headless frame seeking, and a responsive native playback inspector. These systems deliberately separate **semantic simulation time** from **wall-clock presentation time**.

## Authoritative time model

An animated frame is identified by:

```text
(recipe semantic payload, initial state implied by that recipe, requested fixed tick)
```

Tick `0` is the canonical initial state. Tick `N` is reconstructed by applying exactly `N` ordered fixed updates. Display refresh rate, timer jitter, worker completion order, playback speed, and whether the user reached `N` by Play or by repeated Step commands are not semantic inputs.

AM-006 growth nodes retain their explicit node-local `tick` parameter. AM-007 motion/feedback uses an execution-surface tick instead; the recipe remains the complete initial system/rule description while `render-tick`, `render-range`, and the playback inspector select the observation tick.

## Explicit feedback boundary

Ordinary graph cycles remain invalid. A same-tick dependency edge entering a node classified as `state_boundary` is the sole exception because that edge has different temporal meaning:

- at tick `0`, the boundary emits its declared initial value;
- at tick `N > 0`, the boundary emits its `next` input evaluated at tick `N - 1`;
- the edge into the boundary is therefore excluded from the same-tick dependency graph;
- every other edge is a same-tick dependency and participates in ordinary cycle detection.

`core.state.delay.image` is the AM-007 image-history boundary. Its initial mode is `black`, `white`, or `transparent`. `core.state.delay.scalar` uses zero as its tick-zero initial value in the AM-007 animation evaluator. A feedback loop that does not cross an explicit state boundary is rejected during normal recipe validation.

This is a semantic boundary, not a convenient evaluator loophole. State-boundary behavior is versioned node behavior and deterministic tests protect the contract.

## ParticleSet contract

`core.motion.particles` produces a first-class `ParticleSet`. Each particle has a stable unsigned 64-bit ID, position, velocity, age, lifetime, and bounded trail history.

Canonical ordering is ascending particle ID. The current version has one spawn event at tick zero and no implicit respawn:

- `center`: particles begin in a small deterministic ring around the center;
- `ring`: particles are distributed deterministically around a larger ring;
- `seeded`: position is derived from `(recipe root seed, node identity, particle ID)`.

Each fixed update processes particles in stable ID order. A particle is removed once its age reaches its explicit lifetime. There are no hidden shared random streams: stochastic walker decisions are keyed by `(node seed, tick, particle ID, decision purpose)`, so evaluation order or future worker partitioning cannot change the result.

Supported motion modes are:

- `flow`: deterministic analytic flow-field following;
- `attract`: steer toward the explicit target;
- `repel`: steer away from the explicit target;
- `orbit`: steer tangentially around the target;
- `walker`: seeded per-particle random turning.

`speed`, `turn_strength`, `field_scale`, target coordinates, boundary mode, trail length, count, lifetime, and spawn mode are explicit recipe parameters. Boundary handling is either normalized repeat or clamp. Merge/split behavior is intentionally omitted in this milestone because no equally simple, stable-ID rule was needed to satisfy the core motion capability.

## Deposition and composition

`core.particles.deposit.scalar` converts trails into a deterministic `ScalarField`. `core.particles.deposit.image` provides direct grayscale or heat-map image deposition. Deposition iterates particles in canonical ID order and trail samples newest-to-oldest with explicit radius, intensity, and decay.

`core.image.blend` provides deterministic image history accumulation. Its history weight is quantized to a 1/256 integer blend weight before RGBA8 mixing, avoiding presentation-dependent floating-point blending.

`core.palette.cycle` rotates a palette by an explicit integer rate and phase at the requested fixed tick. This allows otherwise static fields to animate without mutable UI state.

Representative recipes are committed under `examples/`:

- `am007-particle-flow.amr`;
- `am007-particle-orbit.amr`;
- `am007-feedback-trails.amr`;
- `am007-palette-cycle.amr`.

## Canonical renderer and resource bounds

`nodes::render_animation_reference` is the normative AM-007 CPU frame renderer. It validates the same typed recipe graph used elsewhere and evaluates nodes at an explicit integer tick. Its result is an ordinary canonical RGBA8 `Image`, so image fingerprinting and WIC PNG export remain shared with earlier milestones.

Current explicit safety bounds are:

- requested motion tick: at most `16384`;
- feedback-boundary tick: at most `256`;
- animation raster: at most `1,048,576` pixels;
- animation graph: at most `256` nodes;
- particle reconstruction: at most `8,000,000` particle updates.

Exceeding a bound produces a `resource_limit` error instead of attempting an unbounded allocation or loop.

## Snapshot cache

`FrameSnapshotCache` stores a bounded number of completed canonical image frames. Each key includes the AM-007 frame evaluator version marker, semantic recipe fingerprint, output name, and requested tick. A semantic recipe change, output change, or tick change therefore cannot reuse stale pixels.

The cache is explicitly disposable. Removing or clearing it changes performance only; rendering the same recipe and tick without a cache must return identical bytes. The native playback worker owns its cache, which also avoids shared mutable cache state between UI and worker threads.

These are final-frame snapshots, not hidden authoritative simulator objects. The canonical evaluator can always reconstruct a frame from the recipe and tick alone.

## Native playback inspector

Open an AM-007 recipe with:

```bat
ArtMiner.exe animate examples\am007-particle-flow.amr
```

The inspector provides:

- Play/Pause;
- single Step;
- Reset to tick zero;
- current integer tick;
- adjustable `1..60` tick/s preview speed;
- canonical image hash for the most recently completed frame.

Space toggles Play/Pause, Right Arrow performs one Step, and `R` resets.

Canonical frame rendering runs on a dedicated worker thread. The worker keeps only the newest pending tick request, so a slow frame cannot build an unbounded queue and the Win32 message thread remains available for pause/reset/speed input. Playback may visually skip intermediate frames when rendering cannot keep up, but the displayed final tick is still reconstructed canonically; presentation skipping never skips simulation updates inside that reconstruction.

Changing preview speed only changes how quickly wall-clock time advances the requested integer tick. It does not alter particle forces, random decisions, feedback, deposition, or any frame at a given tick.

## Headless frame seeking and ranges

Render one requested tick:

```bat
ArtMiner.exe render-tick examples\am007-particle-flow.amr 120 frame.png
```

Render an inclusive range of up to 256 frames:

```bat
ArtMiner.exe render-range examples\am007-particle-flow.amr 0 120 frames
```

Range files use zero-padded names such as `tick-0000000120.png`. Each PNG receives the normal provenance sidecar plus non-semantic `render.tick` metadata recording the requested observation tick. The semantic recipe fingerprint remains the recipe identity; the tick is a separate animation-state coordinate.

The existing `ArtMiner render` command remains the AM-003/AM-006 static and node-local-tick canonical renderer. AM-007 recipes that use `ParticleSet`, palette cycling, image feedback, or the new delay boundary use `render-tick`/`render-range`/`animate`.

## Determinism tests

`ArtMinerMotionTests` protects:

- legal feedback only through a state boundary and rejection of ordinary cycles;
- ParticleSet stable ordering, deterministic replay, explicit lifetime/death, and every motion mode;
- exact replay/reset at multiple fixed ticks;
- feedback and palette-cycle evolution;
- Play-to-tick versus repeated Step equivalence;
- preview-speed neutrality;
- cache key invalidation and cache-disposal equivalence;
- explicit feedback resource limits.

CTest also renders representative particle and feedback recipes through the real headless `render-tick` executable path.
