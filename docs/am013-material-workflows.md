# AM-013 material, seam and loop contracts

AM-013 extends the canonical CPU workflow with explicit material semantics, quantitative seam inspection and loop validation. It does **not** infer physical meaning from ordinary colour images and it does **not** claim arbitrary stateful simulations are perfect loops.

## Seamless and periodic evaluation

ArtMiner distinguishes *periodic addressing* from a *verified discrete seamless result*.

| Primitive / operation | Periodic or wrap behaviour | Relationship to output dimensions | Seam guarantee |
| --- | --- | --- | --- |
| `core.scalar.transform.symmetry` (`x`, `y`, `xy`) | Mirrors the source coordinate about the selected axis/axes. | The mirrored domain spans the complete rendered tile. With the canonical pixel-centre sampler, corresponding opposite edge texels map to the same source coordinate on each selected axis. | Exact opposite-edge identity on selected axes, subject to deterministic downstream operations preserving equality. |
| `core.scalar.transform.repeat` (`x_count`, `y_count`) | Repeats the source mapping `x_count` and `y_count` times across the unit output domain. | One repeated cell occupies `output_width / x_count` by `output_height / y_count` in continuous coordinates. Integer output dimensions are not required to divide evenly. | Periodic continuous addressing, but not by itself a promise that the first and last *sampled texel centres* are equal. Validate the actual raster with the seam metric. |
| `core.scalar.warp` with `wrap=repeat` | Toroidal addressing for source samples displaced outside the unit domain. | Wrap period is one source image/domain extent. | Prevents clamp boundaries during sampling; does not make a non-periodic source seamless. |
| `core.loop.wave_image` | Analytic integer-cycle spatial wave driven by `core.loop.phase`. | `spatial_cycles` is the number of full cycles between the first and last texel centres on the selected axis. The endpoint-inclusive coordinate convention makes opposite edges phase-identical for integer cycles. | Exact edge identity for the generated wave image on its selected axis. |
| Growth/motion systems with repeat-style simulation boundaries | Toroidal simulation topology where supported by that system. | Period is the logical simulation extent, not automatically the rendered edge sample interval. | No automatic seamless-texture or perfect-loop claim. Quantitative validation is still required. |

The representative `examples/am013-seamless-texture.amr` uses `core.scalar.transform.symmetry mode xy`, so both pairs of output edges are identical under canonical sampling.

### Seam metric and inspection

AM-013 reuses the AM-010 `tile_seam_error` metric as the authoritative combined score. It does not maintain a second combined seam algorithm. `quarry::compute_tile_seam_diagnostics()` additionally reports horizontal and vertical components using the same alpha-weighted integer luminance convention, and `quarry::make_tile_seam_inspection()` builds a deterministic 2x2 repeat view whose central joins are the measured wrap boundaries.

Headless checks:

```text
ArtMiner material seam <file.amr> [output-name] [threshold]
```

The default threshold is `0.02`. Exit status is non-zero when the combined error exceeds the selected threshold. The native inspection view is available with:

```text
ArtMiner material ui <file.amr> [output-name]
```

It displays horizontal, vertical and combined errors over a 2x2 inspection view. `+`/`-` adjust the inspection threshold, `R` rerenders canonically and `Esc` closes the view.

## Explicit height semantics

The graph carrier type remains `ScalarField`, but AM-013's workflow evaluator tracks a stricter semantic `HeightField` subtype internally. This is deliberate: existing graph and recipe compatibility is retained while physical height meaning still requires an explicit conversion operation.

`core.material.height_from_image` is the only AM-013 image-to-height conversion. Its visible recipe parameters are:

- `channel`: `luminance`, `r`, `g`, `b` or `a`;
- `minimum`, `maximum`: explicit source range remapping into normalized height;
- `invert`: `no` or `yes`.

No ordinary RGB or generic scalar field is silently interpreted as physical height. `core.material.height_image` and `core.material.normal_from_height` reject a graph value that is type-compatible at the generic `ScalarField` carrier level but did not acquire height semantics through the explicit conversion node.

This means a recipe can deliberately choose luminance as a height source, but that choice is a serialized parameter rather than an implicit convention.

## Deterministic height-to-normal conversion

`core.material.normal_from_height` consumes semantic height and exposes all conversion assumptions:

