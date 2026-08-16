import { createHash } from "node:crypto";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, test } from "bun:test";
import {
  networkV1FeatureIdsFromBuildPlan,
  networkV1PlanHashBytes,
} from "../../../../contracts/spec/network/network-v1.ts";
import { transformFile } from "../../../../framework/compiler/jsx-plugin.ts";
import {
  finalizeBuildPlan,
  verifyPlanHash,
  type ResolvedBuildPlan,
} from "../../../../framework/src/manifest/plan.ts";
import { createNetworkFactoryBuildContext } from
  "../../../../tools/network-bundle-factory.ts";
import { generate } from "./generate.ts";

const COMPONENT = dirname(fileURLToPath(import.meta.url));
const ROOT = join(COMPONENT, "../../../..");
const GENERATED = join(COMPONENT, "generated");
const EXPECTED_PLAN_HASH =
  "sha256:04856acc82e7aa31648b015e62a63a4cadf6f48a3d1d3f46f3987539520b63fd";

async function readPlan(): Promise<ResolvedBuildPlan> {
  return await Bun.file(join(GENERATED, "resolved-plan.json")).json();
}

async function spawnBuild(
  planPath: string,
  output: string,
  permit: boolean,
): Promise<{ exitCode: number; output: string }> {
  const stdoutPath = `${output}.stdout.log`;
  const stderrPath = `${output}.stderr.log`;
  const child = Bun.spawn({
    cmd: [
      "bun",
      join(ROOT, "tools/build.ts"),
      `--plan=${planPath}`,
      `--project-root=${COMPONENT}`,
      `--outdir=${output}`,
      "--no-config",
      "--network-factory",
      ...(permit ? ["--test-only-staged-http-client-fetch"] : []),
    ],
    cwd: ROOT,
    stdout: Bun.file(stdoutPath),
    stderr: Bun.file(stderrPath),
  });
  const exitCode = await child.exited;
  const [stdout, stderr] = await Promise.all([
    Bun.file(stdoutPath).text(),
    Bun.file(stderrPath).text(),
  ]);
  return {
    exitCode,
    output: `exit=${exitCode} signal=${String(child.signalCode)}\n${stdout}\n${stderr}`,
  };
}

