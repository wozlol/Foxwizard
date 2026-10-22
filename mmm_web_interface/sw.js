// FoxWizard Patchling — offline app shell cache.
// CACHE_NAME carries the Patchling version, which is always the firmware version.
// firmware/build_firmware.sh checks it.
const CACHE_NAME = 'foxwizard-patchling-1.67';
const SHELL_FILES = [
  './',
  './index.html',
  './manifest.json',
  './css/style.css',
  './js/app.js',
  './js/midi.js',
  './js/mapper.js',
  './js/presetXml.js',
  './js/dragReorder.js',
  './js/hostReport.js',
  './icons/icon.svg',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/mode-shark.png',
  './icons/mode-octopus.png'
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => cache.addAll(SHELL_FILES))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((names) => Promise.all(
      names.filter((name) => name !== CACHE_NAME).map((name) => caches.delete(name))
    )).then(() => self.clients.claim())
  );
});

// Network first: online visitors always get the current Patchling and firmware right away. The
// cache is only a fallback for offline use, refreshed by every successful fetch.
self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;
  event.respondWith(
    fetch(event.request).then((response) => {
      if (response.ok && response.type === 'basic') {
        const copy = response.clone();
        caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
      }
      return response;
    }).catch(() => caches.match(event.request))
  );
});
