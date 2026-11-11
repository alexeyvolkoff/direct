import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import monacoEditorPlugin from 'vite-plugin-monaco-editor';

import { execSync } from 'child_process';
import fs from 'fs';
import path from 'path';

function getBuildVersionInfo() {
  const isP2P = process.env.P2P_ENABLED === '1' ||
                process.env.P2P_ENABLED === 'true' ||
                process.env.DEFINES?.includes('P2P_ENABLED') ||
                process.env.DEFINE?.includes('P2P_ENABLED') ||
                process.argv.includes('P2P_ENABLED') ||
                process.argv.includes('-DP2P_ENABLED') ||
                process.argv.some(a => typeof a === 'string' && a.includes('P2P_ENABLED'));

  // Building with P2P_ENABLED sets PRODUCT_VERSION = 3
  let productVersion = '3';
  if (process.env.PRODUCT_VERSION) {
    productVersion = process.env.PRODUCT_VERSION;
  } else if (!isP2P && process.env.LEGACY_V2) {
    productVersion = '2';
  }

  // If VITE_APP_VERSION is explicitly passed from CI/env, use it
  if (process.env.VITE_APP_VERSION) {
    return {
      version: process.env.VITE_APP_VERSION,
      productVersion,
      isP2P
    };
  }

  // Check generated build_version.h from C++ qmake build if present
  const candidateHeaders = [
    path.resolve(process.cwd(), 'build_version.h'),
    path.resolve(process.cwd(), '../build_version.h'),
    path.resolve(process.cwd(), '../Node/build_version.h'),
    path.resolve(process.cwd(), '../../build_version.h')
  ];
  for (const h of candidateHeaders) {
    if (fs.existsSync(h)) {
      try {
        const c = fs.readFileSync(h, 'utf8');
        const m = c.match(/#define\s+APP_VERSION\s+"([^"]+)"/);
        if (m && m[1]) {
          let ver = m[1];
          if (productVersion && !ver.startsWith(productVersion + '.')) {
            ver = productVersion + '.' + ver.replace(/^\d+\./, '');
          }
          return {
            version: ver,
            productVersion,
            isP2P
          };
        }
      } catch (_) {}
    }
  }

  // Fallback to git commit count
  let revision = '';
  try {
    revision = execSync('git rev-list HEAD --count', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();
  } catch (_) {
    revision = '0';
  }

  const version = `${productVersion}.1.5.${revision}`;
  return { version, productVersion, isP2P };
}

const buildInfo = getBuildVersionInfo();
const webBuildId = `${buildInfo.version}-${Date.now().toString(36)}`;

function webVersionManifestPlugin() {
  return {
    name: 'web-version-manifest',
    apply: 'build',
    writeBundle(options) {
      try {
        const outDir = options?.dir || path.resolve(process.cwd(), 'dist');
        const assetsDir = path.join(outDir, 'assets');
        fs.mkdirSync(assetsDir, { recursive: true });
        fs.writeFileSync(
          path.join(assetsDir, 'web-version.json'),
          JSON.stringify({ version: buildInfo.version, buildId: webBuildId }) + '\n',
          'utf8'
        );
      } catch (err) {
        console.warn('[vite] failed to write web-version.json:', err);
      }
    }
  };
}

// "chat" | "files" | "" (auto). Used as the default for static/Capacitor
// bundles and as the Vite dev default; the production gateway overrides it
// per request from the Host header.
const buildAppMode = String(process.env.VITE_APP_MODE || process.env.APP_MODE || "").toLowerCase();

/**
 * Dev-only plugin: emulates the C++ Stefan Frings TemplateParser that runs in the
 * production gateway. In dev mode Vite serves index.html directly, so we strip the
 * `{if …}…{end …}` blocks and replace `{VAR}` placeholders with sensible defaults.
 */
function templateParserPlugin() {
  const defaults = {
    TITLE:       buildAppMode === 'chat' ? 'On My Chat' : 'On My Disk',
    APPTITLE:    buildAppMode === 'chat' ? 'On My Chat' : 'On My Disk',
    COMPANY:     'On My Chat community',
    COMPANYURL:  'https://github.com/alexeyvolkoff/onmydisk',
    SLOGAN:      'Delivering content right from your disk',
    DESCRIPTION: 'Secure P2P file sharing',
    OWNER:       'OnMyDisk',
    CARD:        'summary_large_image',
    SERVERURL:   '',
    LINKURL:     '',
    COVERVERSION:'0',
    IMG_WIDTH:   '1200',
    IMG_HEIGHT:  '630',
    VERSION:     buildInfo.version,
    APP_MODE:    buildAppMode || 'files',
  };

  return {
    name: 'template-parser',
    apply: 'serve',
    transformIndexHtml(html) {
      // 1. Remove <!--{if isShared}-->…<!--{end isShared}--> or {if isShared}…{end isShared} blocks entirely
      html = html.replace(/(?:<!--)?\{if\s+isShared\}(?:-->)?[\s\S]*?(?:<!--)?\{end\s+isShared\}(?:-->)?/g, '');

      // 2. Replace known {PLACEHOLDER} tokens with default values
      for (const [key, val] of Object.entries(defaults)) {
        html = html.replaceAll(`{${key}}`, val);
      }

      return html;
    },
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const url = req.url ? req.url.split('?')[0].split('#')[0] : '';
        const staticTemplates = {
          '/tos': 'public/tos.html',
          '/tos.html': 'public/tos.html',
          '/about': 'public/about.html',
          '/about.html': 'public/about.html',
        };
        if (staticTemplates[url]) {
          const tplPath = path.resolve(process.cwd(), staticTemplates[url]);
          if (fs.existsSync(tplPath)) {
            let html = fs.readFileSync(tplPath, 'utf8');
            for (const [key, val] of Object.entries(defaults)) {
              html = html.replaceAll(`{${key}}`, val);
            }
            res.setHeader('Content-Type', 'text/html; charset=utf-8');
            res.end(html);
            return;
          }
        }
        next();
      });
    }
  };
}