describe("ESP formal network smoke artifact", () => {
  test("is reproducible and carries exact handshake and endpoint metadata", async () => {
    await generate(true);
    const [plan, metadata, binary, metadataSource] = await Promise.all([
      readPlan(),
      Bun.file(join(GENERATED, "metadata.json")).json(),
      Bun.file(join(GENERATED, "factory.js.bin")).arrayBuffer(),
      Bun.file(join(GENERATED, "formal_smoke_metadata.c")).text(),
    ]);
    expect(verifyPlanHash(plan)).toBe(true);
    expect(plan.planHash).toBe(EXPECTED_PLAN_HASH);
    expect(metadata.planHashBytes).toEqual(Array.from(networkV1PlanHashBytes(plan.planHash)));
    expect(metadata.featureIds).toEqual(Array.from(
      networkV1FeatureIdsFromBuildPlan(plan.features),
    ));
    expect(metadata.featureIds).toEqual([0x0100]);
    expect(metadata.providers).toEqual({
      httpClientBackendId: "pocketjs.net.http-client-core.v1.experimental",
      netDriverId: "pocketjs.net.esp-idf.transport.v1.experimental",
    });
    expect(plan.network?.policy.connect).toEqual([{
      protocol: "http",
      host: "172.16.10.126",
      port: { min: 8088, max: 8088 },
    }]);
    expect(metadata.endpoint.origin).toBe("http://172.16.10.126:8088");
    expect(metadataSource).toContain('asm("_binary_factory_js_bin_start")');
    expect(metadataSource).toContain(
      "pocketjs_net_formal_smoke_http_client_backend_id",
    );
    expect(metadataSource).toContain("pocketjs_net_formal_smoke_net_driver_id");
    expect(metadataSource).not.toContain("_binary_generated_factory_js_bin_start");

    const storage = new Uint8Array(binary);
    expect(storage.at(-1)).toBe(0);
    expect(storage.subarray(0, -1).includes(0)).toBe(false);
    expect(storage.length).toBe(metadata.factory.storageBytes);
    expect(storage.length - 1).toBe(metadata.factory.sourceBytes);
    expect(`sha256:${createHash("sha256").update(storage.subarray(0, -1)).digest("hex")}`)
      .toBe(metadata.factory.sha256);
  });

  test("keeps the staged permit narrow and out of normal build contexts", async () => {
    const plan = await readPlan();
    const normal = createNetworkFactoryBuildContext(plan);
    expect(normal.testOnlyStagedHttpClientFetch).toBeUndefined();
    await expect(transformFile(
      "/virtual/formal-smoke-default.ts",
      'import { fetch } from "@pocketjs/framework/net/http"; void fetch;',
      "solid",
      { features: plan.features, networkPrivate: normal },
    )).rejects.toThrow("staged surface");

    const permitted = Object.freeze({
      ...normal,
      testOnlyStagedHttpClientFetch: true as const,
    });
    await expect(transformFile(
      "/virtual/formal-smoke-permitted.ts",
      'import { fetch } from "@pocketjs/framework/net/http"; void fetch;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).resolves.toBeDefined();
    await expect(transformFile(
      "/virtual/formal-smoke-headers-attack.ts",
      'import { Headers } from "@pocketjs/framework/net/http"; void Headers;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).rejects.toThrow("staged surface");
    await expect(transformFile(
      "/virtual/formal-smoke-serve-attack.ts",
      'import { serve } from "@pocketjs/framework/net/http"; void serve;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).rejects.toThrow("network.http.server");
    await expect(transformFile(
      "/virtual/formal-smoke-namespace-attack.ts",
      'import * as http from "@pocketjs/framework/net/http"; void http.fetch;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).rejects.toThrow();
  });

  test("rejects the permit outside the exact plan snapshot", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "pocketjs-smoke-gate-"));
    try {
      const defaultResult = await spawnBuild(
        join(GENERATED, "resolved-plan.json"),
        join(temporary, "default"),
        false,
      );
      expect(defaultResult.exitCode).not.toBe(0);
      expect(defaultResult.output).toContain("staged surface");

      const plan = await readPlan();
      const { planHash: _ignored, ...content } = plan;
      const wrongTarget = finalizeBuildPlan({
        ...content,
        target: { ...content.target, id: "esp-formal-network-smoke-wrong" },
      });
      const wrongPath = join(temporary, "wrong-plan.json");
      await Bun.write(wrongPath, `${JSON.stringify(wrongTarget)}\n`);
      const wrongResult = await spawnBuild(
        wrongPath,
        join(temporary, "wrong"),
        true,
      );
      expect(wrongResult.exitCode).not.toBe(0);
      expect(wrongResult.output).toContain(
        "restricted to the exact ESP formal network smoke plan and entry",
      );

      const changedPolicy = finalizeBuildPlan({
        ...content,
        network: {
          ...content.network!,
          policy: {
            ...content.network!.policy,
            connect: [{
              protocol: "http",
              host: "172.16.10.127",
              port: { min: 8088, max: 8088 },
            }],
          },
        },
      });
      const changedPath = join(temporary, "changed-policy.json");
      await Bun.write(changedPath, `${JSON.stringify(changedPolicy)}\n`);
      const changedResult = await spawnBuild(
        changedPath,
        join(temporary, "changed"),
        true,
      );
      expect(changedResult.exitCode).not.toBe(0);
      expect(changedResult.output).toContain(
        "restricted to the exact ESP formal network smoke plan and entry",
      );

      const changedResources = finalizeBuildPlan({
        ...content,
        network: {
          ...content.network!,
          resources: {
            minimum: {
              ...content.network!.resources.minimum,
              http: {
                ...content.network!.resources.minimum.http!,
                headerBytes: 4097,
              },
            },
          },
        },
      });
      const changedResourcesPath = join(temporary, "changed-resources-plan.json");
      await Bun.write(changedResourcesPath, `${JSON.stringify(changedResources)}\n`);
      const changedResourcesResult = await spawnBuild(
        changedResourcesPath,
        join(temporary, "changed-resources"),
        true,
      );
      expect(changedResourcesResult.exitCode).not.toBe(0);
      expect(changedResourcesResult.output).toContain(
        "restricted to the exact ESP formal network smoke plan and entry",
      );
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  });

  test("mounts only at factory invocation and publishes a bounded refusal report", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "pocketjs-smoke-probe-"));
    try {
      const stdoutPath = join(temporary, "stdout.log");
      const stderrPath = join(temporary, "stderr.log");
      const child = Bun.spawn({
        cmd: ["bun", join(COMPONENT, "runtime_probe.ts")],
        cwd: ROOT,
        stdout: Bun.file(stdoutPath),
        stderr: Bun.file(stderrPath),
      });
      const exitCode = await child.exited;
      const [stdout, stderr] = await Promise.all([
        Bun.file(stdoutPath).text(),
        Bun.file(stderrPath).text(),
      ]);
      expect(
        exitCode,
        `exit=${exitCode} signal=${String(child.signalCode)}\n${stdout}\n${stderr}`,
      ).toBe(0);
      expect(JSON.parse(stdout)).toMatchObject({
        phase: "failed",
        done: true,
        ok: false,
        roundsTotal: 20,
        frameCalls: 0,
        errorCode: "permission_denied",
      });
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  });
});
