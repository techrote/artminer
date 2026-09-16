# ArtMiner AM-009 export formats and provenance

AM-009 makes export a model-facing subsystem rather than a collection of UI-specific writers. The native export window (`ArtMiner export ui <recipe.amr>`) and every headless `ArtMiner export ...` command delegate to the same `export_recipe()` implementation and therefore share rendering, naming, provenance, validation, collision, and publication semantics.

## Transaction and destination contract

An export destination is a **complete asset-set directory**, not a single mutable file. ArtMiner never overwrites an existing destination. It first creates a sibling `<destination>.artminer-part` staging directory, writes and checksums all requested assets there, writes `manifest.artminer.txt` last, and then publishes the complete set with one same-parent filesystem rename. If a normal export step fails, ArtMiner removes the staging directory and does not create the final destination.

A staging directory that already existed before the export started is treated as possible evidence of an interrupted prior operation. ArtMiner refuses to overwrite or silently delete it; the operator must inspect or remove it explicitly. This distinction prevents an interrupted set from being mistaken for a completed export.

Deterministic sequence frame names are `tick-XXXXXXXXXX.<ext>`, where the tick is zero-padded to ten decimal digits. A sprite sheet uses `<stem>-sheet.<ext>` plus `<stem>-atlas.csv`. Output-set manifests contain only relative filenames, so identical deterministic exports do not acquire destination-path-dependent manifest text.

## Canonical raster source

All raster exports derive from the canonical CPU evaluator or canonical fixed-tick animation evaluator. Export does not edit, annotate, or otherwise mutate the source `Recipe`; the source semantic fingerprint is checked before publication.

The canonical in-memory image contract is:

- dimensions: unsigned 32-bit width and height;
- layout: tightly packed row-major pixels, rows top-to-bottom;
- channel order: `R, G, B, A`;
- channel representation: 8-bit unsigned normalized values;
- alpha: straight (unpremultiplied);
- colour interpretation: sRGB-encoded channel values.

### PNG

PNG is encoded through Windows Imaging Component (WIC) from the canonical RGBA8 image using a 32-bit BGRA transport buffer. The image is accompanied by `<image>.artminer.txt`, which records the exact recipe fingerprint, schema/evaluator/exporter versions, dimensions, colour/alpha contract, tick/range where applicable, and the complete canonical recipe text.

The deterministic sidecar is the normative provenance mechanism. Consumers do not need to rely on PNG ancillary-chunk preservation by third-party image tools.

### BMP

BMP is encoded through WIC as deterministic 24-bit BGR transport. Because that representation has no alpha channel, ArtMiner **rejects any source image containing a non-opaque pixel**. It does not flatten, premultiply, discard, or invent a background. Successful BMP sidecars state `alpha opaque-required`.

### Raw RGBA (`.rgba`)

Raw output is the canonical image bytes verbatim: top-to-bottom rows, tightly packed, `R,G,B,A`, one byte per channel, with stride `width * 4`. There is no file header or row padding. Its provenance sidecar explicitly records channel order, row order, stride, dimensions, colour space, alpha semantics, tick/range, and canonical source recipe.

## Animation sequence and sprite-sheet semantics

Frame ranges are inclusive and limited to 256 frames per export. Frames are evaluated by increasing fixed tick using the same canonical `render_animation_reference()` semantics used elsewhere in ArtMiner. Sequence ordering is tick-major and independent of worker/display cadence.

Sprite-sheet input frames must have identical dimensions. They are placed row-major: index `i` is at column `i % columns`, row `i / columns`. Unused cells in the final row remain transparent zero RGBA. `<stem>-atlas.csv` records `tick,x,y,width,height` for every populated cell in increasing tick order.

The sprite-sheet provenance sidecar records the selected tick range. The manifest also records the requested sheet column count and the checksum of the atlas CSV.

## Palette text and CSV

Palette export currently supports palette data whose exact stop values are defined by ArtMiner's implemented palette nodes:

- `core.palette.default` (`mono`, `warm`, `cool`);
- `core.palette.gradient2`.

Plain text writes an ArtMiner header followed by `index r g b a`. CSV writes the stable columns `index,r,g,b,a`. Values use locale-independent round-trip-capable decimal formatting. These are palette stop values in ArtMiner's sRGB-encoded colour convention; export does not resample or quantize them.

## `.cube` restrictions

ArtMiner deliberately does **not** claim that every scalar palette is an RGB colour-grading LUT. A `.cube` 1D LUT independently maps RGB channels, while an arbitrary ArtMiner scalar palette maps one scalar coordinate to an RGBA colour. Treating a coloured scalar palette as a `.cube` would require inventing an RGB-to-scalar interpretation that is not present in the recipe.

For that reason AM-009 emits a 1D `.cube` only when the source palette is losslessly representable under the current contract:

- at least two stops;
- every stop is fully opaque (`a == 1`);
- every stop is channel-neutral (`r == g == b`).

Eligible palettes are written as `LUT_1D_SIZE N` with domain `[0,1]`. Coloured palettes and palettes with alpha are rejected explicitly. No silent approximation, luminance conversion, alpha discard, or 3D-LUT invention is performed.

## Manifest and provenance contract

Every completed export set contains `manifest.artminer.txt`, versioned as `ArtMiner-Export-Manifest 1`. It records:

- exporter semantic version;
- exact source recipe semantic fingerprint;
- recipe schema and evaluator versions;
- export kind and raster format where applicable;
- output dimensions;
- source pixel layout, colour-space and alpha semantics for raster output;
- fixed tick or inclusive tick range where applicable;
- sprite-sheet column count where applicable;
- palette node ID where applicable;
- every produced asset/sidecar relative filename, byte count, and FNV-1a 64 checksum;
- the complete canonical source recipe.

Raster assets additionally have adjacent deterministic provenance sidecars so a copied image/raw frame can still reconstruct its source recipe without depending on the set-level manifest. Palette/LUT files include identifying comments where their format permits them; the set manifest remains the complete provenance record for those exports.

Checksums are integrity/identity aids, not cryptographic authenticity claims.

## Headless commands

```text
ArtMiner export ui <file.amr>
ArtMiner export still <file.amr> <output-dir> <png|bmp|rgba>
ArtMiner export frame <file.amr> <tick> <output-dir> <png|bmp|rgba>
ArtMiner export sequence <file.amr> <start> <end> <output-dir> <png|bmp|rgba>
ArtMiner export sheet <file.amr> <start> <end> <columns> <output-dir> <png|bmp|rgba>
ArtMiner export palette <file.amr> <palette-node> <output-dir> <text|csv|cube>
```

The native export window exposes the same still/frame/sequence/sprite-sheet and palette/LUT request model. It is intentionally a thin control surface: acceptance/rejection and all resulting bytes are determined by the shared export subsystem, not by UI code.
