# Asset library

## Provenance

These files came from the course's **Assignment 3 asset library**, supplied with
the starting code for the assignment. They are **third-party game art used for
coursework** and are not original work of this project.

This directory is a **verbatim copy** of that library, committed into this
repository so the engine has a local, version-controlled asset source. The copy
is byte-for-byte identical to the original: nothing was renamed, resized,
recompressed, converted or otherwise altered, and the original directory
structure and filenames are preserved.

Source of the copy:

```
/Users/nurulhasan/Developer/Game Programming/Assignment3-main/bin/images
  -> assets/library/images
/Users/nurulhasan/Developer/Game Programming/Assignment3-main/bin/fonts
  -> assets/library/fonts
```

The only file intentionally **not** copied is the macOS Finder metadata file
`.DS_Store`, which is present in the source `images/` directory. It is not an
asset and is already excluded by the repository's `.gitignore`.

## Contents

| | Count |
| --- | --- |
| PNG images | **24** |
| TTF fonts | **3** |
| Total size | ~456 KB |

Images are organised into three subdirectories:

| Directory | Files | Contents |
| --- | --- | --- |
| `images/animations/` | 3 | animation sprite sheets |
| `images/mario/` | 15 | tiles, pipes, scenery, enemies |
| `images/megaman/` | 6 | player character sheets and a projectile |

> **Note on the expected count:** the import task specified 23 PNGs. The source
> library actually contains **24** (`3 + 15 + 6`). The count was re-derived by
> inspecting the source directory rather than assumed. All 24 files were copied
> unaltered; none was dropped to reach 23, because removing a file would have
> meant modifying the library.

## PNG inventory

Dimensions and colour type were read directly from each file's PNG **IHDR**
chunk (width at byte 16, height at byte 20, bit depth at byte 24, colour type at
byte 25) and cross-checked against `sips` and `file(1)`. PNG colour type `3`
means an indexed/palette image; `6` means truecolour with alpha (RGBA).

| Path | Dimensions | Bit depth | Colour type |
| --- | --- | --- | --- |
| `animations/coinspin.png` | 500x280 | 8 | RGBA |
| `animations/explosion.png` | 1152x96 | 8 | **INDEXED / COLORMAP** |
| `animations/QuestionBlock.png` | 64x16 | 8 | RGBA |
| `mario/BigBush.png` | 186x79 | 8 | RGBA |
| `mario/BigCloud.png` | 200x97 | 8 | RGBA |
| `mario/Busher.png` | 130x51 | 8 | RGBA |
| `mario/cloudsmall.png` | 117x69 | 8 | RGBA |
| `mario/coin.png` | 500x299 | 8 | RGBA |
| `mario/Flagpole.png` | 75x528 | 4 | **INDEXED / COLORMAP** |
| `mario/GoombaDeath.png` | 60x26 | 8 | RGBA |
| `mario/GoombaWalk.png` | 100x41 | 8 | RGBA |
| `mario/ground.png` | 64x64 | 8 | RGBA |
| `mario/question.png` | 360x360 | 4 | **INDEXED / COLORMAP** |
| `mario/question2.png` | 160x160 | 8 | RGBA |
| `mario/SmallBush.png` | 150x68 | 8 | RGBA |
| `mario/SmallCloud.png` | 70x51 | 8 | RGBA |
| `mario/SmallPipe.png` | 70x70 | 8 | RGBA |
| `mario/TallPipe.png` | 500x464 | 8 | RGBA |
| `megaman/megaBuster.png` | 32x26 | 8 | RGBA |
| `megaman/megaJump.png` | 279x266 | 8 | RGBA |
| `megaman/megaRun.png` | 733x246 | 8 | RGBA |
| `megaman/megaShot.png` | 253x222 | 8 | RGBA |
| `megaman/megaSlit.png` | 226x218 | 8 | RGBA |
| `megaman/megaStand.png` | 190x208 | 8 | RGBA |

### Indexed / colormap PNGs

Three files are palette-indexed rather than RGBA:

| File | Bit depth | Dimensions |
| --- | --- | --- |
| `animations/explosion.png` | 8 | 1152x96 |
| `mario/Flagpole.png` | 4 | 75x528 |
| `mario/question.png` | 4 | 360x360 |

These are recorded here because they are the one place in this library that could
plausibly have needed special handling, and it is worth knowing they were checked.

**They do not need any.** An earlier version of this file claimed that a
`copyToImage()` step was generally required for a palette-indexed PNG, and that
was wrong. SFML 2.6.2 decodes all three correctly through
`sf::Texture::loadFromFile`, because the image loader underneath it (stb_image) is
asked for four channels and resolves the palette itself.

This was verified rather than assumed. Each file was decoded independently to
RGBA and compared pixel by pixel against the texture the loader produces, at
several coordinates including partly transparent and fully transparent pixels. All
three matched exactly. For example `mario/question.png` at (180,180) is
`(227,53,0,255)` from both, which is a decoder that read the palette correctly
rather than one that mistook palette indices for colour channels.

The loader therefore loads these files the same way as every other image. The
pixel comparison is kept as a test (`assets.loading`), so a future graphics
library that changed this behaviour would fail there rather than being noticed
as wrong colours on screen.

**Nothing in this directory is loaded, read or rendered by the engine yet.** It
remains data.

## Font inventory

| File | Size | Family | Glyphs | Outlines |
| --- | --- | --- | --- | --- |
| `fonts/numbers.ttf` | 7,780 bytes | see note below | 97 | `glyf` (TrueType) |
| `fonts/pixeled.ttf` | 38,540 bytes | `Pixeled` | 314 | `glyf` (TrueType) |
| `fonts/tech.ttf` | 20,532 bytes | `FAST-TRACK` | 108 | `glyf` (TrueType) |

All three are valid TrueType outlines (`glyf` table present), not CFF/OTF.

**Note on `numbers.ttf`:** its `name` table holds two conflicting `nameID=1`
(family) records — the Macintosh record says `New` and the Windows/en-US record
says `Secret Code`. Tools disagree on which one they report, so neither value is
authoritative here. This is a property of the upstream font, not of the copy.

## Verifying this copy

To confirm the committed files still match the original library, run from the
repository root:

```bash
SRC="/Users/nurulhasan/Developer/Game Programming/Assignment3-main/bin"
diff -r --exclude='.DS_Store' "$SRC/images" assets/library/images
diff -r --exclude='.DS_Store' "$SRC/fonts"  assets/library/fonts
```

Both commands should print nothing and exit `0`.
