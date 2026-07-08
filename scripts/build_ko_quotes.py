#!/usr/bin/env python3
"""Build Korean Author Clock data from ko.wikisource public-domain literature.

Absolute rule (see KO_SPEC.md): every emitted quote MUST be a verbatim substring
of the RAW fetched wikisource text. Nothing is hand written, translated, or
paraphrased. Each candidate sentence is verified with
    normalize_ws(sentence) in normalize_ws(raw_source)
and discarded on failure. Only authors dead by 1955 (public domain, 70y) are used.

Outputs (data/):
    ko_quotes.js    window.AUTHOR_CLOCK_QUOTES_KO = {...};
    ko_quotes.json  same object, pure JSON
    ko_coverage.json  provenance + counts + substring_verified flag
Raw sources cached at data/ko_sources/<title>.txt (reused unless --refresh).

Standard library only. Networking via urllib. No subprocess.
"""

import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = REPO_ROOT / "data"
SRC_DIR = DATA_DIR / "ko_sources"
QUOTES_JS = DATA_DIR / "ko_quotes.js"
QUOTES_JSON = DATA_DIR / "ko_quotes.json"
COVERAGE_JSON = DATA_DIR / "ko_coverage.json"

RAW_URL = "https://ko.wikisource.org/w/index.php?title=%s&action=raw"
USER_AGENT = "AuthorClockBot/1.0 (ko.wikisource public-domain corpus; contact rbals1012@gmail.com)"

# Curated public-domain works. (title, author, death_year). death_year must be <= 1955.
# subpage=True works are TOC pages on wikisource; their chapters live on subpages
# and are fetched + concatenated. Titles corrected from live probing.
WORKS = [
    ("운수 좋은 날", "현진건", 1943, False),
    ("빈처", "현진건", 1943, False),
    ("B사감과 러브레터", "현진건", 1943, False),
    ("술 권하는 사회", "현진건", 1943, False),
    ("고향 (현진건)", "현진건", 1943, False),
    ("감자", "김동인", 1951, False),
    ("배따라기", "김동인", 1951, False),
    ("광염 소나타", "김동인", 1951, False),
    ("발가락이 닮았다", "김동인", 1951, False),
    ("물레방아", "나도향", 1926, False),
    ("뽕", "나도향", 1926, False),
    ("날개", "이상", 1937, False),
    ("봄봄", "김유정", 1937, False),
    ("동백꽃", "김유정", 1937, False),
    ("만무방", "김유정", 1937, False),
    ("메밀꽃 필 무렵", "이효석", 1942, False),
    ("홍염", "최서해", 1932, False),
    ("탈출기", "최서해", 1932, False),
    ("레디메이드 인생", "채만식", 1950, False),
    ("경희", "나혜석", 1948, False),
    ("상록수", "심훈", 1936, True),
    ("무정", "이광수", 1950, True),
    ("태평천하", "채만식", 1950, True),
]

PD_CUTOFF_YEAR = 1955

# Display title for entries (strip disambiguation suffix like " (현진건)").
DISAMBIG_RE = re.compile(r"\s*\([^)]*\)\s*$")

REDIRECT_RE = re.compile(r"^\s*#(?:넘겨주기|REDIRECT)\s*\[\[([^\]]+)\]\]", re.IGNORECASE)


def http_get_raw(title):
    """Fetch raw wikitext for a page title. Returns text or None on 404/empty."""
    url = RAW_URL % urllib.parse.quote(title)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=45) as resp:
            data = resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            sys.stdout.write("  [skip] 404 for '%s'\n" % title)
            return None
        raise RuntimeError("HTTP error fetching '%s': %s" % (title, exc)) from exc
    except urllib.error.URLError as exc:
        raise RuntimeError("Network error fetching '%s': %s" % (title, exc)) from exc
    if not data.strip():
        sys.stdout.write("  [skip] empty body for '%s'\n" % title)
        return None
    return data


