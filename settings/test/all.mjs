// Runs every test of the settings page. No browser, and nothing to build.
//   node settings/test/all.mjs
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = fileURLToPath(new URL(".", import.meta.url));
let failed = 0;
for (const test of ["model.mjs"]) {
  if (spawnSync(process.execPath, [here + test], { stdio: "inherit" }).status !== 0) failed++;
}
process.exit(failed ? 1 : 0);
