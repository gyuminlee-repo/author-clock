/* Author Clock service worker: offline cache-first app shell. */
"use strict";

var CACHE_NAME = "author-clock-v4";
var PRECACHE_URLS = [
  "./index.html",
  "assets/style.css",
  "assets/app.js",
  "data/ko_quotes.js",
  "manifest.webmanifest",
  "assets/icons/icon-192.png",
  "assets/icons/icon-512.png",
  "assets/icons/icon-512-maskable.png",
  "assets/icons/apple-touch-icon.png",
  "assets/icons/favicon-32.png",
];

self.addEventListener("install", function (event) {
  event.waitUntil(
    caches.open(CACHE_NAME).then(function (cache) {
      return cache.addAll(PRECACHE_URLS);
    })
  );
  self.skipWaiting();
});

self.addEventListener("activate", function (event) {
  event.waitUntil(
    caches.keys().then(function (keys) {
      return Promise.all(
        keys
          .filter(function (key) {
            return key !== CACHE_NAME;
          })
          .map(function (key) {
            return caches.delete(key);
          })
      );
    })
  );
  self.clients.claim();
});

function isCacheFirstPath(pathname) {
  return pathname.indexOf("/data/") !== -1 || pathname.indexOf("/assets/icons/") !== -1;
}

function networkFirst(event) {
  return fetch(event.request)
    .then(function (response) {
      var copy = response.clone();
      caches.open(CACHE_NAME).then(function (cache) {
        cache.put(event.request, copy);
      });
      return response;
    })
    .catch(function () {
      return caches.match(event.request).then(function (cached) {
        if (cached) {
          return cached;
        }
        if (event.request.mode === "navigate") {
          return caches.match("./index.html");
        }
        return Promise.reject(new Error("network and cache both failed"));
      });
    });
}

function cacheFirst(event) {
  return caches.match(event.request).then(function (cached) {
    var network = fetch(event.request)
      .then(function (response) {
        caches.open(CACHE_NAME).then(function (cache) {
          cache.put(event.request, response.clone());
        });
        return response;
      })
      .catch(function () {
        return cached;
      });
    return cached || network;
  });
}

self.addEventListener("fetch", function (event) {
  var url = new URL(event.request.url);

  if (url.origin !== self.location.origin) {
    return;
  }

  if (event.request.mode === "navigate" || isCacheFirstPath(url.pathname) === false) {
    event.respondWith(networkFirst(event));
    return;
  }

  event.respondWith(cacheFirst(event));
});