export default defineConfig({
  define: {
    'import.meta.env.VITE_APP_VERSION': JSON.stringify(buildInfo.version),
    'import.meta.env.VITE_WEB_BUILD_ID': JSON.stringify(webBuildId),
    'import.meta.env.VITE_APP_MODE': JSON.stringify(buildAppMode),
    'PRODUCT_VERSION': JSON.stringify(Number(buildInfo.productVersion)),
    '__PRODUCT_VERSION__': JSON.stringify(buildInfo.productVersion),
    '__P2P_ENABLED__': JSON.stringify(buildInfo.isP2P),
    '__APP_MODE__': JSON.stringify(buildAppMode),
  },
  server: {
    proxy: {
      '/api/device_memory_thumb': {
        target: 'http://localhost:4000',
        changeOrigin: true,
        secure: false,
      },
      '/chat/image': {
        target: 'http://localhost:4000',
        changeOrigin: true,
        secure: false,
      },
      '/personas': {
        target: 'http://localhost:4000',
        changeOrigin: true,
        secure: false,
      },
      '/generate': {
        target: 'http://localhost:4000',
        changeOrigin: true,
        secure: false,
      },
      '/assistant': {
        target: 'http://localhost:4000',
        changeOrigin: true,
        secure: false,
      },
      '/chat': {
        target: 'http://localhost:4000',
        changeOrigin: true,
        secure: false,
      },
      '/api': {
        target: 'https://disk.direct',
        changeOrigin: true,
        secure: false,
      },
      '/rest': {
        target: 'https://disk.direct',
        changeOrigin: true,
        secure: false,
      },
    },
  },
  optimizeDeps: {
    include: ['@ai-sdk/svelte'],
    exclude: ['@xterm/xterm'],
  },
  plugins: [
    templateParserPlugin(),
    webVersionManifestPlugin(),
    svelte(),
    monacoEditorPlugin.default({
      languageWorkers: ['editorWorkerService', 'css', 'html', 'json']
    })
  ],
  esbuild: {
    // Preserve WASM/Emscripten & xterm.js exception/control structures across production builds.
    // esbuild's minifySyntax rewrites try/catch & typed array memory views inside WASM glue,
    // which causes libssh2 WASM output buffers to drop frames under minified execution.
    minifySyntax: false,
    minifyWhitespace: true
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    minify: 'esbuild',
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (id.includes('node_modules/monaco-editor')) {
            return 'vendor-monaco';
          }
          if (id.includes('node_modules/pdfjs-dist')) {
            return 'vendor-pdfjs';
          }
          if (id.includes('node_modules/@xterm')) {
            return 'vendor-xterm';
          }
          if (id.includes('node_modules/gun')) {
            return 'vendor-gun';
          }
          if (id.includes('node_modules/video.js') || id.includes('node_modules/lightgallery')) {
            return 'vendor-media';
          }
          if (id.includes('node_modules/rxjs')) {
            return 'vendor-rxjs';
          }
        }
      }
    }
  }
});
