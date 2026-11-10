// Generates pre-compressed siblings (.br + .gz) for text-like assets after
// `vite build`, so the reverse proxy can serve them statically
// (brotli_static / gzip_static, no CPU per request, no HTTP overhead).
// Strategy applies to every dist file above MIN; binaries that carry their own
// compression (png, jpg, gif, mp4, webm, ico...) are skipped.
// Browser support: both .br (brotli_static) and .gz (gzip_static) are served
// only when the client sends a matching Accept-Encoding — nginx picks the one.
import { gzipSync, brotliCompressSync, constants } from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';

// Brotli is opt-in: the staging nginx has no ngx_brotli module (only gzip_static),
// so .br files would sit unused on disk while q11 costs minutes of CI per build.
// Flip COMPRESS_BR=1 when a brotli-capable proxy is in front of the assets.
const WITH_BR = process.env.COMPRESS_BR === '1';

const DIST = path.resolve(process.cwd(), 'dist');
// Mobile (Capacitor/Android AAPT2) consumes the same dist and rejects foo.js.gz
// next to foo.js as a duplicate resource: mobile jobs build with COMPRESS_ASSETS=0.
if (process.env.COMPRESS_ASSETS === '0') {
  console.log('compress-assets: skipped (COMPRESS_ASSETS=0 — mobile bundle)');
  process.exit(0);
}
const MIN = 64 * 1024; // compress text-like files above this size
const COMPRESSIBLE = new Set(['.js', '.mjs', '.wasm', '.css', '.html', '.htm', '.svg', '.txt', '.json', '.map']);
// Skip images/media that are already compressed end-to-end.
const SKIP = new Set(['.png', '.jpg', '.jpeg', '.gif', '.webp', '.apng', '.ico', '.mp4', '.m4v', '.mov', '.webm', '.mp3', '.woff', '.woff2', '.ttf', '.otf']);

let saved = 0, files = 0;
const walk = (dir) => {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) { walk(p); continue; }
    const ext = path.extname(e.name).toLowerCase();
    if (SKIP.has(ext) || !COMPRESSIBLE.has(ext)) continue;
    const size = e.size || fs.statSync(p).size;
    if (size < MIN) continue;
    if (p.endsWith('.br') || p.endsWith('.gz')) continue;
    const data = fs.readFileSync(p);
    // brotli q11 = smallest; use window for big files (24 = max)
    let br = null;
    if (WITH_BR) {
      br = brotliCompressSync(data, { [constants.BROTLI_PARAM_QUALITY]: 11, [constants.BROTLI_PARAM_LGWIN]: 24 });
      fs.writeFileSync(p + '.br', br);
      saved += size - br.length;
    }
    const gz = gzipSync(data, { level: 9 });
    fs.writeFileSync(p + '.gz', gz);
    const human = (n) => (n / 1e6).toFixed(1) + ' MB';
    console.log(`  ${path.relative(DIST, p)}: ${human(size)} → gz ${human(gz.length)}${WITH_BR ? ' / br ' + human(br.length) : ''}`);
    files++;
  }
};
if (!fs.existsSync(DIST)) { console.error('dist/ not found — run vite build first'); process.exit(1); }
walk(DIST);
console.log(`compress-assets: ${files} file(s) pre-compressed with gzip${WITH_BR ? ' + brotli' : ' (brotli off — set COMPRESS_BR=1 to enable)'}`);
