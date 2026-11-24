// Runs every test of the web harness, one after the other. The wasm must be built first.
//   node harness/web/build.mjs && node harness/web/test/all.mjs
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

const here = fileURLToPath(new URL(".", import.meta.url));
if (!existsSync(here + "../wasm/target/wasm32-unknown-unknown/release/bada_web.wasm")) {
  console.error("the wasm isn't built: run node harness/web/build.mjs first");
  process.exit(2);
}
let failed = 0;
for (const test of ["keytable.mjs", "field.mjs", "conformance.mjs", "smoke.mjs"]) {
  if (spawnSync(process.execPath, [here + test], { stdio: "inherit" }).status !== 0) failed++;
}
console.log(failed ? `FAIL ${failed} of 4 test files failed` : "ok   all 4 test files passed");
process.exit(failed ? 1 : 0);
