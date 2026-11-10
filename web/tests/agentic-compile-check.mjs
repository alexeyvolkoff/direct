// Compile check for the agentic-mode touched svar sources.
// JS modules: esbuild parse; Svelte components: svelte/compiler (no vite build — CI only).
import * as esbuild from "esbuild";
import { createRequire } from "module";
import { promises as fs } from "fs";
import * as path from "path";
import url from "url";

const root = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), "..");
const require = createRequire(import.meta.url);
const svelteCompiler = require("svelte/compiler");
let failed = 0;

async function checkJs(rel) {
  const file = path.join(root, rel);
  try {
    const res = await esbuild.build({
      entryPoints: [file],
      bundle: false,
      write: false,
      logLevel: "silent",
      format: "esm",
    });
    if (res.errors.length) throw new Error(res.errors.map(e => e.text).join("; "));
    console.log("OK  js:", rel);
  } catch (e) {
    failed++;
    console.log("FAIL js:", rel, "\n   ", e.message);
  }
}

async function checkSvelte(rel) {
  const file = path.join(root, rel);
  try {
    const src = await fs.readFile(file, "utf8");
    const res = svelteCompiler.compile(src, { generate: "client", filename: file });
    console.log("OK  svelte:", rel, `(warnings=${res.warnings.length})`);
  } catch (e) {
    failed++;
    console.log("FAIL svelte:", rel, "\n   ", e.message && e.message.split("\n").slice(0, 12).join("\n"));
  }
}

async function checkJson(rel) {
  try {
    JSON.parse(await fs.readFile(path.join(root, rel), "utf8"));
    console.log("OK  json:", rel);
  } catch (e) {
    failed++;
    console.log("FAIL json:", rel, e.message);
  }
}

const checks = [
  ["svelte", "src/components/Settings.svelte"],
  ["svelte", "src/components/ChatApplet.svelte"],
  ["svelte", "src/App.svelte"],
];

for (const [kind, rel] of checks) {
  await checkSvelte(rel);
}
for (const rel of ["src/lib/i18n/locales/en.json", "src/lib/i18n/locales/ru.json"]) {
  await checkJson(rel);
}

process.exit(failed ? 1 : 0);