def resolve_redirect(title, raw):
    """Follow a single #redirect if present."""
    m = REDIRECT_RE.match(raw)
    if not m:
        return raw
    target = m.group(1).split("|")[0].strip()
    sys.stdout.write("  [redirect] '%s' -> '%s'\n" % (title, target))
    resolved = http_get_raw(target)
    return resolved if resolved else raw


SUBPAGE_LINK_RE = re.compile(r"\[\[\s*(/?[^\]|#]+?)\s*(?:\||\]\])")
SKIP_LINK_PREFIX = ("분류:", "파일:", "File:", "Image:", "저자:", "글쓴이:", "위키백과", "s:", "w:")


def find_subpages(title, raw):
    """Extract chapter subpage titles from a TOC page."""
    subs = []
    seen = set()
    for link in SUBPAGE_LINK_RE.findall(raw):
        link = link.strip()
        if any(link.startswith(p) for p in SKIP_LINK_PREFIX):
            continue
        if "/" not in link:
            continue
        if link.startswith("/"):
            resolved = title + "/" + link.strip("/")
        elif link.startswith(title + "/"):
            resolved = link
        else:
            continue
        if resolved not in seen:
            seen.add(resolved)
            subs.append(resolved)
    return subs


def acquire_raw(title, is_subpage_toc, refresh):
    """Return raw source text for a work, using/refreshing the on-disk cache."""
    SRC_DIR.mkdir(parents=True, exist_ok=True)
    cache = SRC_DIR / (title.replace("/", "_") + ".txt")
    if cache.exists() and not refresh:
        sys.stdout.write("  [cache] %s\n" % cache.name)
        return cache.read_text(encoding="utf-8")

    raw = http_get_raw(title)
    if raw is None:
        return None
    raw = resolve_redirect(title, raw)

    if is_subpage_toc:
        subs = find_subpages(title, raw)
        if not subs:
            sys.stdout.write("  [warn] '%s' marked TOC but no subpages found\n" % title)
        parts = []
        for sub in subs:
            body = http_get_raw(sub)
            if body:
                parts.append(body)
        if parts:
            raw = "\n\n".join(parts)
        else:
            sys.stdout.write("  [skip] '%s' TOC yielded no chapter text\n" % title)
            return None

    cache.write_text(raw, encoding="utf-8")
    return raw


# ---- wikitext cleaning ----------------------------------------------------

TEMPLATE_RE = re.compile(r"\{\{[^{}]*\}\}")
TABLE_RE = re.compile(r"\{\|.*?\|\}", re.DOTALL)
COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
REF_PAIR_RE = re.compile(r"<ref[^>]*>.*?</ref>", re.DOTALL | re.IGNORECASE)
REF_SELF_RE = re.compile(r"<ref[^>]*/>", re.IGNORECASE)
TAG_RE = re.compile(r"<[^>]+>")
CAT_LINK_RE = re.compile(r"\[\[(?:분류|파일|File|Image|파일)\s*:[^\]]*\]\]", re.IGNORECASE)
PIPED_LINK_RE = re.compile(r"\[\[[^\]|]*\|([^\]]+)\]\]")
PLAIN_LINK_RE = re.compile(r"\[\[([^\]]+)\]\]")
HEADER_LINE_RE = re.compile(r"^\s*=+\s*.*?\s*=+\s*$", re.MULTILINE)
LIST_MARK_RE = re.compile(r"^[\*#:;]+\s*", re.MULTILINE)
BOLD5_RE = re.compile(r"'{5}")
BOLD3_RE = re.compile(r"'{3}")
ITAL2_RE = re.compile(r"'{2}")
WS_RE = re.compile(r"\s+")


