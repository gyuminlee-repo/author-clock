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
   * Filter a precise[hhmm] list down to items matching the current am/pm,
   * falling back to the full list if the filter would empty it out.
   * Shared by pickKoQuote and buildShufflePool so the am/pm rule lives
   * in one place.
   */
  function filteredPrecise(preciseList, hhmm) {
    var currentAmpm = ampmOf(hhmm);
    var filtered = preciseList.filter(function (item) {
      return item.ampm === "unknown" || item.ampm === currentAmpm;
    });
    return filtered.length > 0 ? filtered : preciseList;
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
        var pool = filteredPrecise(preciseList, hhmm);

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

  /**
   * Build the shuffle candidate pool for a given "HH:MM" time: precise[hhmm]
   * items only (am/pm filtered, no preferOriginal bias). The quote's stated
   * time must match the actual clock time, so daypart buckets are never
   * mixed in here (that would show a quote timestamped for a different
   * minute). If this minute has no precise entries at all, the caller falls
   * back to pickKoQuote's own adjacent-bucket/any-bucket path. Never throws;
   * returns an array (possibly empty).
   */
  function buildShufflePool(data, hhmm) {
    if (!data) return [];
    var precise = data.precise || {};

    var preciseList = precise[hhmm];
    if (Array.isArray(preciseList) && preciseList.length > 0) {
      return filteredPrecise(preciseList, hhmm);
    }

    return [];
  }

  /** Stable signature for a quote item, used for recent-shown tracking. */
  function quoteSignature(item) {
    var title = item.title || "";
    var q = String(item.q || "").slice(0, 30);
    return title + "|" + q;
  }

  /**
   * Pick one item from `pool` avoiding signatures already present in the
   * `recentSigs` Set. If the whole pool is already covered, the set is
   * cleared and a fresh pick is made from the full pool. Mutates
   * `recentSigs` in place (adds the chosen signature, trims oldest entries
   * beyond `maxRecent`). Returns null if pool is empty.
   */
  function pickShuffle(pool, recentSigs, maxRecent) {
    if (!Array.isArray(pool) || pool.length === 0) return null;

    var available = pool.filter(function (item) {
      return !recentSigs.has(quoteSignature(item));
    });
    if (available.length === 0) {
      recentSigs.clear();
      available = pool;
    }

    var chosen = pickRandom(available);
    recentSigs.add(quoteSignature(chosen));
    while (recentSigs.size > maxRecent) {
      recentSigs.delete(recentSigs.values().next().value);
    }
    return chosen;
  }

  /**
   * Pure favorites-toggle helper: given the current saved-favorites list and
   * a quote item, returns a new list with the item added (if not already
   * present, by quoteSignature) or removed (if already present). Never
   * mutates `list`. Used by both the browser UI and node tests.
   */
  function toggleFavoriteList(list, item) {
    var sig = quoteSignature(item);
    var idx = -1;
    for (var i = 0; i < list.length; i++) {
      if (quoteSignature(list[i]) === sig) {
        idx = i;
        break;
      }
    }
    var next = list.slice();
    if (idx !== -1) {
      next.splice(idx, 1);
    } else {
      next.push({
        t: item.t,
        q: item.q,
        title: item.title,
        author: item.author,
        kind: item.kind,
        time: item.time,
        savedAt: Date.now(),
      });
    }
    return next;
  }

  var core = {
    escapeHtml: escapeHtml,
    renderQuoteHtml: renderQuoteHtml,
    formatHHMM: formatHHMM,
    formatKoreanClock: formatKoreanClock,
    pickRandom: pickRandom,
    findBucketForTime: findBucketForTime,
    filteredPrecise: filteredPrecise,
    pickKoQuote: pickKoQuote,
    buildShufflePool: buildShufflePool,
    quoteSignature: quoteSignature,
    pickShuffle: pickShuffle,
    toggleFavoriteList: toggleFavoriteList,
  };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = core;
  }

  // Browser-only wiring below. Skipped entirely under Node (no `document`).
  if (typeof document === "undefined") {
    return;
  }

  var THEME_KEY = "authorClockTheme"; // "auto" | "light" | "dark"
  var DOCK_MODE_KEY = "authorClockDockMode"; // "on" | "off"
  var MANUAL_DIM_KEY = "authorClockManualDim"; // "0".."0.8"
  var FONT_SCALE_KEY = "authorClockFontScale"; // "small" | "normal" | "large"
  var FAVORITES_KEY = "authorClockFavorites"; // JSON array of saved quote items
  var CONTROLS_IDLE_MS = 3500;
  var SHUFFLE_RECENT_MAX = 30;
  var FONT_SCALE_MAP = { small: 0.85, normal: 1, large: 1.2 };
  var CARD_FONT_STACK = '"AppleMyungjo","Apple Myungjo","Batang","Nanum Myeongjo",serif';
  var CARD_GOLD = "#c8963e";

  var quoteEl = document.getElementById("quote");
  var sourceEl = document.getElementById("source");
  var digitalClockEl = document.getElementById("digital-clock");
  var bigClockEl = document.getElementById("big-clock");
  var bigClockTimeEl = bigClockEl ? bigClockEl.querySelector(".big-time") : null;
  var bigClockAmpmEl = bigClockEl ? bigClockEl.querySelector(".big-ampm") : null;
  var themeToggleEl = document.getElementById("theme-toggle");
  var stageEl = document.getElementById("stage");
  var controlsEl = document.getElementById("controls");
  var dockToggleEl = document.getElementById("dock-toggle");
  var fullscreenToggleEl = document.getElementById("fullscreen-toggle");
  var nightDimEl = document.getElementById("night-dim");
  var settingsToggleEl = document.getElementById("settings-toggle");
  var settingsPanelEl = document.getElementById("settings-panel");
  var dimSliderEl = document.getElementById("dim-slider");
  var saveToggleEl = document.getElementById("save-toggle");
  var shareToggleEl = document.getElementById("share-toggle");
  var favoritesListEl = document.getElementById("favorites-list");
  var fontScaleBtns = settingsPanelEl
    ? Array.prototype.slice.call(settingsPanelEl.querySelectorAll(".font-scale-btn"))
    : [];

  var wakeLockSentinel = null;
  var controlsHideTimer = null;

  var state = {
    currentQuote: null,
    recentSigs: new Set(),
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
    updateSaveButtonState();
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

  /** Read the saved-favorites array from localStorage. Never throws. */
  function loadFavoritesRaw() {
    try {
      var raw = localStorage.getItem(FAVORITES_KEY);
      var parsed = raw ? JSON.parse(raw) : [];
      return Array.isArray(parsed) ? parsed : [];
    } catch (e) {
      return [];
    }
  }

  function saveFavoritesRaw(list) {
    localStorage.setItem(FAVORITES_KEY, JSON.stringify(list));
  }

  function isCurrentFavorited() {
    if (!state.currentQuote) return false;
    var list = loadFavoritesRaw();
    var sig = quoteSignature(state.currentQuote);
    return list.some(function (item) {
      return quoteSignature(item) === sig;
    });
  }

  function updateSaveButtonState() {
    if (!saveToggleEl) return;
    var fav = isCurrentFavorited();
    saveToggleEl.setAttribute("aria-pressed", fav ? "true" : "false");
    saveToggleEl.classList.toggle("is-active", fav);
    saveToggleEl.textContent = fav ? "♥" : "♡";
    saveToggleEl.setAttribute("aria-label", fav ? "저장 해제" : "문장 저장");
  }

  function toggleFavorite() {
    if (!state.currentQuote) return;
    var list = toggleFavoriteList(loadFavoritesRaw(), state.currentQuote);
    saveFavoritesRaw(list);
    updateSaveButtonState();
    renderFavoritesList();
  }

  /** Render the "저장한 문장" list inside the settings panel, newest first. */
  function renderFavoritesList() {
    if (!favoritesListEl) return;
    var list = loadFavoritesRaw();
    if (list.length === 0) {
      favoritesListEl.innerHTML = '<p class="favorites-empty">저장한 문장이 없습니다.</p>';
      return;
    }
    var html = "";
    for (var i = list.length - 1; i >= 0; i--) {
      var item = list[i];
      var title = item.title || "";
      var author = item.author || "";
      var label = title && author ? title + " · " + author : title || author || "";
      var plainQ = String(item.q || "").replace(/<br\s*\/?>/gi, " ");
      var preview = plainQ.slice(0, 28);
      if (plainQ.length > 28) preview += "…";
      var lineText = preview + (label ? " · " + label : "");
      html +=
        '<div class="favorite-item-row">' +
        '<button type="button" class="favorite-item" data-index="' +
        i +
        '" title="' +
        escapeHtml(lineText) +
        '">' +
        escapeHtml(lineText) +
        "</button>" +
        '<button type="button" class="favorite-delete" data-index="' +
        i +
        '" aria-label="삭제">×</button>' +
        "</div>";
    }
    favoritesListEl.innerHTML = html;
  }

  function setupFavoritesList() {
    if (!favoritesListEl) return;
    favoritesListEl.addEventListener("click", function (e) {
      var delBtn = e.target.closest(".favorite-delete");
      if (delBtn) {
        var delIdx = parseInt(delBtn.getAttribute("data-index"), 10);
        var list = loadFavoritesRaw();
        if (delIdx >= 0 && delIdx < list.length) {
          list.splice(delIdx, 1);
          saveFavoritesRaw(list);
          renderFavoritesList();
          updateSaveButtonState();
        }
        return;
      }
      var itemBtn = e.target.closest(".favorite-item");
      if (itemBtn) {
        var idx = parseInt(itemBtn.getAttribute("data-index"), 10);
        var chosen = loadFavoritesRaw()[idx];
        if (chosen) {
          state.currentQuote = chosen;
          renderCurrentQuote(null);
        }
      }
    });
  }

  function isDarkTheme() {
    var root = document.documentElement;
    if (root.classList.contains("theme-dark")) return true;
    if (root.classList.contains("theme-light")) return false;
    return !!(window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches);
  }

  /**
   * Split a quote's raw text (which may contain literal "<br>" line breaks,
   * same convention as renderQuoteHtml) into a flat token stream of
   * {type:"break"} and {type:"char", ch, hl} entries, marking the first
   * case-insensitive match of `t` as highlighted.
   */
  function buildQuoteRuns(q, t) {
    var raw = String(q || "");
    var runs;
    if (t) {
      var lower = raw.toLowerCase();
      var idx = lower.indexOf(String(t).toLowerCase());
      if (idx !== -1) {
        runs = [
          { text: raw.slice(0, idx), hl: false },
          { text: raw.slice(idx, idx + t.length), hl: true },
          { text: raw.slice(idx + t.length), hl: false },
        ];
      } else {
        runs = [{ text: raw, hl: false }];
      }
    } else {
      runs = [{ text: raw, hl: false }];
    }

    var tokens = [];
    runs.forEach(function (run) {
      var parts = run.text.split(/<br\s*\/?>/gi);
      parts.forEach(function (part, i) {
        if (i > 0) tokens.push({ type: "break" });
        for (var j = 0; j < part.length; j++) {
          tokens.push({ type: "char", ch: part[j], hl: run.hl });
        }
      });
    });
    return tokens;
  }

  /** Word/char-level wrap of `tokens` to fit `maxWidth`, using canvas metrics. */
  function wrapCardTokens(ctx, tokens, maxWidth, fontNormal, fontItalic) {
    var lines = [];
    var current = [];
    var currentWidth = 0;
    tokens.forEach(function (tok) {
      if (tok.type === "break") {
        lines.push(current);
        current = [];
        currentWidth = 0;
        return;
      }
      if (tok.ch === " " && current.length === 0) return; // no leading space
      ctx.font = tok.hl ? fontItalic : fontNormal;
      var w = ctx.measureText(tok.ch).width;
      if (currentWidth + w > maxWidth && current.length > 0) {
        lines.push(current);
        current = [];
        currentWidth = 0;
        if (tok.ch === " ") return;
      }
      current.push({ ch: tok.ch, hl: tok.hl, w: w });
      currentWidth += w;
    });
    lines.push(current);
    return lines;
  }

  /** Find the largest font size (down to a floor) whose wrapped lines fit maxHeight. */
  function fitCardText(ctx, tokens, maxWidth, maxHeight) {
    var fontSize = 64;
    var minFontSize = 30;
    var lineHeightRatio = 1.5;
    var fontNormal, fontItalic, lines, lineHeight;
    while (fontSize >= minFontSize) {
      fontNormal = "400 " + fontSize + "px " + CARD_FONT_STACK;
      fontItalic = "italic 400 " + fontSize + "px " + CARD_FONT_STACK;
      lines = wrapCardTokens(ctx, tokens, maxWidth, fontNormal, fontItalic);
      lineHeight = fontSize * lineHeightRatio;
      if (lines.length * lineHeight <= maxHeight) {
        return { lines: lines, lineHeight: lineHeight, fontNormal: fontNormal, fontItalic: fontItalic };
      }
      fontSize -= 4;
    }
    fontNormal = "400 " + minFontSize + "px " + CARD_FONT_STACK;
    fontItalic = "italic 400 " + minFontSize + "px " + CARD_FONT_STACK;
    lines = wrapCardTokens(ctx, tokens, maxWidth, fontNormal, fontItalic);
    lineHeight = minFontSize * lineHeightRatio;
    return { lines: lines, lineHeight: lineHeight, fontNormal: fontNormal, fontItalic: fontItalic };
  }

  /** Draw a 1080x1350 share card for `item` and return the canvas. */
  function drawShareCard(item, dark) {
    var canvas = document.createElement("canvas");
    canvas.width = 1080;
    canvas.height = 1350;
    var ctx = canvas.getContext("2d");

    var bg = dark ? "#1b1a17" : "#f6efe1";
    var fg = dark ? "#ece5d6" : "#2c241b";
    var muted = dark ? "#a89d89" : "#7a6f5d";

    ctx.fillStyle = bg;
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    var marginX = 90;
    var topY = 170;
    var bottomReserved = 230;
    var maxWidth = canvas.width - marginX * 2;
    var maxHeight = canvas.height - topY - bottomReserved;

    var tokens = buildQuoteRuns(item.q, item.t);
    var fit = fitCardText(ctx, tokens, maxWidth, maxHeight);
    var totalTextHeight = fit.lines.length * fit.lineHeight;
    var startY = topY + Math.max(0, (maxHeight - totalTextHeight) / 2) + fit.lineHeight * 0.72;

    ctx.textBaseline = "alphabetic";
    fit.lines.forEach(function (line, li) {
      var lineWidth = line.reduce(function (sum, c) {
        return sum + c.w;
      }, 0);
      var x = (canvas.width - lineWidth) / 2;
      var y = startY + li * fit.lineHeight;
      line.forEach(function (c) {
        ctx.font = c.hl ? fit.fontItalic : fit.fontNormal;
        ctx.fillStyle = c.hl ? CARD_GOLD : fg;
        ctx.fillText(c.ch, x, y);
        x += c.w;
      });
    });

    var title = item.title || "";
    var author = item.author || "";
    var sourceText = title && author ? title + " · " + author : title || author || "";
    if (item.kind === "역") sourceText += " · 역";

    ctx.textAlign = "center";
    ctx.fillStyle = muted;
    ctx.font = "400 32px " + CARD_FONT_STACK;
    ctx.fillText(sourceText, canvas.width / 2, canvas.height - 150);

    ctx.strokeStyle = dark ? "rgba(236,229,214,0.18)" : "rgba(44,36,27,0.18)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(canvas.width / 2 - 60, canvas.height - 190);
    ctx.lineTo(canvas.width / 2 + 60, canvas.height - 190);
    ctx.stroke();

    ctx.globalAlpha = 0.75;
    ctx.font = "300 24px " + CARD_FONT_STACK;
    ctx.fillText("작가시계", canvas.width / 2, canvas.height - 90);
    ctx.globalAlpha = 1;
    ctx.textAlign = "left";

    return canvas;
  }

  function canvasToBlob(canvas) {
    return new Promise(function (resolve, reject) {
      canvas.toBlob(function (blob) {
        if (blob) resolve(blob);
        else reject(new Error("toBlob failed"));
      }, "image/png");
    });
  }

  function downloadBlob(blob) {
    var url = URL.createObjectURL(blob);
    var a = document.createElement("a");
    a.href = url;
    a.download = "작가시계.png";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(function () {
      URL.revokeObjectURL(url);
    }, 1000);
  }

  /** Share `blob` via the Web Share API if a file share is supported, else download it. */
  function deliverShareBlob(blob) {
    var file = null;
    try {
      file = new File([blob], "작가시계.png", { type: "image/png" });
    } catch (e) {
      file = null;
    }
    if (file && navigator.canShare && navigator.share && navigator.canShare({ files: [file] })) {
      return navigator.share({ files: [file], title: "작가시계" }).catch(function () {
        downloadBlob(blob);
      });
    }
    downloadBlob(blob);
    return Promise.resolve();
  }

  function shareCurrentQuote() {
    if (!state.currentQuote) return;
    var canvas = drawShareCard(state.currentQuote, isDarkTheme());
    canvasToBlob(canvas)
      .then(deliverShareBlob)
      .catch(function () {});
  }

  /**
   * Fill in the landscape big clock (#big-clock) from `date`, reusing
   * formatKoreanClock's "오전 7:23" output split on the first space.
   * No-op if the big clock markup isn't present.
   */
  function updateBigClock(date) {
    if (!bigClockTimeEl && !bigClockAmpmEl) return;
    var text = formatKoreanClock(date);
    var idx = text.indexOf(" ");
    var period = idx === -1 ? "" : text.slice(0, idx);
    var time = idx === -1 ? text : text.slice(idx + 1);
    if (bigClockAmpmEl) bigClockAmpmEl.textContent = period;
    if (bigClockTimeEl) bigClockTimeEl.textContent = time;
  }

  function loadForNow() {
    var data = getData();
    var now = new Date();
    var hhmm = formatHHMM(now);
    if (digitalClockEl) digitalClockEl.textContent = formatKoreanClock(now);
    updateBigClock(now);

    var result = pickKoQuote(data, hhmm, true);
    state.currentQuote = result.quote;
    renderCurrentQuote(result.message);
  }

  function shuffleQuote() {
    var data = getData();
    var now = new Date();
    var hhmm = formatHHMM(now);
    if (digitalClockEl) digitalClockEl.textContent = formatKoreanClock(now);
    updateBigClock(now);

    var pool = buildShufflePool(data, hhmm);
    if (pool.length === 0) {
      // No precise/bucket union available for this minute: fall back to
      // the same adjacent-bucket/"nothing available" path pickKoQuote uses.
      var fallback = pickKoQuote(data, hhmm, false);
      state.currentQuote = fallback.quote;
      renderCurrentQuote(fallback.message);
      return;
    }

    var chosen = pickShuffle(pool, state.recentSigs, SHUFFLE_RECENT_MAX);
    state.currentQuote = chosen;
    renderCurrentQuote(chosen ? null : "아직 문장이 없습니다.");
  }

  /**
   * Night dim overlay opacity for a given date: 0 outside 22:00-06:00,
   * ramping linearly up to ~0.45 near midnight (wrap-around range).
   */
  function computeNightDimOpacity(date) {
    var nowMinutes = date.getHours() * 60 + date.getMinutes();
    var start = 22 * 60; // 22:00
    var end = 6 * 60; // 06:00
    var span = 1440 - start + end; // total wrap-around night length
    var inNight = nowMinutes >= start || nowMinutes <= end;
    if (!inNight) return 0;
    var elapsed = nowMinutes >= start ? nowMinutes - start : 1440 - start + nowMinutes;
    var midpoint = span / 2;
    var distanceFromEdge = Math.min(elapsed, span - elapsed);
    var ratio = distanceFromEdge / midpoint;
    return 0.45 * ratio;
  }

  /**
   * Effective dim opacity: the manual slider value acts as a floor that
   * always applies (dock on or off), combined with the automatic
   * dock-only night dim via max() so neither one is lost.
   */
  function effectiveNightDim(date) {
    var manual = parseFloat(localStorage.getItem(MANUAL_DIM_KEY) || "0") || 0;
    var dockOn = localStorage.getItem(DOCK_MODE_KEY) === "on";
    var auto = dockOn ? computeNightDimOpacity(date) : 0;
    return Math.max(manual, auto);
  }

  function applyNightDim() {
    if (!nightDimEl) return;
    nightDimEl.style.opacity = String(effectiveNightDim(new Date()));
  }

  /**
   * OLED burn-in mitigation: while dock mode is on, drift #stage and
   * #digital-clock within a small translate() range using a smooth
   * transition, following a Lissajous-like sweep driven by wall-clock
   * time. Snaps back to translate(0,0) when dock mode is off.
   */
  function applyBurnInShift() {
    var dockOn = localStorage.getItem(DOCK_MODE_KEY) === "on";
    if (!dockOn) {
      if (stageEl) stageEl.style.transform = "translate(0px, 0px)";
      if (digitalClockEl) digitalClockEl.style.transform = "translate(0px, 0px)";
      if (bigClockEl) bigClockEl.style.transform = "translate(0px, 0px)";
      return;
    }
    var t = Date.now() / 60000; // slow drift, minute-scale period
    var vw = window.innerWidth / 100;
    var vh = window.innerHeight / 100;
    var x = Math.sin(t * 0.9) * 3 * vw;
    var y = Math.cos(t * 0.6) * 3 * vh;
    var transform = "translate(" + x.toFixed(2) + "px, " + y.toFixed(2) + "px)";
    if (stageEl) stageEl.style.transform = transform;
    if (digitalClockEl) digitalClockEl.style.transform = transform;
    if (bigClockEl) bigClockEl.style.transform = transform;
  }

  function requestWakeLock() {
    if (!("wakeLock" in navigator)) return;
    navigator.wakeLock
      .request("screen")
      .then(function (sentinel) {
        wakeLockSentinel = sentinel;
      })
      .catch(function () {});
  }

  function releaseWakeLock() {
    if (wakeLockSentinel) {
      wakeLockSentinel.release().catch(function () {});
      wakeLockSentinel = null;
    }
  }

  function setupWakeLock() {
    if (!("wakeLock" in navigator)) return;
    if (localStorage.getItem(DOCK_MODE_KEY) === "on") requestWakeLock();
    document.addEventListener("click", function () {
      if (localStorage.getItem(DOCK_MODE_KEY) === "on") requestWakeLock();
    });
    document.addEventListener("visibilitychange", function () {
      if (
        document.visibilityState === "visible" &&
        localStorage.getItem(DOCK_MODE_KEY) === "on"
      ) {
        requestWakeLock();
      }
    });
  }

  function setupFullscreenToggle() {
    if (!fullscreenToggleEl) return;
    var supported = !!(
      document.documentElement.requestFullscreen && document.exitFullscreen
    );
    if (!supported) {
      fullscreenToggleEl.hidden = true;
      return;
    }
    fullscreenToggleEl.hidden = false;

    function updateLabel() {
      var isFullscreen = !!document.fullscreenElement;
      var label = isFullscreen ? "창모드" : "전체화면";
      fullscreenToggleEl.textContent = label;
      fullscreenToggleEl.setAttribute("aria-label", label);
    }

    fullscreenToggleEl.addEventListener("click", function () {
      if (document.fullscreenElement) {
        document.exitFullscreen().catch(function () {});
      } else {
        document.documentElement.requestFullscreen().catch(function () {});
      }
    });

    document.addEventListener("fullscreenchange", updateLabel);
    updateLabel();
  }

  function isSettingsOpen() {
    return !!(settingsPanelEl && !settingsPanelEl.hidden);
  }

  function openSettingsPanel() {
    if (!settingsPanelEl) return;
    settingsPanelEl.hidden = false;
    if (settingsToggleEl) settingsToggleEl.setAttribute("aria-expanded", "true");
    renderFavoritesList();
    resetControlsHideTimer();
  }

  function closeSettingsPanel() {
    if (!settingsPanelEl) return;
    settingsPanelEl.hidden = true;
    if (settingsToggleEl) settingsToggleEl.setAttribute("aria-expanded", "false");
    resetControlsHideTimer();
  }

  function toggleSettingsPanel() {
    if (isSettingsOpen()) closeSettingsPanel();
    else openSettingsPanel();
  }

  function applyFontScale(scaleKey) {
    var scale = FONT_SCALE_MAP[scaleKey] || FONT_SCALE_MAP.normal;
    document.documentElement.style.setProperty("--quote-scale", String(scale));
    fontScaleBtns.forEach(function (btn) {
      var active = btn.getAttribute("data-scale") === scaleKey;
      btn.setAttribute("aria-pressed", active ? "true" : "false");
    });
  }

  function resetControlsHideTimer() {
    if (!controlsEl) return;
    controlsEl.classList.remove("controls-hidden");
    if (controlsHideTimer) clearTimeout(controlsHideTimer);
    var dockOn = localStorage.getItem(DOCK_MODE_KEY) === "on";
    if (!dockOn) return;
    if (isSettingsOpen()) return;
    controlsHideTimer = setTimeout(function () {
      controlsEl.classList.add("controls-hidden");
    }, CONTROLS_IDLE_MS);
  }

  function setupControlsAutoHide() {
    if (!controlsEl) return;
    ["mousemove", "touchstart", "click", "keydown"].forEach(function (evt) {
      document.addEventListener(evt, resetControlsHideTimer);
    });
    resetControlsHideTimer();
  }

  function applyDockMode(mode) {
    var isOn = mode === "on";
    if (dockToggleEl) {
      dockToggleEl.classList.toggle("is-active", isOn);
      dockToggleEl.setAttribute(
        "aria-label",
        isOn ? "거치 모드 켜짐" : "거치 모드 꺼짐"
      );
    }
    if (isOn) {
      requestWakeLock();
    } else {
      releaseWakeLock();
    }
    applyNightDim();
    applyBurnInShift();
    resetControlsHideTimer();
  }

  function toggleDockMode() {
    var current = localStorage.getItem(DOCK_MODE_KEY) === "on" ? "on" : "off";
    var next = current === "on" ? "off" : "on";
    localStorage.setItem(DOCK_MODE_KEY, next);
    applyDockMode(next);
  }

  function registerServiceWorker() {
    if (!("serviceWorker" in navigator)) return;
    navigator.serviceWorker.register("sw.js").catch(function () {});
  }

  function scheduleNextTick() {
    var now = new Date();
    var msToNextMinute =
      (60 - now.getSeconds()) * 1000 - now.getMilliseconds();
    setTimeout(function () {
      fadeSwap(loadForNow);
      applyNightDim();
      applyBurnInShift();
      scheduleNextTick();
    }, msToNextMinute);
  }

  function init() {
    var savedTheme = localStorage.getItem(THEME_KEY) || "auto";
    applyTheme(savedTheme);

    if (themeToggleEl) {
      themeToggleEl.addEventListener("click", cycleTheme);
    }

    if (dockToggleEl) {
      dockToggleEl.addEventListener("click", toggleDockMode);
    }

    if (saveToggleEl) {
      saveToggleEl.addEventListener("click", toggleFavorite);
    }

    if (shareToggleEl) {
      shareToggleEl.addEventListener("click", shareCurrentQuote);
    }

    setupFavoritesList();

    if (settingsToggleEl) {
      settingsToggleEl.addEventListener("click", toggleSettingsPanel);
    }

    if (dimSliderEl) {
      dimSliderEl.addEventListener("input", function () {
        localStorage.setItem(MANUAL_DIM_KEY, dimSliderEl.value);
        applyNightDim();
      });
    }

    fontScaleBtns.forEach(function (btn) {
      btn.addEventListener("click", function () {
        var scaleKey = btn.getAttribute("data-scale");
        localStorage.setItem(FONT_SCALE_KEY, scaleKey);
        applyFontScale(scaleKey);
      });
    });

    document.addEventListener("click", function (e) {
      if (themeToggleEl && themeToggleEl.contains(e.target)) return;
      if (dockToggleEl && dockToggleEl.contains(e.target)) return;
      if (saveToggleEl && saveToggleEl.contains(e.target)) return;
      if (shareToggleEl && shareToggleEl.contains(e.target)) return;
      if (fullscreenToggleEl && fullscreenToggleEl.contains(e.target)) return;
      if (settingsToggleEl && settingsToggleEl.contains(e.target)) return;
      if (settingsPanelEl && settingsPanelEl.contains(e.target)) return;
      if (isSettingsOpen()) {
        closeSettingsPanel();
        return;
      }
      fadeSwap(shuffleQuote);
    });

    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape") {
        if (isSettingsOpen()) closeSettingsPanel();
        return;
      }
      if (isSettingsOpen()) return;
      if (e.code === "Space" || e.key === " ") {
        e.preventDefault();
        fadeSwap(shuffleQuote);
      }
    });

    var savedManualDim = localStorage.getItem(MANUAL_DIM_KEY) || "0";
    if (dimSliderEl) dimSliderEl.value = savedManualDim;

    var savedFontScale = localStorage.getItem(FONT_SCALE_KEY) || "normal";
    applyFontScale(savedFontScale);

    var savedDockMode = localStorage.getItem(DOCK_MODE_KEY) === "on" ? "on" : "off";
    applyDockMode(savedDockMode);

    setupWakeLock();
    setupFullscreenToggle();
    setupControlsAutoHide();
    registerServiceWorker();

    loadForNow();
    scheduleNextTick();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})(typeof window !== "undefined" ? window : this);
