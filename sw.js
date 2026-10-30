const CACHE = "suhu-v3";
const SHELL = ["./", "index.html", "manifest.webmanifest", "icon-192.png", "icon-512.png"];
self.addEventListener("install", e => e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL))));
self.addEventListener("activate", e => e.waitUntil(
  caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k))))));
self.addEventListener("fetch", e => {
  if (e.request.method !== "GET") return;
  e.respondWith(fetch(e.request).then(r => {
    if (new URL(e.request.url).origin === location.origin) {
      const copy = r.clone(); caches.open(CACHE).then(c => c.put(e.request, copy));
    }
    return r;
  }).catch(() => caches.match(e.request)));
});
