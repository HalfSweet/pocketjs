// tools/soft.ts — build and drive the software framebuffer host (hosts/soft).
//
// The host is the shared QuickJS C runtime (hosts/soft/pocket_runtime.c, the
// one every C device host links) on top of engine/symbian's C ABI built
// `software-only`, with a main that renders each virtual frame into memory
// and prints one FNV-1a hash per frame. No window, no wall clock.
//
//   bun tools/soft.ts doctor
//   bun tools/soft.ts build
//   bun tools/soft.ts run <app> [--frames N] [--input "f:mask,..."] [--framework fw]
//                          [--capture f,f] [--png-dir DIR] [--hashes out.json]
//   bun tools/soft.ts check <app> [--frames N] [--input "f:mask,..."] [--framework fw]
//
// `run` builds the app bundle with tools/build.ts, runs it on the host and
// prints the per-frame hashes. `check` runs the SAME bundle and tape on the
// wasm oracle (hosts/web/wasm-ops.js, the golden harness) and on the soft
// host, and exits 1 at the first frame whose hash differs — the QuickJS host
// must agree with the oracle byte for byte, the way every device host does.
//
// `--input` is the latched tape shared with tools/tape.ts record and the PSP
// capture build: `frame:mask` pairs, each mask held until the next pair.

import { existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { fileURLToPath } from "node:url";
import { join, resolve } from "node:path";
import { createWasmUi } from "../hosts/web/wasm-ops.js";
import { encodePNG } from "../tests/png.ts";
import {
  ensureQuickJsCheckout,
  mustRunCommand,
  printCheck,
  quickJsCheckout,
  quickJsCheckoutStatus,
  runCommand,
  type QuickJsPin,
} from "./native-host-build.ts";
import toolchainJson from "./cli/soft-toolchain.json";

const LABEL = "PocketJS soft";
const ROOT = resolve(fileURLToPath(new URL("..", import.meta.url)));
const BUILD = join(ROOT, ".pocket-build/soft");
const OBJECTS = join(BUILD, "objects");
const RUST_TARGET = join(BUILD, "rust-target");
const DIST = join(BUILD, "dist/");
const BINARY = join(BUILD, "pocket-soft");
const WASM_PATH = join(ROOT, "hosts/web/pocketjs.wasm");
const CORE_DIRECTORY = join(ROOT, "engine/symbian");
const HOST_DIRECTORY = join(ROOT, "hosts/soft");
const TARGET_ID = "soft";
/** Plan-less bundles embed no contract, so the ABI number is informational. */
const HOST_ABI = 0;
const QUICKJS_SOURCES = ["cutils.c", "dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c"];

interface Toolchain {
  readonly cachePath: string;
  readonly rustToolchain: string;
  readonly quickjs: QuickJsPin;
}

const TOOLCHAIN = toolchainJson as Toolchain;
const CACHE = join(homedir(), ".cache/pocket-stack", TOOLCHAIN.cachePath);
const QUICKJS_ROOT = join(CACHE, "sources/quickjs-rs");

// ---------------------------------------------------------------------------
// arguments
// ---------------------------------------------------------------------------

const argv = process.argv.slice(2);
const command = argv[0] ?? "";
const positional: string[] = [];
const flags = new Map<string, string>();
for (let i = 1; i < argv.length; i++) {
  const a = argv[i];
  if (a.startsWith("--")) {
    const eq = a.indexOf("=");
    if (eq >= 0) flags.set(a.slice(2, eq), a.slice(eq + 1));
    else {
      flags.set(a.slice(2), argv[i + 1] ?? "");
      i++;
    }
  } else positional.push(a);
}

function flag(name: string, fallback = ""): string {
  return flags.get(name) ?? fallback;
}

function commandPath(name: string): string | null {
  return Bun.which(name);
}

// ---------------------------------------------------------------------------
// doctor
// ---------------------------------------------------------------------------

function doctor(): boolean {
  const cc = commandPath("cc");
  const cargo = commandPath("cargo");
  const rustup = commandPath("rustup");
  const toolchains = rustup ? runCommand(rustup, ["toolchain", "list"], ROOT).stdout : "";
  const quickjs = quickJsCheckoutStatus(QUICKJS_ROOT, TOOLCHAIN.quickjs);
  const checks = [
    printCheck("C compiler", cc !== null, cc ?? "cc not on PATH"),
    printCheck("cargo", cargo !== null, cargo ?? "cargo not on PATH"),
    printCheck(
      `rust ${TOOLCHAIN.rustToolchain}`,
      toolchains.includes(TOOLCHAIN.rustToolchain),
      toolchains.includes(TOOLCHAIN.rustToolchain)
        ? "installed (engine/symbian/rust-toolchain.toml selects it)"
        : `rustup toolchain install ${TOOLCHAIN.rustToolchain}`,
    ),
    printCheck("pinned QuickJS", quickjs.ok, quickjs.ok ? quickjs.detail : `${quickjs.detail} — build fetches it`),
    printCheck("host binary", existsSync(BINARY), existsSync(BINARY) ? BINARY : "run `bun tools/soft.ts build`"),
    printCheck("wasm oracle", existsSync(WASM_PATH), existsSync(WASM_PATH) ? WASM_PATH : "check builds it (tools/wasm.ts)"),
  ];
  return checks.every(Boolean);
}

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------

function buildHost(): void {
  const cc = commandPath("cc");
  if (!cc) throw new Error(`${LABEL}: cc is not on PATH`);
  const cargo = commandPath("cargo");
  if (!cargo) throw new Error(`${LABEL}: cargo is not on PATH`);

  ensureQuickJsCheckout(LABEL, QUICKJS_ROOT, TOOLCHAIN.quickjs);
  const quickjs = quickJsCheckout(QUICKJS_ROOT).source;

  mkdirSync(OBJECTS, { recursive: true });
  mkdirSync(RUST_TARGET, { recursive: true });

  // The core: engine/symbian's C ABI over pocketjs-core, no GL, libc malloc.
  // rust-toolchain.toml in that directory selects the pinned nightly.
  console.log(`${LABEL}: building pocketjs-symbian-core (software-only) for the host`);
  mustRunCommand(
    LABEL,
    cargo,
    ["build", "--release", "--locked", "--features", "bare-platform,software-only"],
    CORE_DIRECTORY,
    { ...process.env, CARGO_TARGET_DIR: RUST_TARGET },
  );
  const coreLibrary = join(RUST_TARGET, "release/libpocketjs_symbian_core.a");
  if (!existsSync(coreLibrary)) {
    throw new Error(`${LABEL}: cargo did not produce ${coreLibrary}`);
  }

  const compile = (source: string, object: string, extra: readonly string[]) => {
    mustRunCommand(LABEL, cc, ["-O2", ...extra, "-c", source, "-o", object], ROOT);
  };
  const quickJsFlags = [
    "-std=gnu11",
    "-funsigned-char",
    "-fno-strict-aliasing",
    "-D_GNU_SOURCE",
    `-DCONFIG_VERSION="${TOOLCHAIN.quickjs.version}"`,
    "-I",
    quickjs,
    "-w",
  ];
  const objects: string[] = [];
  for (const source of QUICKJS_SOURCES) {
    const object = join(OBJECTS, `quickjs-${source.replace(/\.c$/, "")}.o`);
    compile(join(quickjs, source), object, quickJsFlags);
    objects.push(object);
  }
  const firstParty = [
    "-std=c99",
    "-Wall",
    "-Wextra",
    "-fwrapv",
    `-DPOCKETJS_TARGET_ID="${TARGET_ID}"`,
    `-DPOCKETJS_HOST_ABI=${HOST_ABI}`,
    "-I",
    HOST_DIRECTORY,
  ];
  const runtimeObject = join(OBJECTS, "pocket_runtime.o");
  compile(join(HOST_DIRECTORY, "pocket_runtime.c"), runtimeObject, [...firstParty, "-isystem", quickjs]);
  const mainObject = join(OBJECTS, "main.o");
  compile(join(HOST_DIRECTORY, "main.c"), mainObject, firstParty);
  // compiler_builtins in the panic=abort core still names the unwinding
  // personality on Mach-O and glibc targets; the stub aborts if ever reached.
  const personalityObject = join(OBJECTS, "rust_eh_personality.o");
  compile(join(HOST_DIRECTORY, "rust_eh_personality.c"), personalityObject, firstParty);

  mustRunCommand(
    LABEL,
    cc,
    ["-o", BINARY, mainObject, runtimeObject, personalityObject, ...objects, coreLibrary, "-lm"],
    ROOT,
  );
  console.log(`${LABEL}: host binary -> ${BINARY}`);
}

// ---------------------------------------------------------------------------
// bundles and tapes
// ---------------------------------------------------------------------------

interface Bundle {
  readonly app: string;
  readonly javaScript: string;
  readonly pack: string;
}

function buildBundle(app: string): Bundle {
  rmSync(DIST, { recursive: true, force: true });
  mkdirSync(DIST, { recursive: true });
  const args = [join(ROOT, "tools/build.ts"), app, `--outdir=${DIST}`];
  const framework = flag("framework");
  if (framework) args.push(`--framework=${framework}`);
  mustRunCommand(LABEL, process.execPath, args, ROOT);
  let javaScript = join(DIST, `${app}.js`);
  if (!existsSync(javaScript)) {
    const built = readdirSync(DIST).filter((name) => name.endsWith(".js"));
    if (built.length !== 1) {
      throw new Error(`${LABEL}: expected one bundle in ${DIST}, found ${built.join(", ") || "none"}`);
    }
    javaScript = join(DIST, built[0]);
  }
  const pack = javaScript.replace(/\.js$/, ".pak");
  if (!existsSync(pack)) throw new Error(`${LABEL}: ${pack} is missing`);
  return { app, javaScript, pack };
}

/** `frame:mask,...` → one mask per frame, each held until the next pair. */
function expandTape(script: string, frames: number): Uint32Array {
  const masks = new Uint32Array(frames);
  const pairs = script
    .split(",")
    .map((pair) => pair.trim())
    .filter(Boolean)
    .map((pair) => {
      const [frameText, maskText] = pair.split(":");
      const frame = Number(frameText);
      const mask = Number(maskText);
      if (!Number.isInteger(frame) || frame < 0 || !Number.isInteger(mask) || mask < 0) {
        throw new Error(`${LABEL}: bad --input entry "${pair}"`);
      }
      return { frame, mask };
    });
  let current = 0;
  let next = 0;
  for (let f = 0; f < frames; f++) {
    while (next < pairs.length && pairs[next].frame <= f) current = pairs[next++].mask;
    masks[f] = current;
  }
  return masks;
}

interface HostReport {
  readonly host: string;
  readonly width: number;
  readonly height: number;
  readonly frames: number;
  readonly hashes: string[];
}

function runHost(
  bundle: Bundle,
  frames: number,
  script: string,
  capture: number[],
  dumpDirectory: string | null,
): HostReport {
  if (!existsSync(BINARY)) buildHost();
  const args = [
    "--js",
    bundle.javaScript,
    "--pak",
    bundle.pack,
    "--frames",
    String(frames),
    "--hashes",
    "-",
  ];
  if (script) args.push("--input", script);
  if (capture.length > 0 && dumpDirectory) {
    mkdirSync(dumpDirectory, { recursive: true });
    args.push("--capture", capture.join(","), "--dump", dumpDirectory);
  }
  const result = runCommand(BINARY, args, ROOT);
  if (result.exitCode !== 0) {
    throw new Error(`${LABEL}: pocket-soft failed (${result.exitCode}):\n${result.stderr.trim()}`);
  }
  return JSON.parse(result.stdout) as HostReport;
}

/** FNV-1a 32-bit over the RGBA framebuffer — hosts/sim/sim.ts fnv1a(). */
function fnv1a(bytes: Uint8Array): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) {
    h ^= bytes[i];
    h = Math.imul(h, 0x01000193);
  }
  return (h >>> 0).toString(16).padStart(8, "0");
}

