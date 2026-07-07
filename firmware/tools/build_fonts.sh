#!/usr/bin/env bash
# Build the subset Korean fonts + large digit font as LVGL C fonts for the
# author-clock firmware (ST7305 mono 1bpp panel).
# Requires: curl, python3, pyftsubset (fonttools), node + npx (lv_font_conv).
# Run build_data.py first so ../data/glyphs.txt exists.
# Copy the generated font_*.c into ../main/ before building.
set -euo pipefail
cd "$(dirname "$0")"

# Pretendard is SIL OFL 1.1 (redistributable), same license family as the
# previous Nanum Myeongjo. Downloaded as a release zip; the regular static TTF
# is extracted from it.
FONT_ZIP_URL="https://github.com/orioncactus/pretendard/releases/download/v1.3.9/Pretendard-1.3.9.zip"
FONT_ZIP="Pretendard-1.3.9.zip"
SRC_TTF="Pretendard-Regular.ttf"        # SIL OFL, redistributable
SUBSET_TTF="Pretendard-subset.ttf"
GLYPHS="../data/glyphs.txt"

# Fetch + extract the regular static TTF from the release zip. The in-zip path
# is discovered at runtime instead of hardcoded.
if [ ! -f "$SRC_TTF" ]; then
  [ -f "$FONT_ZIP" ] || curl -sL -o "$FONT_ZIP" "$FONT_ZIP_URL"
  python3 -c "
import zipfile, shutil
z = zipfile.ZipFile('$FONT_ZIP')
cands = [n for n in z.namelist() if n.endswith('Pretendard-Regular.ttf')]
assert cands, 'Pretendard-Regular.ttf not found in $FONT_ZIP'
with z.open(cands[0]) as s, open('$SRC_TTF', 'wb') as d:
    shutil.copyfileobj(s, d)
print('extracted', cands[0])
"
fi

pyftsubset "$SRC_TTF" --text-file="$GLYPHS" --output-file="$SUBSET_TTF" \
  --layout-features='' --no-hinting --desubroutinize

# non-ASCII glyphs (hangul + specials), whitespace stripped, for lv_font_conv --symbols
python3 -c "
s=open('$GLYPHS',encoding='utf-8').read()
non=sorted({c for c in s if ord(c)>=128 and not c.isspace()})
open('hangul_symbols.txt','w',encoding='utf-8').write(''.join(non))
"

# 1bpp on the mono reflective panel (no grayscale). ASCII via --range,
# hangul/specials via --symbols.
npx --yes lv_font_conv --font "$SUBSET_TTF" --size 28 --bpp 1 --format lvgl \
  --range 0x20-0x7E --symbols "$(cat hangul_symbols.txt)" -o font_ko_28.c
npx --yes lv_font_conv --font "$SUBSET_TTF" --size 44 --bpp 1 --format lvgl \
  --range 0x20-0x7E --symbols "$(cat hangul_symbols.txt)" -o font_ko_44.c

# Large clock digits: only 0-9 and ':' (0x30-0x3A) -> a few KB.
npx --yes lv_font_conv --font "$SUBSET_TTF" --size 96 --bpp 1 --format lvgl \
  --range 0x30-0x3A -o font_digits_96.c

echo "done: $SUBSET_TTF, font_ko_28.c, font_ko_44.c, font_digits_96.c"
echo "copy the font_*.c files into ../main/ before running pio run"
