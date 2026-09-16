# Glyph / ANSI synthesis contract (AM-012)

AM-012 turns a canonical ArtMiner `Image` output into a structural glyph grid and then serializes that grid either as plain UTF-8 text or UTF-8 plus ANSI SGR colour sequences. The authoritative result is the ordered glyph-cell sequence and its serialized bytes. A GUI font is never an evaluator input and rendered font pixels are never canonical output.

## Semantic recipe settings

A recipe opts into glyph synthesis by carrying a disconnected `core.glyph.settings` node. It is disconnected deliberately: normal image evaluation still owns the source image, while the settings node records output-stage semantics in the ordinary recipe model. Consequently every glyph setting participates automatically in semantic fingerprinting, mutation, parameter/group locks, two-parent crossover, lineage replay, recipe diff, saved recipes and export provenance.

The node has these explicit parameters:

| Parameter | Domain | Meaning |
| --- | --- | --- |
| `cell_width` | integer 1..128 | source pixels sampled per glyph cell horizontally |
| `cell_height` | integer 1..128 | source pixels sampled per glyph cell vertically |
| `glyph_set` | `ascii-density`, `blocks`, `lines`, `sparkle` | built-in canonical prototype set |
| `choice_mode` | `density`, `structure`, `balanced` | which descriptor terms participate in glyph scoring |
| `density_weight` | 0..8 | luminance/density mismatch weight |
| `orientation_weight` | 0..8 | edge-orientation mismatch weight |
| `detail_weight` | 0..8 | local spatial-detail mismatch weight |
| `corner_weight` | 0..8 | corner/curvature tendency mismatch weight |
| `motion_weight` | 0..8 | supplied motion-vector direction mismatch weight |
| `colour_mode` | `monochrome`, `limited`, `ansi16`, `ansi256`, `truecolor` | terminal foreground quantization |
| `limited_palette` | `mono4`, `warm8`, `cool8` | ANSI16 subset used by `limited` mode |
| `background_mode` | `black`, `terminal-default` | explicit black SGR background or no background SGR |
| `alpha_mode` | `composite-black`, `ignore` | whether source RGB is composited against black before descriptors/colour averaging |

All parameters are mutable. `glyph.layout`, `glyph.mapping` and `glyph.colour` are the lock/crossover groups.

## Grid sampling and resource bound

Cells are row-major from the top-left source pixel. Grid dimensions are `ceil(width / cell_width)` by `ceil(height / cell_height)`. Partial cells on the right and bottom sample only pixels that actually exist; there is no implicit padding. The canonical glyph stage rejects grids above 262,144 cells.

The source must be tightly packed row-major RGBA8. With `composite-black`, each RGB byte is composited using integer `(channel * alpha + 127) / 255`; `ignore` leaves RGB unchanged. Luminance is derived from composited/ignored bytes using `(54 R + 183 G + 19 B) / 65280`, so the byte-to-luminance mapping is explicit.

## Structural descriptors

Each cell records more than mean luminance:

- `luminance` / `density`: mean source luminance;
- `gradient_x`, `gradient_y`: mean central-difference luminance gradient using clamped source-image edges;
- `edge_strength`: magnitude of the mean gradient;
- `edge_orientation`: **line tangent** orientation, modulo 180 degrees (a left-to-right luminance edge therefore describes a vertical line);
- `detail`: mean absolute right/down neighbour luminance difference, a local spatial-frequency proxy;
- `corner`: deterministic combination of two-axis gradient energy and local detail;
- `motion_strength`, `motion_direction`: mean supplied motion vector, when a caller has an explicit vector source.

The public synthesis API accepts one optional motion vector per source pixel. Recipe-driven image export currently supplies image descriptors only; no vector field is fabricated from animation frames. This preserves the rule that ArtMiner must not invent state or motion semantics that the evaluator did not provide.

## Built-in glyph sets

The built-in sets use semantic prototype descriptors, **not measured font raster density**. This is what makes selection independent of installed fonts.

- `ascii-density`: space plus `. : - = + * # % @`; `-`/`=` also carry horizontal orientation prototypes.
- `blocks`: space plus `░ ▒ ▓ █`.
- `lines`: space, middle dot, `─ │ ╲ ╱`, box corners, `┼`, and full block. This is the primary direction-sensitive structural set.
- `sparkle`: space plus `. · + * ✧ ✦`.

