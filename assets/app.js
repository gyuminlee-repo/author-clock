/* Author Clock (Korean edition) - client logic. No dependencies, no network calls. */
(function (global) {
  "use strict";

  /** Escape HTML special chars. */
  function escapeHtml(str) {
    return String(str)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  /**
   * Render a quote object into safe HTML: escape everything, restore <br/>
   * line breaks, then wrap the first case-insensitive match of `t` inside
   * `q` with <strong class="tp">.
   */
  function renderQuoteHtml(q, t) {
    var escaped = escapeHtml(q);
    // restore line breaks: escapeHtml turned "<br/>" / "<br>" into
    // "&lt;br/&gt;" / "&lt;br&gt;"
    escaped = escaped.replace(/&lt;br\s*\/?&gt;/gi, "<br>");

    if (!t) return escaped;

    var escapedT = escapeHtml(t);
    var idx = escaped.toLowerCase().indexOf(escapedT.toLowerCase());
    if (idx === -1) return escaped;

    var before = escaped.slice(0, idx);
    var match = escaped.slice(idx, idx + escapedT.length);
    var after = escaped.slice(idx + escapedT.length);
    return before + '<strong class="tp">' + match + "</strong>" + after;
  }

  /** Zero-pad current time as "HH:MM" (24h, used as data lookup key). */
  function formatHHMM(date) {
    var h = String(date.getHours()).padStart(2, "0");
    var m = String(date.getMinutes()).padStart(2, "0");
    return h + ":" + m;
  }

  /** Display format: "오후 3:07" (12h, Korean am/pm prefix). */
  function formatKoreanClock(date) {
    var h = date.getHours();
    var m = String(date.getMinutes()).padStart(2, "0");
    var period = h < 12 ? "오전" : "오후";
    var h12 = h % 12;
    if (h12 === 0) h12 = 12;
    return period + " " + h12 + ":" + m;
  }

  function pickRandom(list) {
    return list[Math.floor(Math.random() * list.length)];
  }

  function minutesOf(hhmm) {
    var parts = hhmm.split(":");
    return parseInt(parts[0], 10) * 60 + parseInt(parts[1], 10);
  }

  /** Current am/pm ("am"|"pm") for a given HH:MM key. */
  function ampmOf(hhmm) {
    return minutesOf(hhmm) < 12 * 60 ? "am" : "pm";
  }

  /**
   * Length (in minutes) of a bucket range, honoring wrap-around
   * (start > end means the range crosses midnight).
   */
  function bucketLength(meta) {
    var start = minutesOf(meta.start);
    var end = minutesOf(meta.end);
    if (start <= end) return end - start;
    return 1440 - start + end;
  }

  /** Whether `nowMinutes` falls inside a (possibly wrap-around) bucket range. */
  function bucketContains(meta, nowMinutes) {
    var start = minutesOf(meta.start);
    var end = minutesOf(meta.end);
    if (start <= end) return nowMinutes >= start && nowMinutes <= end;
    return nowMinutes >= start || nowMinutes <= end;
  }

  /** Find the narrowest bucket whose range contains `hhmm`, or null. */
  function findBucketForTime(bucketMeta, hhmm) {
    if (!bucketMeta) return null;
    var nowMinutes = minutesOf(hhmm);
    var best = null;
    var bestLen = Infinity;
    for (var name in bucketMeta) {
      if (!Object.prototype.hasOwnProperty.call(bucketMeta, name)) continue;
      var meta = bucketMeta[name];
      if (!meta || !bucketContains(meta, nowMinutes)) continue;
      var len = bucketLength(meta);
      if (len < bestLen) {
        bestLen = len;
        best = name;
      }
    }
    return best;
  }

  /**
   * Pick one Korean quote for the given "HH:MM" time, following the
   * precise-match-then-bucket-fallback algorithm. Never throws; returns
   * { quote: <item>|null, message: <string>|null }.
   */
  function pickKoQuote(data, hhmm, preferOriginal) {
    try {
      if (!data) return { quote: null, message: "아직 문장이 없습니다." };

      var precise = data.precise || {};
      var buckets = data.buckets || {};
      var bucketMeta = data.bucketMeta || {};

      // 1. precise minute match, filtered by am/pm.
      var preciseList = precise[hhmm];
      if (Array.isArray(preciseList) && preciseList.length > 0) {
        var currentAmpm = ampmOf(hhmm);
        var filtered = preciseList.filter(function (item) {
          return item.ampm === "unknown" || item.ampm === currentAmpm;
        });
        var pool = filtered.length > 0 ? filtered : preciseList;

        if (preferOriginal) {
          var originals = pool.filter(function (item) {
            return item.kind === "원문";
          });
          if (originals.length > 0) pool = originals;
        }

        return { quote: pickRandom(pool), message: null };
      }

      // 2. bucket match via bucketMeta, narrowest range wins.
      var bucketName = findBucketForTime(bucketMeta, hhmm);
      if (bucketName && Array.isArray(buckets[bucketName]) && buckets[bucketName].length > 0) {
        return { quote: pickRandom(buckets[bucketName]), message: null };
      }

      // 3. adjacent/any non-empty bucket.
      for (var name in buckets) {
        if (!Object.prototype.hasOwnProperty.call(buckets, name)) continue;
        if (Array.isArray(buckets[name]) && buckets[name].length > 0) {
          return { quote: pickRandom(buckets[name]), message: null };
        }
      }

      // 4. nothing available at all.
      return { quote: null, message: "아직 문장이 없습니다." };
    } catch (e) {
      return { quote: null, message: "아직 문장이 없습니다." };
    }
  }

  var core = {
    escapeHtml: escapeHtml,
    renderQuoteHtml: renderQuoteHtml,
    formatHHMM: formatHHMM,
    formatKoreanClock: formatKoreanClock,
    pickRandom: pickRandom,
    pickKoQuote: pickKoQuote,
  };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = core;
  }

  // Browser-only wiring below. Skipped entirely under Node (no `document`).
  if (typeof document === "undefined") {
    return;
  }

  var THEME_KEY = "authorClockTheme"; // "auto" | "light" | "dark"

  var quoteEl = document.getElementById("quote");
  var sourceEl = document.getElementById("source");
  var digitalClockEl = document.getElementById("digital-clock");
  var themeToggleEl = document.getElementById("theme-toggle");
  var stageEl = document.getElementById("stage");

  var state = {
    currentQuote: null,
    lastPool: [],
  };

  function getData() {
    return global.AUTHOR_CLOCK_QUOTES_KO || null;
  }

  function applyTheme(mode) {
    var root = document.documentElement;
    root.classList.remove("theme-light", "theme-dark");
    if (mode === "light") root.classList.add("theme-light");
    if (mode === "dark") root.classList.add("theme-dark");
    // "auto" leaves both off; CSS prefers-color-scheme takes over.
    if (themeToggleEl) {
      var label = mode === "auto" ? "자동" : mode === "light" ? "밝게" : "어둡게";
      themeToggleEl.textContent = label;
      themeToggleEl.setAttribute("aria-label", "테마: " + label);
    }
  }

  function cycleTheme() {
    var current = localStorage.getItem(THEME_KEY) || "auto";
    var next = current === "auto" ? "light" : current === "light" ? "dark" : "auto";
    localStorage.setItem(THEME_KEY, next);
    applyTheme(next);
  }

  function renderCurrentQuote(message) {
    if (!state.currentQuote) {
      quoteEl.textContent = message || "아직 문장이 없습니다.";
      sourceEl.textContent = "";
      return;
    }
    var item = state.currentQuote;
    quoteEl.innerHTML = renderQuoteHtml(item.q, item.t);
    var title = item.title || "";
    var author = item.author || "";
    var sourceHtml;
    if (title && author) {
      sourceHtml = escapeHtml(title) + " · " + escapeHtml(author);
    } else {
      sourceHtml = escapeHtml(title || author);
    }
    if (item.kind === "역" || item.kind === "창작") {
      sourceHtml +=
        ' <span class="badge-trans" title="' +
        escapeHtml(item.kind) +
        '" aria-label="' +
        escapeHtml(item.kind) +
        '">' +
        escapeHtml(item.kind) +
        "</span>";
    }
    sourceEl.innerHTML = sourceHtml;
  }

  function fadeSwap(fn) {
    if (!stageEl) {
      fn();
      return;
    }
    stageEl.classList.add("fade-out");
    setTimeout(function () {
      fn();
      stageEl.classList.remove("fade-out");
    }, 220);
  }

  function loadForNow() {
    var data = getData();
    var now = new Date();
    var hhmm = formatHHMM(now);
    if (digitalClockEl) digitalClockEl.textContent = formatKoreanClock(now);

    var result = pickKoQuote(data, hhmm, true);
    state.currentQuote = result.quote;
    renderCurrentQuote(result.message);
  }

  function shuffleQuote() {
    var data = getData();
    var now = new Date();
    var hhmm = formatHHMM(now);
    if (digitalClockEl) digitalClockEl.textContent = formatKoreanClock(now);

    var result = pickKoQuote(data, hhmm, false);
    state.currentQuote = result.quote;
    renderCurrentQuote(result.message);
  }

  function scheduleNextTick() {
    var now = new Date();
    var msToNextMinute =
      (60 - now.getSeconds()) * 1000 - now.getMilliseconds();
    setTimeout(function () {
      fadeSwap(loadForNow);
      scheduleNextTick();
    }, msToNextMinute);
  }

  function init() {
    var savedTheme = localStorage.getItem(THEME_KEY) || "auto";
    applyTheme(savedTheme);

    if (themeToggleEl) {
      themeToggleEl.addEventListener("click", cycleTheme);
    }

    document.addEventListener("click", function (e) {
      if (themeToggleEl && themeToggleEl.contains(e.target)) return;
      fadeSwap(shuffleQuote);
    });

    document.addEventListener("keydown", function (e) {
      if (e.code === "Space" || e.key === " ") {
        e.preventDefault();
        fadeSwap(shuffleQuote);
      }
    });

    loadForNow();
    scheduleNextTick();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})(typeof window !== "undefined" ? window : this);