def clean_wikitext(raw):
    """Strip markup, return list of prose paragraphs (whitespace collapsed)."""
    text = COMMENT_RE.sub("", raw)
    text = REF_PAIR_RE.sub("", text)
    text = REF_SELF_RE.sub("", text)
    text = TABLE_RE.sub("", text)
    # Remove nested templates by repeated innermost matching.
    prev = None
    while prev != text:
        prev = text
        text = TEMPLATE_RE.sub("", text)
    text = CAT_LINK_RE.sub("", text)
    text = PIPED_LINK_RE.sub(lambda m: m.group(1), text)
    text = PLAIN_LINK_RE.sub(lambda m: m.group(1), text)
    text = BOLD5_RE.sub("", text)
    text = BOLD3_RE.sub("", text)
    text = ITAL2_RE.sub("", text)
    text = HEADER_LINE_RE.sub("", text)
    text = TAG_RE.sub("", text)
    text = LIST_MARK_RE.sub("", text)

    paragraphs = []
    for block in re.split(r"\n\s*\n", text):
        collapsed = WS_RE.sub(" ", block).strip()
        if collapsed:
            paragraphs.append(collapsed)
    return paragraphs


# ---- sentence splitting ---------------------------------------------------

SENT_SPLIT_RE = re.compile(r'([.!?…]+["\'”’)\]]*|”)')
MIN_LEN = 12
MAX_LEN = 220


def split_sentences(paragraph):
    marked = SENT_SPLIT_RE.sub(lambda m: m.group(1) + "\x00", paragraph)
    out = []
    for chunk in marked.split("\x00"):
        s = chunk.strip()
        if MIN_LEN <= len(s) <= MAX_LEN:
            out.append(s)
    return out


# ---- time expression extraction ------------------------------------------

BUCKET_ORDER = ["자정", "새벽", "아침", "오전", "정오", "오후", "저녁", "밤"]

BUCKET_META = {
    "자정": {"rep": "00:00", "start": "23:30", "end": "00:29"},
    "새벽": {"rep": "04:30", "start": "03:00", "end": "05:59"},
    "아침": {"rep": "07:30", "start": "06:00", "end": "08:59"},
    "오전": {"rep": "10:00", "start": "09:00", "end": "11:29"},
    "정오": {"rep": "12:00", "start": "11:30", "end": "13:29"},
    "오후": {"rep": "15:00", "start": "13:30", "end": "16:59"},
    "저녁": {"rep": "18:30", "start": "17:00", "end": "19:29"},
    "밤": {"rep": "21:30", "start": "19:30", "end": "23:29"},
}

# daypart keyword patterns. 낮/밤 guarded to avoid compound false positives.
DAYPART_PATTERNS = {
    "자정": re.compile(r"자정|한밤중|밤중|한밤"),
    "새벽": re.compile(r"새벽|동트기|동틀|먼동"),
    "아침": re.compile(r"아침|동틀녘|아침나절|조반"),
    "오전": re.compile(r"오전"),
    "정오": re.compile(r"정오|한낮|한나절|대낮|낮(?=에|이|은|을|,|\.|!|\?|\s|$)"),
    "오후": re.compile(r"오후"),
    "저녁": re.compile(r"저녁|해질|해 질|황혼|땅거미|저물|어스름|노을"),
    "밤": re.compile(r"초저녁|야밤|밤(?=에|이|은|을|중|새|하늘|길|,|\.|!|\?|\s|$)"),
}

HOUR_WORDS = [
    ("열두", 12), ("열한", 11), ("열", 10), ("아홉", 9), ("여덟", 8),
    ("일곱", 7), ("여섯", 6), ("다섯", 5), ("네", 4), ("세", 3), ("두", 2), ("한", 1),
]
HOUR_ALT = "|".join(w for w, _ in HOUR_WORDS)
HOUR_TO_NUM = dict(HOUR_WORDS)
# "<word> 시" but not 시간/시절/시집/시골/시내/시기/시험/시국/시장/시월/시계/시위/시비...
HOUR_RE = re.compile(
    r"(" + HOUR_ALT + r")\s*시(?!간|절|집|골|내|기|험|국|장|월|계|위|비|각)(\s*반)?"
)
DIGIT_MIN_RE = re.compile(r"(\d{1,2})\s*분")

