import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import {
  extractHostBuildInputs,
  type HostBuildInputs,
} from "../framework/src/manifest/host-build-inputs.ts";
import type { ResolvedBuildPlan } from "../framework/src/manifest/plan.ts";
import {
  type BlackBerryClassicTargetId,
  resolveBlackBerryClassicBuildPlan,
} from "./blackberry-classic-profile.ts";

/**
 * Build steps shared by the two BlackBerry Classic host tools
 * (`blackberry-qnx.ts`, `blackberry-android.ts`): process helpers, the pinned
 * QuickJS checkout, and the guest bundle that both hosts embed.
 */

export interface CommandResult {
  readonly exitCode: number;
  readonly stdout: string;
  readonly stderr: string;
}

export function runCommand(
  program: string,
  args: readonly string[],
  cwd: string,
  env: NodeJS.ProcessEnv = process.env,
): CommandResult {
  const result = Bun.spawnSync({
    cmd: [program, ...args],
    cwd,
    env,
    stdout: "pipe",
    stderr: "pipe",
  });
  return {
    exitCode: result.exitCode,
    stdout: result.stdout.toString(),
    stderr: result.stderr.toString(),
  };
}

export function mustRunCommand(
  label: string,
  program: string,
  args: readonly string[],
  cwd: string,
  env: NodeJS.ProcessEnv = process.env,
): string {
  const result = runCommand(program, args, cwd, env);
  if (result.exitCode !== 0) {
    const detail = [result.stdout.trim(), result.stderr.trim()]
      .filter(Boolean)
      .join("\n");
    throw new Error(
      `${label}: ${program} ${args.join(" ")} failed (${result.exitCode})${
        detail ? `:\n${detail}` : ""
      }`,
    );
  }
  return result.stdout.trim();
}

export function sha256File(path: string): string {
  return createHash("sha256").update(readFileSync(path)).digest("hex");
}

export function printCheck(label: string, ok: boolean, detail: string): boolean {
  console.log(`${ok ? "[ok]" : "[missing]"} ${label}: ${detail}`);
  return ok;
}

export interface QuickJsPin {
  readonly version: string;
  readonly repository: string;
  readonly revision: string;
}

export interface QuickJsCheckout {
  readonly root: string;
  /** `libquickjs-sys/embed/quickjs` — the C sources both hosts compile. */
  readonly source: string;
  readonly staticFunctions: string;
}

export function quickJsCheckout(root: string): QuickJsCheckout {
  return {
    root,
    source: join(root, "libquickjs-sys/embed/quickjs"),
    staticFunctions: join(root, "libquickjs-sys/embed/static-functions.c"),
  };
}

/** The checkout is usable only at the pinned revision with a clean tree. */
export function quickJsCheckoutStatus(
  root: string,
  pin: QuickJsPin,
): { ok: boolean; detail: string } {
  if (!existsSync(join(root, ".git"))) {
    return { ok: false, detail: root };
  }
  const revision = runCommand("git", ["-C", root, "rev-parse", "HEAD"], root);
  const changes = runCommand(
    "git",
    ["-C", root, "status", "--porcelain=v1", "--untracked-files=all"],
    root,
  );
  const versionPath = join(quickJsCheckout(root).source, "VERSION");
  const version = existsSync(versionPath)
    ? readFileSync(versionPath, "utf8").trim()
    : "";
  const ok =
    revision.exitCode === 0 &&
    revision.stdout.trim() === pin.revision &&
    changes.exitCode === 0 &&
    changes.stdout.trim() === "" &&
    version === pin.version;
  return {
    ok,
    detail: `${root} (${revision.stdout.trim() || "missing"}, ${version || "no VERSION"})`,
  };
}

export function ensureQuickJsCheckout(
  label: string,
  root: string,
  pin: QuickJsPin,
): void {
  const status = quickJsCheckoutStatus(root, pin);
  if (status.ok) return;
  if (existsSync(root)) {
    throw new Error(
      `${label}: refusing to replace an unverified QuickJS directory: ${status.detail}`,
    );
  }
  mkdirSync(dirname(root), { recursive: true });
  mustRunCommand(
    label,
    "git",
    ["clone", "--filter=blob:none", "--no-checkout", pin.repository, root],
    dirname(root),
  );
  mustRunCommand(
    label,
    "git",
    ["-C", root, "checkout", "--detach", pin.revision],
    root,
  );
  const verified = quickJsCheckoutStatus(root, pin);
  if (!verified.ok) {
    throw new Error(`${label}: QuickJS verification failed: ${verified.detail}`);
  }
}

export interface GuestBundle {
  readonly plan: ResolvedBuildPlan;
  readonly inputs: HostBuildInputs;
  readonly javaScript: string;
  readonly pack: string;
}

export interface GuestBundleRequest {
  readonly label: string;
  readonly repository: string;
  readonly target: BlackBerryClassicTargetId;
  readonly manifestPath: string;
  /** Where the resolved plan is written for `tools/build.ts --plan`. */
  readonly planPath: string;
  readonly outputDirectory: string;
}

function locateGuestBundle(
  request: GuestBundleRequest,
  plan: ResolvedBuildPlan,
): GuestBundle {
  const inputs = extractHostBuildInputs(plan, { expectedTarget: request.target });
  return {
    plan,
    inputs,
    javaScript: join(request.outputDirectory, `${inputs.appOutput}.js`),
    pack: join(request.outputDirectory, `${inputs.appOutput}.pak`),
  };
}

export function currentGuestPlan(request: GuestBundleRequest): ResolvedBuildPlan {
  return resolveBlackBerryClassicBuildPlan(
    JSON.parse(readFileSync(request.manifestPath, "utf8")),
    request.target,
  );
}

/** Resolves the manifest for the target and compiles app.js + app.pak. */
export function buildGuestBundle(request: GuestBundleRequest): GuestBundle {
  const plan = currentGuestPlan(request);
  mkdirSync(dirname(request.planPath), { recursive: true });
  rmSync(request.outputDirectory, { recursive: true, force: true });
  mkdirSync(request.outputDirectory, { recursive: true });
  writeFileSync(request.planPath, `${JSON.stringify(plan, null, 2)}\n`);
  mustRunCommand(
    request.label,
    process.execPath,
    [
      join(request.repository, "tools/build.ts"),
      `--plan=${request.planPath}`,
      `--project-root=${request.repository}`,
      `--outdir=${request.outputDirectory}`,
    ],
    request.repository,
  );
  const bundle = locateGuestBundle(request, plan);
  if (!existsSync(bundle.javaScript) || !existsSync(bundle.pack)) {
    throw new Error(`${request.label}: guest build did not emit app.js and app.pak`);
  }
  console.log(`${request.label}: guest bundle -> ${request.outputDirectory}`);
  return bundle;
}

/** Reads a previously built bundle and rejects it when the manifest moved on. */
export function readGuestBundle(request: GuestBundleRequest): GuestBundle {
  if (!existsSync(request.planPath)) {
    throw new Error(`${request.label}: resolved plan is absent; run build-demo first`);
  }
  const stored = JSON.parse(readFileSync(request.planPath, "utf8")) as ResolvedBuildPlan;
  if (stored.planHash !== currentGuestPlan(request).planHash) {
    throw new Error(`${request.label}: resolved plan is stale; rerun build-demo`);
  }
  const bundle = locateGuestBundle(request, stored);
  if (!existsSync(bundle.javaScript) || !existsSync(bundle.pack)) {
    throw new Error(
      `${request.label}: guest JavaScript or pack is absent; rerun build-demo`,
    );
  }
  return bundle;
}
