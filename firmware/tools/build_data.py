#!/usr/bin/env python3
"""Build embedded quote data + glyph set for the ESP32 author-clock firmware.

Reads the web app dataset (../../data/ko_quotes.js) and emits:
  ../data/quotes_min.json  minute-keyed, exact-time quotes only (daypart buckets dropped)
  ../data/glyphs.txt       every unique character used, for font subsetting

Exact-time only: the firmware shows a quote whose stated time equals the clock,
so daypart-bucket quotes (approximate time) are intentionally excluded.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "data", "ko_quotes.js")
OUT_JSON = os.path.join(HERE, "..", "data", "quotes_min.json")
OUT_GLYPHS = os.path.join(HERE, "..", "data", "glyphs.txt")

# UI strings the firmware renders, so their glyphs are in the subset too.
UI_TEXT = "오전오후시분작가시계역원문저장공유설정밝기글자크기자동밝게어둡게거치모드전체화면0123456789:· ()\"'…%°C.됨"


def load_web_dataset(path):
    raw = open(path, encoding="utf-8").read()
    body = re.sub(r"^window\.AUTHOR_CLOCK_QUOTES_KO\s*=\s*", "", raw).strip().rstrip(";")
    return json.loads(body)


def main():
    data = load_web_dataset(SRC)
    precise = data["precise"]

    out = {}
    entries = 0
    for key in sorted(precise):
        arr = []
        for e in precise[key]:
            arr.append({
                "t": e["t"],
                "q": e["q"],
                "a": e.get("title", ""),
                "w": e.get("author", ""),
                "k": 1 if e.get("kind") == "역" else 0,
            })
            entries += 1
        out[key] = arr

    json.dump(out, open(OUT_JSON, "w", encoding="utf-8"),
              ensure_ascii=False, separators=(",", ":"))

    chars = set(UI_TEXT)
    for arr in out.values():
        for e in arr:
            for field in ("t", "q", "a", "w"):
                chars.update(e[field])
    chars.discard("\n")

    open(OUT_GLYPHS, "w", encoding="utf-8").write("".join(sorted(chars)))

    hangul = [c for c in chars if "가" <= c <= "힣"]
    size_kb = os.path.getsize(OUT_JSON) / 1024
    sys.stdout.write("minutes=%d entries=%d quotes_min.json=%.1fKB glyphs=%d (hangul=%d)\n"
                     % (len(out), entries, size_kb, len(chars), len(hangul)))


if __name__ == "__main__":
    main()