- `filter`: `central` or `sobel`;
- `edge`: `clamp` or `repeat`;
- `strength`: height-gradient multiplier;
- `texel_scale`: spatial scale divisor, so effective slope scale is `strength / texel_scale`;
- `handedness`: `right` or `left`, controlling the X slope sign;
- `convention`: `opengl` or `directx`, controlling the encoded Y sign.

For central differences, `dx=(h(x+1)-h(x-1))/2` and `dy=(h(y+1)-h(y-1))/2`. Sobel uses the normalized 3x3 Sobel derivative. The unnormalized right-handed OpenGL normal is `(-dx*s, -dy*s, 1)`, with the documented handedness/convention sign changes applied before normalization. The normalized XYZ vector is encoded into RGB as `0.5*n + 0.5`, with opaque alpha. `edge=repeat` wraps the height samples and `edge=clamp` clamps them.

`examples/am013-height-normal.amr` demonstrates the explicit image -> height -> normal path and also exposes a rendered height output.

## Masks and channel packing

`core.material.mask_from_image` performs an explicit image-channel threshold conversion with serialized `channel`, `threshold` and `invert` parameters. `core.material.mask_image` renders a semantic mask as neutral black/white RGBA. `core.material.pack_masks_rgba` accepts four explicit mask inputs named `r`, `g`, `b` and `a`; it does not infer channel roles from node names or image appearance.

The export manifest records each packed mapping as `material-map <channel> <source-node.port>`. The complete canonical recipe is also embedded, retaining the mask thresholds, source-channel choices and evaluator version. `examples/am013-packed-masks.amr` demonstrates four independent semantic masks packed into RGBA.

## Fixed-tick loop primitives and validation

`core.loop.phase` is an analytic fixed-tick phase primitive. Its value is:

```text
phase = frac((tick mod loop_length) / loop_length + phase_offset)
```

It depends only on the integer canonical tick. Display cadence and wall-clock time cannot change it. A requested validation length must be an integer multiple of every reachable `core.loop.phase` native `loop_length`.

`core.loop.wave_image` consumes that phase and provides an elementary deterministic looping image source. `examples/am013-validated-loop.amr` uses a native period of 32 ticks.

Loop validation is not based only on seeing similar first/last images. For requested length `L`, ArtMiner renders and compares:

- endpoint state/image continuity: tick `0` vs tick `L`;
- boundary transition continuity: transition `0 -> 1` vs transition `L -> L+1`.

The default tolerance is `1/255`. Both measured errors must be within tolerance. In addition, every reachable stateful node must have an AM-013 closure proof. At present only analytic `core.loop.phase` has such a proof. Existing accumulated simulations, particles, feedback and state-boundary graphs can be exported as ordinary sequences, but **cannot be labelled validated perfect loops** merely because the exported frames are repeated or happen to have similar endpoints.

Headless validation:

```text
ArtMiner material loop <file.amr> <loop-length> [output-name] [tolerance]
```

It reports endpoint error, transition error, whether reachable state exists, and the reason for acceptance/rejection. A failed or unproved loop exits non-zero.

## Common export integration and provenance

Material and loop exports use the AM-009 transactional export subsystem, including collision protection, same-parent staging, deterministic file ordering, checksums and embedded canonical recipe provenance.

A specific material output can be exported with:

```text
ArtMiner material export <file.amr> <output-name> <output-dir> <png|bmp|rgba>
```

A proven loop can be exported as the canonical tick range `0..L-1` with:

```text
ArtMiner material loop-sequence <file.amr> <loop-length> <output-dir> <png|bmp|rgba> [output-name]
```

`loop-sequence` requires loop validation to pass before publishing the export set. The manifest and per-raster sidecars include the selected output name, semantic role, explicit material mappings/settings, loop length, endpoint and transition errors, tolerance, validation result/reason, recipe fingerprint, schema/evaluator versions and full canonical recipe. Sequence filenames retain the AM-009 tick-major deterministic ordering.

## Scope limits

AM-013 intentionally does not implement a PBR viewport, automatic channel semantics, an arbitrary-state loop solver or automatic material-role inference. Those behaviours would require stronger contracts than this milestone can truthfully provide. The AM-013 rule is conservative: conversions are explicit, diagnostics are quantitative, and a result is called seamless or perfectly looping only when the relevant documented checks pass.
