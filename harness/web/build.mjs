// Builds the web harness into harness/web/dist: the engine as WebAssembly (harness/web/wasm), the
// page (harness/web/src), the stock layouts' files (engine/layouts, for the keyboard drawn on the
// page) and manifest.json, which says where they are. The wasm is named by its hash, so it can be
// cached for good.
//
//   node harness/web/build.mjs      then serve harness/web/dist (node harness/web/serve.mjs)
//
// Needs Rust with the wasm32-unknown-unknown target. Nothing is installed and nothing is deployed:
// vercel.json says how Vercel runs this.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";

const web = fileURLToPath(new URL(".", import.meta.url));
const root = web + "../../";
const out = web + "dist/";
const hash = bytes => createHash("sha256").update(bytes).digest("hex").slice(0, 12);

// The engine's data (the standard, §11): the name of each buffer → its file under engine/data.
// THE ENGINE LOADS NO DATA YET, so this is empty and the page passes no buffers. When it does, name
// the files here: each is gzipped into dist/data, listed in manifest.json under its buffer's name,
// fetched by src/load.js and passed to bada_data_new by src/engine.js as a named buffer. The
// names are in bada_describe's `data`.
const DATA = {
  // "ko-words": "ko-words.tsv",
  // "kanji": "kanji.bin",
};

execFileSync("cargo", ["build", "--release", "--target", "wasm32-unknown-unknown", "--manifest-path", web + "wasm/Cargo.toml"], { stdio: "inherit" });

rmSync(out, { recursive: true, force: true });
for (const dir of ["data", "layouts"]) mkdirSync(out + dir, { recursive: true });
for (const f of readdirSync(web + "src")) copyFileSync(web + "src/" + f, out + f);
copyFileSync(root + "docs/logo/bada-icon.svg", out + "icon.svg");
copyFileSync(root + "LICENSE", out + "LICENSE.txt");

const wasm = readFileSync(web + "wasm/target/wasm32-unknown-unknown/release/bada_web.wasm");
// One version for everything in the repository: the VERSION file's.
const version = readFileSync(root + "VERSION", "utf8").trim();
const manifest = { version, wasm: { path: `bada.${hash(wasm)}.wasm`, size: wasm.length }, data: {}, layouts: [] };
writeFileSync(out + manifest.wasm.path, wasm);

for (const [name, file] of Object.entries(DATA)) {
  const gz = gzipSync(readFileSync(root + "engine/data/" + file), { level: 9 });
  const path = `data/${name}.${hash(gz)}.gz`;
  writeFileSync(out + path, gz);
  manifest.data[name] = { path, size: gz.length };
}

for (const f of readdirSync(root + "engine/layouts").filter(f => f.endsWith(".json")).sort()) {
  const bytes = readFileSync(root + "engine/layouts/" + f);
  writeFileSync(out + "layouts/" + f, bytes);
  manifest.layouts.push(`layouts/${f}?v=${hash(bytes)}`);
}
writeFileSync(out + "manifest.json", JSON.stringify(manifest, null, 1) + "\n");

const total = manifest.wasm.size + Object.values(manifest.data).reduce((n, d) => n + d.size, 0);
console.log(`harness/web/dist: bada ${version}, ${manifest.layouts.length} layouts, ${Object.keys(manifest.data).length} data files, ${(total / 1e6).toFixed(2)} MB to download`);
