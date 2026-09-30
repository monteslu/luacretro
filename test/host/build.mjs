// Compile a cart/Lua file with the dynamic tier and build the headless host
// binary: node build.mjs <cart> <out-binary>
import { existsSync, readFileSync } from "node:fs";
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { compileCart, RUNTIME_SOURCES } from "../../compiler/dyn/index.js";
import { formatDiagnostics } from "../../compiler/index.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const rt = path.join(here, "../../runtime");

export function buildHost(cartPath, outBin, { cflags = [] } = {}) {
  const r = compileCart(new Uint8Array(fs.readFileSync(cartPath)), path.basename(cartPath), { debugLines: !!process.env.LC_DEBUG_LINES, resolveInclude: (p) => { const f = path.resolve(path.dirname(cartPath), p); return existsSync(f) ? readFileSync(f) : null; } });
  if (!r.ok) return { ok: false, error: formatDiagnostics(r.diagnostics) };
  const cfile = outBin + ".c";
  fs.writeFileSync(cfile, r.c);
  const srcs = [cfile, path.join(here, "host.c"), ...RUNTIME_SOURCES.map((s) =>
    s === "lc_p8snd.c" && process.env.LC_AUDIO_STUB ? process.env.LC_AUDIO_STUB : path.join(rt, s))];
  try {
    execFileSync("gcc", ["-std=gnu11", "-O2", "-g", "-no-pie", "-fno-pie", "-DLC_HOSTED_LIBC", `-I${rt}`,
      "-Wall", "-Wno-unused-parameter", "-Wno-unused-function", ...cflags, "-o", outBin, ...srcs], { stdio: ["ignore", "pipe", "pipe"] });
  } catch (e) {
    return { ok: false, error: String(e.stderr || e.message) };
  }
  return { ok: true, c: r.c };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const r = buildHost(process.argv[2], process.argv[3]);
  if (!r.ok) { console.error(r.error); process.exit(1); }
}
