#!/usr/bin/env bash
# Build the subset Korean fonts + large digit font as LVGL C fonts for the
# author-clock firmware (ST7305 mono 1bpp panel).
# Requires: curl, python3, pyftsubset (fonttools), node + npx (lv_font_conv).
# Run build_data.py first so ../data/glyphs.txt exists.
# Copy the generated font_*.c into ../main/ before building.
set -euo pipefail
cd "$(dirname "$0")"

# BookkMyungjo (Bukk Myeongjo), a Korean Myeongjo serif by BOOKK, free for
# personal and commercial use. The vendor page is JS-gated, so this pulls the
# TTF from the community fonts-archive mirror. NOTE: unlike the SIL OFL fonts
# used before (Pretendard, Nanum Myeongjo), BookkMyungjo grants commercial
# embedding but not explicit modification; the subset + bitmap conversion below
# is treated as personal-device use. Confirm the license before redistributing.
FONT_TTF_URL="https://raw.githubusercontent.com/fonts-archive/BookkMyungjo/main/BookkMyungjo-Bold.ttf"
SRC_TTF="BookkMyungjo-Bold.ttf"
SUBSET_TTF="BookkMyungjo-subset.ttf"
GLYPHS="../data/glyphs.txt"

[ -f "$SRC_TTF" ] || curl -sL -o "$SRC_TTF" "$FONT_TTF_URL"

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
# Smaller quote-body sizes for the clock-screen auto-fit ladder (28 -> 22 -> 18).
npx --yes lv_font_conv --font "$SUBSET_TTF" --size 22 --bpp 1 --format lvgl \
  --range 0x20-0x7E --symbols "$(cat hangul_symbols.txt)" -o font_ko_22.c
npx --yes lv_font_conv --font "$SUBSET_TTF" --size 18 --bpp 1 --format lvgl \
  --range 0x20-0x7E --symbols "$(cat hangul_symbols.txt)" -o font_ko_18.c
npx --yes lv_font_conv --font "$SUBSET_TTF" --size 44 --bpp 1 --format lvgl \
  --range 0x20-0x7E --symbols "$(cat hangul_symbols.txt)" -o font_ko_44.c

# Large clock digits: only 0-9 and ':' (0x30-0x3A) -> a few KB.
npx --yes lv_font_conv --font "$SUBSET_TTF" --size 96 --bpp 1 --format lvgl \
  --range 0x30-0x3A -o font_digits_96.c

echo "done: $SUBSET_TTF, font_ko_28.c, font_ko_22.c, font_ko_18.c, font_ko_44.c, font_digits_96.c"
echo "copy the font_*.c files into ../main/ before running pio run"