/** The golden harness, inlined: the same bundle on the wasm rasterizer. */
async function runOracle(bundle: Bundle, masks: Uint32Array): Promise<string[]> {
  if (!existsSync(WASM_PATH)) {
    mustRunCommand(LABEL, process.execPath, [join(ROOT, "tools/wasm.ts")], ROOT);
  }
  const wasm = await createWasmUi(await Bun.file(WASM_PATH).arrayBuffer());
  const g = globalThis as Record<string, unknown>;
  g.ui = wasm.ops;
  g.__pak = await Bun.file(bundle.pack).arrayBuffer();
  g.frame = undefined;
  try {
    (0, eval)(await Bun.file(bundle.javaScript).text());
    const frame = g.frame as ((buttons: number) => void) | undefined;
    if (typeof frame !== "function") {
      throw new Error(`${LABEL}: bundle did not install globalThis.frame`);
    }
    const hashes: string[] = [];
    for (let f = 0; f < masks.length; f++) {
      frame(masks[f]);
      wasm.tick();
      hashes.push(fnv1a(wasm.render()));
    }
    return hashes;
  } finally {
    delete g.ui;
    delete g.__pak;
    g.frame = undefined;
  }
}

function parseCapture(): number[] {
  return flag("capture")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean)
    .map((s) => {
      const n = Number(s);
      if (!Number.isInteger(n) || n < 0) throw new Error(`${LABEL}: bad --capture entry "${s}"`);
      return n;
    });
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