AM_CUE_RE = re.compile(r"새벽|아침|오전")
PM_CUE_RE = re.compile(r"오후|저녁|초저녁|밤")


# Tokens where HOUR_RE grabs a spurious "<n> 시": ASAP idiom (한시바삐), Chinese
# poetry (한시/漢詩), verb endings (두시구/두시우), relief (한시름), and word-split
# artifacts (모두 시세, 만한 시금치, 번화한 시가지, 독창이래두 시키세, 시신경,
# 시체). Reject the candidate when one is present so a rebuild does not
# reintroduce the entries cleaned out of the shipped data.
FALSE_POSITIVE_RE = re.compile(
    r"한시바삐|한시름|漢詩|한시\s*한문|한시\s*맛|한시\s*공부|한시와|"
    r"두시구|두시우|알아\s*두시|시금치|시신경|시가지|시체|시세를|시키세|만한\s*시"
)


def extract_precise(sentence):
    """Return (rep_key, t_text, minute, ampm) or None."""
    if FALSE_POSITIVE_RE.search(sentence):
        return None
    m = HOUR_RE.search(sentence)
    if not m:
        return None
    hour = HOUR_TO_NUM[m.group(1)]
    has_ban = m.group(2) is not None
    if has_ban:
        minute = 30
    else:
        dm = DIGIT_MIN_RE.search(sentence)
        minute = int(dm.group(1)) if dm else 0
    if minute > 59:
        minute = 0
    t_text = m.group(0).strip()

    am = bool(AM_CUE_RE.search(sentence))
    pm = bool(PM_CUE_RE.search(sentence))
    if am and not pm:
        ampm = "am"
    elif pm and not am:
        ampm = "pm"
    else:
        ampm = "unknown"

    if ampm == "pm":
        h = 12 if hour == 12 else hour + 12
    else:  # am or unknown use am-style canonical key; frontend matches hour for unknown
        h = 0 if hour == 12 else hour
    key = "%02d:%02d" % (h, minute)
    return key, t_text, minute, ampm


def extract_bucket(sentence):
    """Return (bucket_name, t_text) using KO_SPEC priority, or None.

    Priority: 자정 > 정오 > (새벽/아침/오전/오후/저녁/밤).
    """
    for name in ("자정", "정오"):
        mo = DAYPART_PATTERNS[name].search(sentence)
        if mo:
            return name, mo.group(0)
    for name in ("새벽", "아침", "오전", "오후", "저녁", "밤"):
        mo = DAYPART_PATTERNS[name].search(sentence)
        if mo:
            return name, mo.group(0)
    return None


def normalize_ws(text):
    return WS_RE.sub(" ", text).strip()


