import { afterEach, describe, expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import {
  finalizeBuildPlan,
  type ResolvedBuildPlanContent,
} from "../framework/src/manifest/plan.ts";
import {
  finalizeBundleArtifact,
  NETWORK_BINDING_DEFINE,
  NETWORK_BINDING_FACTORY_PARAMETER,
  networkFactoryDefines,
  selectBundleArtifactMode,
  wrapNetworkBundleFactory,
} from "../tools/network-bundle-factory.ts";

const MARKER = "__pocketNetworkFactoryTestMarker";
const ROOT = dirname(fileURLToPath(new URL("../package.json", import.meta.url)));
const GLOBAL_BINDING_NAMES = [
  NETWORK_BINDING_DEFINE,
  NETWORK_BINDING_FACTORY_PARAMETER,
] as const;

type TestGlobal = typeof globalThis & Record<string, unknown>;

function globals(): TestGlobal {
  return globalThis as TestGlobal;
}

function evaluateArtifact(source: string): unknown {
  return (0, eval)(source);
}

function content(withNetwork: boolean): ResolvedBuildPlanContent {
  return {
    app: {
      id: "dev.pocketjs.network-factory-test",
      title: "Network factory test",
      entry: "app.ts",
      framework: "solid",
      output: "app",
    },
    target: { id: "esp-test", hostAbi: 1 },
    viewport: {
      logical: [320, 240],
      physical: [320, 240],
      presentation: "integer-fit",
      rasterDensity: 1,
    },
    features: withNetwork ? { "network.http.client": true } : {},
    ...(withNetwork
      ? {
          network: {
            policy: {
              version: 1 as const,
              connect: [],
              listen: [],
              localNetwork: false,
              insecureTransport: false,
              broadcast: false,
              multicast: false,
              allowInvalidTlsForDevelopment: false,
              browserAmbientCredentials: false,
              browserOpaqueWebSocketRedirects: false,
              credentials: [],
            },
            providers: {
              backendByRole: {},
              tlsByRole: {},
              netDriverId: "net.driver.test",
            },
            resources: { minimum: {} },
          },
        }
      : {}),
  };
}

afterEach(() => {
  delete globals()[MARKER];
  for (const name of GLOBAL_BINDING_NAMES) delete globals()[name];
});

describe("network factory admission", () => {
  test("requires the verified network plan and factory flag to agree", () => {
    const plainPlan = finalizeBuildPlan(content(false));
    const networkPlan = finalizeBuildPlan(content(true));

    expect(selectBundleArtifactMode(undefined, false)).toBe("iife");
    expect(selectBundleArtifactMode(plainPlan, false)).toBe("iife");
    expect(selectBundleArtifactMode(networkPlan, true)).toBe("network-factory");
    expect(() => selectBundleArtifactMode(networkPlan, false)).toThrow(
      "requires a factory-aware loader",
    );
    expect(() => selectBundleArtifactMode(plainPlan, true)).toThrow(
      "requires a ResolvedBuildPlan with network admission",
    );
    expect(() => selectBundleArtifactMode(undefined, true)).toThrow(
      "requires a ResolvedBuildPlan with network admission",
    );
  });

  test("rejects a modified plan before selecting an artifact ABI", () => {
    const plan = finalizeBuildPlan(content(true));
    expect(() => selectBundleArtifactMode({
      ...plan,
      target: { ...plan.target, id: "modified" },
    }, true)).toThrow("invalid ResolvedBuildPlan checksum");
  });

  test("adds the private lexical define only to network factories", () => {
    expect(networkFactoryDefines("iife")).toEqual({});
    expect(networkFactoryDefines("network-factory")).toEqual({
      [NETWORK_BINDING_DEFINE]: NETWORK_BINDING_FACTORY_PARAMETER,
    });
  });
});

describe("network factory artifact", () => {
  test("preserves a non-network IIFE byte-for-byte", () => {
    const source = "\uFEFF(() => { globalThis.legacy = true; })();\n";
    expect(finalizeBundleArtifact(source, "iife")).toBe(source);
  });

  test("defers Bun's IIFE and exposes the binding only as a lexical value", async () => {
    const directory = await mkdtemp(join(tmpdir(), "pocketjs-network-factory-"));
    try {
      const entry = join(directory, "entry.ts");
      const bindingModule = join(directory, "binding.ts");
      await Bun.write(bindingModule, `
        declare const ${NETWORK_BINDING_DEFINE}: unknown;
        export const capturedBinding = ${NETWORK_BINDING_DEFINE};
      `);
      await Bun.write(entry, `
        import { capturedBinding } from "./binding.ts";
        const ${NETWORK_BINDING_FACTORY_PARAMETER} = "application shadow";
        (globalThis as Record<string, unknown>).${MARKER} = {
          binding: capturedBinding,
          applicationValue: ${NETWORK_BINDING_FACTORY_PARAMETER},
          sourceGlobal: Object.prototype.hasOwnProperty.call(
            globalThis,
            ${JSON.stringify(NETWORK_BINDING_DEFINE)},
          ),
          parameterGlobal: Object.prototype.hasOwnProperty.call(
            globalThis,
            ${JSON.stringify(NETWORK_BINDING_FACTORY_PARAMETER)},
          ),
        };
      `);
      const result = await Bun.build({
        entrypoints: [entry],
        format: "iife",
        target: "browser",
        define: networkFactoryDefines("network-factory"),
      });
      expect(result.success).toBe(true);
      const source = await result.outputs[0]!.text();
      const factory = evaluateArtifact(wrapNetworkBundleFactory(source));

      expect(typeof factory).toBe("function");
      expect((factory as Function).length).toBe(1);
      expect(globals()[MARKER]).toBeUndefined();

      const binding = Object.freeze({ abiMajor: 1, abiMinor: 0 });
      expect((factory as (binding: object) => unknown)(binding)).toBeUndefined();
      expect(globals()[MARKER]).toEqual({
        binding,
        applicationValue: "application shadow",
        sourceGlobal: false,
        parameterGlobal: false,
      });
      for (const name of GLOBAL_BINDING_NAMES) {
        expect(Object.prototype.hasOwnProperty.call(globalThis, name)).toBe(false);
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  test("builds a PocketJS app whose mount and frame checkpoint start at factory call", async () => {
    const directory = await mkdtemp(join(tmpdir(), "pocketjs-network-build-e2e-"));
    const output = join(directory, "dist");
    const app = join(directory, "app.tsx");
    const planPath = join(directory, "resolved-plan.json");
    const priorUi = globals().ui;
    const priorFrame = globals().frame;
    try {
      await Bun.write(app, `
        import { Text, View } from "@pocketjs/framework/components";
        import { onFrame } from "@pocketjs/framework/lifecycle";
        import { mount } from "@pocketjs/framework/solid";
        import ${JSON.stringify(join(ROOT, "framework/src/net/http-binding.ts"))};

        const checkpoint = (globalThis as Record<string, any>).${MARKER};
        checkpoint.initializers++;
        mount(() => {
          onFrame(() => checkpoint.frames++);
          return <View><Text>factory ready</Text></View>;
        });
        checkpoint.mounted = true;
      `);
      const plan = finalizeBuildPlan({
        ...content(true),
        app: {
          ...content(true).app,
          entry: "app.tsx",
          output: "network-factory-e2e",
        },
      });
      await Bun.write(planPath, `${JSON.stringify(plan)}\n`);

      const buildProcess = Bun.spawn([
        process.execPath,
        "tools/build.ts",
        `--plan=${planPath}`,
        `--project-root=${directory}`,
        `--outdir=${output}`,
        "--no-config",
        "--network-factory",
      ], {
        cwd: ROOT,
        stdout: "pipe",
        stderr: "pipe",
      });
      const [exitCode, stdout, stderr] = await Promise.all([
        buildProcess.exited,
        new Response(buildProcess.stdout).text(),
        new Response(buildProcess.stderr).text(),
      ]);
      expect(exitCode, `${stdout}\n${stderr}`).toBe(0);

      globals()[MARKER] = { initializers: 0, frames: 0, mounted: false };
      let nextNode = 2;
      globals().ui = {
        __host: "esp-test",
        __hostAbi: 1,
        __textures: Object.freeze({}),
        createNode: () => nextNode++,
        destroyNode: () => {},
        insertBefore: () => {},
        removeChild: () => {},
        setStyle: () => {},
        setProp: () => {},
        setText: () => {},
        replaceText: () => {},
        uploadTexture: () => 1,
        setImage: () => {},
        setSprite: () => {},
        animate: () => 1,
        cancelAnim: () => {},
        setFocus: () => {},
        measureText: () => 0,
      };
      delete globals().frame;

      const artifact = await Bun.file(
        join(output, "network-factory-e2e.js"),
      ).text();
      const factory = evaluateArtifact(artifact);
      expect(typeof factory).toBe("function");
      expect(globals()[MARKER]).toEqual({
        initializers: 0,
        frames: 0,
        mounted: false,
      });
      expect(globals().frame).toBeUndefined();

      const binding = Object.freeze({
        abiMajor: 1,
        abiMinor: 0,
        featureSet: Object.freeze(["network.http.client"]),
        start: () => {
          throw new Error("unused test binding");
        },
      });
      expect((factory as (value: object) => unknown)(binding)).toBeUndefined();
      expect(globals()[MARKER]).toEqual({
        initializers: 1,
        frames: 0,
        mounted: true,
      });
      expect(typeof globals().frame).toBe("function");
      (globals().frame as (buttons: number) => void)(0);
      expect(globals()[MARKER]).toEqual({
        initializers: 1,
        frames: 1,
        mounted: true,
      });
      for (const name of GLOBAL_BINDING_NAMES) {
        expect(Object.prototype.hasOwnProperty.call(globalThis, name)).toBe(false);
      }
    } finally {
      if (priorUi === undefined) delete globals().ui;
      else globals().ui = priorUi;
      if (priorFrame === undefined) delete globals().frame;
      else globals().frame = priorFrame;
      await rm(directory, { recursive: true, force: true });
    }
  });

  test("consumes the factory on an invalid first call without running the IIFE", () => {
    const factory = evaluateArtifact(wrapNetworkBundleFactory(
      `globalThis.${MARKER} = true;`,
    )) as (...args: unknown[]) => void;

    expect(() => factory({})).toThrow("requires a frozen binding table");
    expect(globals()[MARKER]).toBeUndefined();
    expect(() => factory(Object.freeze({}))).toThrow("already invoked");
    expect(globals()[MARKER]).toBeUndefined();
  });

  test("requires exactly one argument and does not retry a throwing initializer", () => {
    const missing = evaluateArtifact(wrapNetworkBundleFactory(
      `globalThis.${MARKER} = "missing-ran";`,
    )) as (...args: unknown[]) => void;
    expect(() => missing()).toThrow("requires exactly one binding argument");
    expect(globals()[MARKER]).toBeUndefined();

    const extra = evaluateArtifact(wrapNetworkBundleFactory(
      `globalThis.${MARKER} = "extra-ran";`,
    )) as (...args: unknown[]) => void;
    expect(() => extra(Object.freeze({}), Object.freeze({}))).toThrow(
      "requires exactly one binding argument",
    );
    expect(globals()[MARKER]).toBeUndefined();

    const throwing = evaluateArtifact(wrapNetworkBundleFactory(`
      globalThis.${MARKER} = ((globalThis.${MARKER} ?? 0) + 1);
      throw new Error("initializer failed");
    `)) as (...args: unknown[]) => void;
    expect(() => throwing(Object.freeze({}))).toThrow("initializer failed");
    expect(globals()[MARKER]).toBe(1);
    expect(() => throwing(Object.freeze({}))).toThrow("already invoked");
    expect(globals()[MARKER]).toBe(1);
  });
});