Weighted selection minimizes a deterministic score. `density` mode disables structural terms; `structure` reduces density influence to one quarter while preserving configured structural weights; `balanced` uses all configured terms. Prototype array order is the final tie-break because only a strictly lower score replaces the current winner.

`examples/am012-glyph-x-lines.amr` and `examples/am012-glyph-y-lines.amr` intentionally have the same luminance distribution and glyph settings but orthogonal gradients. The first prefers vertical line structure while the second prefers horizontal line structure. They are a compact demonstration that AM-012 is not a brightness-only ASCII mapper.

## Colour semantics

Foreground colour is the rounded mean source RGB for the cell after the selected alpha rule.

`monochrome` uses ANSI16 bright white (index 15). `limited` selects the nearest member of one fixed ANSI16 subset: `mono4 = {0,8,7,15}`, `warm8 = {0,1,9,3,11,5,13,15}`, `cool8 = {0,4,12,6,14,2,10,15}`. `ansi16` uses the standard 16-colour RGB table. `ansi256` searches the standard xterm 0..15 colours, 6×6×6 cube (16..231, levels 0/95/135/175/215/255), and grayscale ramp (232..255, 8 + 10n). `truecolor` emits the source mean RGB directly.

Quantization distance is squared Euclidean distance in the declared sRGB-encoded byte space. Equal-distance ties always choose the numerically lowest palette index. This includes duplicate exact colours in the 16-colour and 256-colour spaces; for example pure bright red selects index 9 rather than the later equal RGB cube entry.

With `background_mode=black`, ANSI16/limited/monochrome use SGR background 40, ANSI256 uses `48;5;0`, and truecolour uses `48;2;0;0;0`. With `terminal-default`, no background SGR is emitted. ArtMiner never assumes what a terminal's default background colour is.

## Canonical byte serialization

Plain output is UTF-8 without BOM. Every grid row is followed by LF, including the final row. No platform newline conversion is permitted.

ANSI output is also UTF-8 without BOM and LF-only. Each cell receives a complete deterministic foreground (and, when requested, background) SGR prefix; each row ends with `ESC[0m` and LF. Per-cell prefixes are intentionally redundant: the byte contract is simple, stateless within the row, and stable even if a previous cell changes colour.

Invalid Unicode scalar values, surrogate codepoints, C0/C1 controls and Unicode noncharacters are rejected as unsupported glyphs. In particular a cell cannot smuggle CR, LF or escape into the canonical payload.

## Native preview is not canonical

`ArtMiner export glyph-preview <recipe> <settings-node> [tick]` opens a native Win32 preview of the canonical UTF-8 sequence. It requests a fixed-pitch Windows font for convenience, but installed font coverage, fallback, hinting, ClearType, glyph metrics and rasterization can vary by machine. The window labels these effects as preview-only. Exported text/ANSI bytes and the `GlyphGrid` are authoritative.

This separation is intentional: a visually different fallback glyph on another PC does not change a recipe fingerprint, glyph choice, export bytes, metric identity or lineage.

## Export commands

Static plain/ANSI output:

```text
ArtMiner export glyph <file.amr> <glyph-settings-node> <output-dir> <text|ansi>
```

One fixed-tick frame:

```text
ArtMiner export glyph-frame <file.amr> <glyph-settings-node> <tick> <output-dir> <text|ansi>
```

Inclusive fixed-tick sequence, up to the shared AM-009 limit of 256 frames:

```text
ArtMiner export glyph-sequence <file.amr> <glyph-settings-node> <start> <end> <output-dir> <text|ansi>
```

The glyph exporter shares the AM-009 transactional contract: a same-parent `.artminer-part` directory is built first, existing final/staging paths are never overwritten, failed newly-created staging state is cleaned, and a completed set is published with one rename. Every asset has `ArtMiner-Provenance 2`; every set has `ArtMiner-Export-Manifest 1`; both embed the canonical source recipe/fingerprint plus grid, mapping, colour, tick and byte-format semantics. Checksums are FNV-1a64 like the existing exporter.

Sequence files are `frames/tick-##########.txt` or `.ans` in ascending inclusive tick order. Fixed-tick rendering is reconstructed through the AM-007 canonical animation evaluator, so display cadence is not an input and reset/replay produces byte-identical cell output for the same recipe and tick.