def build(refresh):
    SRC_DIR.mkdir(parents=True, exist_ok=True)

    # Public-domain gate.
    for title, author, death, _sub in WORKS:
        if death > PD_CUTOFF_YEAR:
            raise RuntimeError(
                "Work '%s' (%s, d.%d) exceeds PD cutoff %d"
                % (title, author, death, PD_CUTOFF_YEAR)
            )

    precise = {}
    buckets = {name: [] for name in BUCKET_ORDER}
    sources = []
    seen_q = set()
    total_fetch_attempts = 0
    fetch_ok = 0
    failed = []

    for title, author, death, is_toc in WORKS:
        total_fetch_attempts += 1
        sys.stdout.write("Fetching '%s' (%s, d.%d)\n" % (title, author, death))
        raw = acquire_raw(title, is_toc, refresh)
        if raw is None:
            failed.append(title)
            continue
        fetch_ok += 1
        raw_norm = normalize_ws(raw)
        display_title = DISAMBIG_RE.sub("", title)

        extracted = 0
        for para in clean_wikitext(raw):
            for sent in split_sentences(para):
                key = normalize_ws(sent)
                if key in seen_q:
                    continue
                # Absolute rule: verbatim substring of RAW source.
                if key not in raw_norm:
                    continue

                pr = extract_precise(sent)
                if pr is not None:
                    rep_key, t_text, _minute, ampm = pr
                    precise.setdefault(rep_key, []).append({
                        "t": t_text, "q": sent, "title": display_title,
                        "author": author, "ampm": ampm,
                    })
                    seen_q.add(key)
                    extracted += 1
                    continue

                bk = extract_bucket(sent)
                if bk is not None:
                    name, t_text = bk
                    buckets[name].append({
                        "t": t_text, "q": sent, "title": display_title,
                        "author": author,
                    })
                    seen_q.add(key)
                    extracted += 1

        sources.append({
            "title": display_title, "author": author, "death_year": death,
            "page": title, "url": RAW_URL % urllib.parse.quote(title),
            "fetched_chars": len(raw), "extracted": extracted,
        })
        sys.stdout.write("  extracted %d quotes (raw %d chars)\n" % (extracted, len(raw)))

    if fetch_ok == 0:
        raise RuntimeError("All fetches failed; refusing to write empty output.")

    total_quotes = sum(len(v) for v in precise.values()) + sum(
        len(v) for v in buckets.values()
    )
    if total_quotes == 0:
        raise RuntimeError("Zero quotes extracted; refusing to write empty output.")

    # Internal substring re-verification (should be all-pass by construction).
    verify_fail = 0
    raw_cache = {}
    for src in sources:
        cache = SRC_DIR / (src["page"].replace("/", "_") + ".txt")
        raw_cache[src["author"] + "\x00" + src["title"]] = normalize_ws(
            cache.read_text(encoding="utf-8")
        )
    # Cross-check every emitted quote against at least one source of same author.
    author_raw = {}
    for src in sources:
        author_raw.setdefault(src["author"], []).append(
            normalize_ws((SRC_DIR / (src["page"].replace("/", "_") + ".txt")).read_text(encoding="utf-8"))
        )
    for group in (precise, buckets):
        for entries in group.values():
            for e in entries:
                q = normalize_ws(e["q"])
                if not any(q in blob for blob in author_raw.get(e["author"], [])):
                    verify_fail += 1
    if verify_fail:
        raise RuntimeError("%d quotes failed substring re-verification" % verify_fail)

    precise_sorted = {
        k: precise[k] for k in sorted(precise)
    }
    buckets_out = {name: buckets[name] for name in BUCKET_ORDER}

    obj = {
        "precise": precise_sorted,
        "buckets": buckets_out,
        "bucketMeta": BUCKET_META,
    }

    DATA_DIR.mkdir(parents=True, exist_ok=True)
    QUOTES_JSON.write_text(
        json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    QUOTES_JS.write_text(
        "window.AUTHOR_CLOCK_QUOTES_KO = %s;\n"
        % json.dumps(obj, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    coverage = {
        "sources": sources,
        "precise_keys": sorted(precise_sorted),
        "bucket_counts": {name: len(buckets[name]) for name in BUCKET_ORDER},
        "total_quotes": total_quotes,
        "substring_verified": True,
    }
    COVERAGE_JSON.write_text(
        json.dumps(coverage, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    return obj, coverage, failed


def main():
    refresh = "--refresh" in sys.argv[1:]
    obj, coverage, failed = build(refresh)

    sys.stdout.write("\n==== summary ====\n")
    sys.stdout.write("Wrote %s\n" % QUOTES_JS)
    sys.stdout.write("Wrote %s\n" % QUOTES_JSON)
    sys.stdout.write("Wrote %s\n" % COVERAGE_JSON)
    sys.stdout.write("Sources OK: %d, failed: %s\n" % (len(coverage["sources"]), failed or "none"))
    sys.stdout.write("Total quotes: %d\n" % coverage["total_quotes"])
    sys.stdout.write("Precise keys (%d): %s\n" % (
        len(coverage["precise_keys"]), coverage["precise_keys"]))
    sys.stdout.write("Bucket counts: %s\n" % coverage["bucket_counts"])


if __name__ == "__main__":
    main()