function usage(): never {
  console.error(
    "usage:\n" +
      "  bun tools/soft.ts doctor\n" +
      "  bun tools/soft.ts build\n" +
      '  bun tools/soft.ts run <app> [--frames N] [--input "f:mask,..."] [--framework fw]\n' +
      "                         [--capture f,f] [--png-dir DIR] [--hashes out.json]\n" +
      '  bun tools/soft.ts check <app> [--frames N] [--input "f:mask,..."] [--framework fw]',
  );
  process.exit(2);
}

async function main(): Promise<void> {
  switch (command) {
    case "doctor": {
      process.exit(doctor() ? 0 : 1);
    }
    case "build": {
      buildHost();
      return;
    }
    case "run": {
      const app = positional[0];
      if (!app) usage();
      const frames = Number(flag("frames", "60"));
      const capture = parseCapture();
      const pngDirectory = flag("png-dir");
      const dump = capture.length > 0 ? join(BUILD, "captures") : null;
      const bundle = buildBundle(app);
      const report = runHost(bundle, frames, flag("input"), capture, dump);
      if (pngDirectory && dump) {
        mkdirSync(pngDirectory, { recursive: true });
        for (const f of capture) {
          const raw = new Uint8Array(await Bun.file(join(dump, `frame-${String(f).padStart(4, "0")}.rgba`)).arrayBuffer());
          const png = join(pngDirectory, `${app}.${f}.png`);
          writeFileSync(png, encodePNG(raw, report.width, report.height));
          console.log(`${LABEL}: ${png}`);
        }
      }
      const hashesPath = flag("hashes");
      if (hashesPath) {
        writeFileSync(hashesPath, `${JSON.stringify(report, null, 2)}\n`);
        console.log(`${LABEL}: ${hashesPath}`);
      } else {
        console.log(JSON.stringify(report));
      }
      return;
    }
    case "check": {
      const app = positional[0];
      if (!app) usage();
      const frames = Number(flag("frames", "60"));
      const script = flag("input");
      const bundle = buildBundle(app);
      const masks = expandTape(script, frames);
      const soft = runHost(bundle, frames, script, [], null);
      const oracle = await runOracle(bundle, masks);
      let divergent = -1;
      for (let f = 0; f < frames; f++) {
        if (soft.hashes[f] !== oracle[f]) {
          divergent = f;
          break;
        }
      }
      if (divergent >= 0) {
        console.error(
          `${LABEL}: ${app} diverges at frame ${divergent}: soft=${soft.hashes[divergent]} oracle=${oracle[divergent]}`,
        );
        process.exit(1);
      }
      console.log(`${LABEL}: ${app} — ${frames} frames, soft host == wasm oracle (last hash ${oracle[frames - 1]})`);
      return;
    }
    default:
      usage();
  }
}

await main();
