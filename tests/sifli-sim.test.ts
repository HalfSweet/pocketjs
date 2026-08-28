// tests/sifli-sim.test.ts — the SiFli host's simulator smoke: boot the
// repository's hero demo at the SF32LB58 viewport (512x300 logical, raster
// density 2, render scale 2 -> a 1024x600 frame) and assert a non-flat,
// deterministic frame trace. With POCKETJS_SIFLI_PROJECT set to a firmware
// project directory, every guest in its pocket-sifli.json boots from
// <project>/assets and an optional `verify` block drives the launcher
// selection flow ({ "launcher": "launcher-main", "select": ["RIGHT"],
// "confirm": "CIRCLE", "expect": "cafe-main" }).

import { describe, expect, test } from "bun:test";
import { copyFileSync, existsSync, mkdirSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { BTN } from "../contracts/spec/spec.ts";
import { bootWorld, fnv1a } from "../hosts/sim/sim.ts";

const VIEWPORT = { width: 512, height: 300, rasterDensity: 2, renderScale: 2 };
const FRAME_BYTES = 1024 * 600 * 4;
const SETTLE_FRAMES = 12;

function distinctColors(pixels: Uint8Array, limit = 32): number {
  const colors = new Set<string>();
  for (let offset = 0; offset < pixels.length && colors.size < limit; offset += 4) {
    colors.add(`${pixels[offset]},${pixels[offset + 1]},${pixels[offset + 2]}`);
  }
  return colors.size;
}

async function settle(
  app: string,
  mutateOps?: (ops: Record<string, unknown>) => void,
): Promise<{ pixels: Uint8Array; hashes: string[] }> {
  const world = await bootWorld(app, 60, undefined, mutateOps, VIEWPORT);
  const hashes: string[] = [];
  let pixels = new Uint8Array();
  for (let frame = 0; frame < SETTLE_FRAMES; frame += 1) {
    world.frame(0);
    world.tick();
    pixels = world.render().slice();
    hashes.push(fnv1a(pixels));
  }
  return { pixels, hashes };
}

describe("sifli sim", () => {
  test("hero-main renders a non-flat 1024x600 frame at the SF32LB58 viewport", async () => {
    const { pixels } = await settle("hero-main");
    expect(pixels.length).toBe(FRAME_BYTES);
    expect(distinctColors(pixels)).toBeGreaterThanOrEqual(4);
  }, 60000);

  test("the frame trace is identical across boots", async () => {
    const first = await settle("hero-main");
    const second = await settle("hero-main");
    expect(second.hashes).toEqual(first.hashes);
  }, 60000);
});

interface ProjectManifest {
  readonly guests: readonly { readonly output: string }[];
  readonly verify?: {
    readonly launcher: string;
    readonly select?: readonly string[];
    readonly confirm?: string;
    readonly expect: string;
  };
}

const projectDir = process.env.POCKETJS_SIFLI_PROJECT
  ? resolve(process.env.POCKETJS_SIFLI_PROJECT)
  : undefined;

if (projectDir) {
  const manifest = JSON.parse(
    readFileSync(join(projectDir, "pocket-sifli.json"), "utf8"),
  ) as ProjectManifest;
  const dist = join(import.meta.dir, "..", "dist");
  mkdirSync(dist, { recursive: true });
  for (const guest of manifest.guests) {
    for (const extension of [".js", ".pak"]) {
      const source = join(projectDir, "assets", guest.output + extension);
      if (!existsSync(source)) throw new Error(`${source} missing: run bun tools/sifli.ts assets`);
      copyFileSync(source, join(dist, guest.output + extension));
    }
  }

  describe(`sifli project ${projectDir}`, () => {
    for (const guest of manifest.guests) {
      test(`${guest.output} renders a non-flat 1024x600 frame`, async () => {
        const { pixels } = await settle(
          guest.output,
          guest.output === manifest.verify?.launcher ? (ops) => (ops.appLaunch = () => 1) : undefined,
        );
        expect(pixels.length).toBe(FRAME_BYTES);
        expect(distinctColors(pixels)).toBeGreaterThanOrEqual(4);
      }, 60000);
    }

    if (manifest.verify) {
      const flow = manifest.verify;
      test(`launcher flow selects ${flow.expect}`, async () => {
        let launched = "";
        const launcher = await bootWorld(flow.launcher, 60, undefined, (ops) => {
          ops.appLaunch = (output: unknown) => {
            launched = String(output);
            return 1;
          };
        }, VIEWPORT);
        const step = (buttons: number): void => {
          launcher.frame(buttons);
          launcher.tick();
          launcher.render();
        };
        const button = (name: string): number => {
          const value = (BTN as Record<string, number>)[name];
          if (value === undefined) throw new Error(`unknown button ${name}`);
          return value;
        };
        for (let frame = 0; frame < 3; frame += 1) step(0);
        for (const name of flow.select ?? []) {
          step(button(name));
          for (let frame = 0; frame < SETTLE_FRAMES + 1; frame += 1) step(0);
        }
        step(button(flow.confirm ?? "CIRCLE"));
        expect(launched).toBe(flow.expect);
      }, 60000);
    }
  });
}
