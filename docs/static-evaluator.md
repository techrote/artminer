# AM-003 canonical static evaluator

AM-003 introduces the first **canonical CPU/reference renderer**. This path is normative for schema-1 static-art regression fixtures and reference PNG export. GPU preview arrives later and may only claim documented equivalence, not bit identity.

## Raster and colour contract

A recipe's `render` record gives the exact raster size. Pixel `(x,y)` samples normalized coordinates at the pixel centre:

```text
u = (x + 0.5) / width
v = (y + 0.5) / height
```

The origin is at the top-left: `u` increases rightward and `v` increases downward. Scalar/vector/colour intermediates are evaluated as finite C++ `double` values under the project's precise floating-point build settings. Operations that produce display colour clamp components to `[0,1]` before conversion to RGBA8. Conversion uses nearest integer via `floor(component * 255 + 0.5)`. Alpha is straight (not premultiplied).

Schema/evaluator version 1 treats colour components as normalized display-encoded channel values; it does not insert an implicit transfer-function conversion. WIC writes the resulting RGBA8 channels to PNG. A later change to these semantics requires explicit evaluator/node versioning.

Reference rendering rejects more than 4,194,304 pixels, more than 1,024 graph nodes, or a conservative estimated intermediate working set above 768 MiB. These renderer limits are intentionally tighter than schema validation's general dimension envelope so a syntactically valid recipe cannot trigger an unreasonable canonical allocation.

## Seed contract

Random-looking static nodes never consume shared RNG state. Each node receives a purpose-local seed:

```text
identity = node-id + "\\n" + node-type-id
node-seed = derive_seed(recipe-root-seed, FNV1a64(identity))
```

fBm derives each octave seed from that node seed. Lattice/cellular samples use SplitMix64-based coordinate hashing. Evaluation order therefore cannot alter a node's random field.

## Implemented CPU node families

AM-003 marks CPU capability only for implemented nodes. The first reference set contains:

- scalar constants/pass-through and normalized X/Y coordinates;
- radial and angular fields;
- deterministic value noise, gradient noise, Worley/cellular distance and fBm;
- scalar domain warp with explicit clamp/repeat sampling;
- circle and box signed-distance fields plus scalar minimum/maximum composition;
- threshold and quantisation;
- scalar repetition and mirror symmetry transforms;
- scalar-to-vector and scalar-to-colour composition;
- colour channel/luminance extraction;
- scalar masks;
- built-in palettes and explicit two-colour gradients;
- scalar-to-palette colour mapping;
- scalar/colour final image conversion;
- Bayer 4x4/8x8 ordered colour dithering.

Particle and state-boundary metadata remain intentionally non-evaluable until their fixed-tick milestones. A recipe that reaches a node with no canonical CPU evaluator fails explicitly rather than changing meaning.

## Headless render and provenance

After a Release build:

```bat
ArtMiner.exe render examples\am003-fbm-warp.amr output\fbm-warp.png
```

The renderer validates the recipe, evaluates output `main`, writes PNG through Windows Imaging Component, and creates a deterministic adjacent sidecar:

```text
output\fbm-warp.png
output\fbm-warp.png.artminer.txt
```

The sidecar records the semantic recipe fingerprint, schema/evaluator versions, raster dimensions, pixel format, and complete canonical recipe. This is the AM-003 provenance baseline. The richer common export/manifest subsystem remains scheduled for AM-009.

## Regression policy

Small exact RGBA8 fixtures protect simple node/raster semantics. Seeded procedural nodes are additionally tested for replay identity and root-seed sensitivity. PNG tests verify the PNG signature and the reconstruction sidecar. A canonical golden must not be changed merely to make a failing test pass; intentional semantic changes require versioning and documentation.
