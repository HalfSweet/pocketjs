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
import {
  finalizeBuildPlan,
  verifyPlanHash,
  type ResolvedBuildPlan,
} from "../../../../framework/src/manifest/plan.ts";
import { generate } from "./generate.ts";

const COMPONENT = dirname(fileURLToPath(import.meta.url));
const ROOT = join(COMPONENT, "../../../..");
const GENERATED = join(COMPONENT, "generated");
const TLS_SMOKE = join(
  COMPONENT,
  "../pocketjs_net_formal_tls_smoke_artifact",
);
const EXPECTED_PLAN_HASH =
  "sha256:fe3014e4d3628eb60aaeedd414432eb8c9a5932e904b258a9d05a17c7f6abcce";

type Permit = "none" | "http" | "https" | "both";

async function readPlan(path = join(GENERATED, "resolved-plan.json")):
  Promise<ResolvedBuildPlan> {
  return await Bun.file(path).json();
}

async function spawnBuild(
  planPath: string,
  output: string,
  permit: Permit,
  projectRoot = COMPONENT,
): Promise<{ exitCode: number; output: string }> {
  const stdoutPath = `${output}.stdout.log`;
  const stderrPath = `${output}.stderr.log`;
  const child = Bun.spawn({
    cmd: [
      "bun",
      join(ROOT, "tools/build.ts"),
      `--plan=${planPath}`,
      `--project-root=${projectRoot}`,
      `--outdir=${output}`,
      "--no-config",
      "--network-factory",
      ...(permit === "http" || permit === "both"
        ? ["--test-only-staged-http-client-fetch"]
        : []),
      ...(permit === "https" || permit === "both"
        ? ["--test-only-staged-https-client-fetch"]
        : []),
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

async function expectPermitRefusal(
  plan: ResolvedBuildPlan,
  temporary: string,
  label: string,
): Promise<void> {
  const planPath = join(temporary, `${label}.json`);
  await Bun.write(planPath, `${JSON.stringify(plan)}\n`);
  const result = await spawnBuild(planPath, join(temporary, label), "https");
  expect(result.exitCode).not.toBe(0);
  expect(result.output).toContain(
    "restricted to the exact ESP formal TLS test plans and entries",
  );
}

describe("ESP formal TLS conformance artifact", () => {
  test("is reproducible and uses a distinct exact TLS plan", async () => {
    await generate(true);
    const [plan, metadata, binary, source] = await Promise.all([
      readPlan(),
      Bun.file(join(GENERATED, "metadata.json")).json(),
      Bun.file(join(GENERATED, "factory.js.bin")).arrayBuffer(),
      Bun.file(join(GENERATED, "formal_tls_smoke_metadata.c")).text(),
    ]);
    expect(verifyPlanHash(plan)).toBe(true);
    expect(plan.planHash).toBe(EXPECTED_PLAN_HASH);
    expect(plan.planHash).not.toBe((await readPlan(
      join(TLS_SMOKE, "generated/resolved-plan.json"),
    )).planHash);
    expect(metadata.planHashBytes).toEqual(
      Array.from(networkV1PlanHashBytes(plan.planHash)),
    );
    expect(metadata.featureIds).toEqual(
      Array.from(networkV1FeatureIdsFromBuildPlan(plan.features)),
    );
    expect(metadata.featureIds).toEqual([0x0100, 0x0101]);
    expect(metadata.target).toEqual({
      id: "esp-formal-network-tls-conformance-test",
      hostAbi: 1,
    });
    expect(metadata.endpoint.origin).toBe("https://pocketjs.test:8443");
    expect(metadata.tls).toMatchObject({
      minVersion: "1.2",
      maxVersion: "1.2",
      verification: "full",
      caDerSha256:
        "sha256:318ae57f0fb82d12cf86431571fb6ec3556ecb74f530a5be6f741a482b5447af",
    });
    expect(metadata.stagedSurfaceBuild).toMatchObject({
      exactAppId: "dev.pocketjs.esp-formal-network-tls-conformance",
      exactTargetId: "esp-formal-network-tls-conformance-test",
      productionGateChanged: false,
    });
    const storage = new Uint8Array(binary);
    expect(storage.at(-1)).toBe(0);
    expect(storage.subarray(0, -1).includes(0)).toBe(false);
    expect(`sha256:${createHash("sha256").update(storage.subarray(0, -1)).digest("hex")}`)
      .toBe(metadata.factory.sha256);
    expect(source).toContain(EXPECTED_PLAN_HASH);
    expect(source).toContain('asm("_binary_factory_js_bin_start")');
  });

  test("the HTTPS permit accepts only its exact plan and entry", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "pocketjs-tls-conformance-gate-"));
    try {
      const planPath = join(GENERATED, "resolved-plan.json");
      const permitted = await spawnBuild(
        planPath,
        join(temporary, "permitted"),
        "https",
      );
      expect(permitted.exitCode, permitted.output).toBe(0);

      const defaultResult = await spawnBuild(
        planPath,
        join(temporary, "default"),
        "none",
      );
      expect(defaultResult.exitCode).not.toBe(0);
      expect(defaultResult.output).toContain("staged surface");

      const oldPermit = await spawnBuild(
        planPath,
        join(temporary, "old-permit"),
        "http",
      );
      expect(oldPermit.exitCode).not.toBe(0);
      expect(oldPermit.output).toContain(
        "restricted to the exact ESP formal network smoke plan and entry",
      );

      const both = await spawnBuild(planPath, join(temporary, "both"), "both");
      expect(both.exitCode).not.toBe(0);
      expect(both.output).toContain("mutually exclusive");

      const oldPlanPath = join(TLS_SMOKE, "generated/resolved-plan.json");
      const crossed = await spawnBuild(
        oldPlanPath,
        join(temporary, "crossed"),
        "https",
      );
      expect(crossed.exitCode).not.toBe(0);
      expect(crossed.output).toContain(
        "restricted to the exact ESP formal TLS test plans and entries",
      );
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  });

  test("rejects policy, provider, feature, target, and app mutations", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "pocketjs-tls-conformance-attacks-"));
    try {
      const plan = await readPlan();
      const { planHash: _ignored, ...content } = plan;
      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        target: { ...content.target, id: "esp-formal-network-tls-conformance-wrong" },
      }), temporary, "target");
      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        app: { ...content.app, id: "dev.pocketjs.attacker" },
      }), temporary, "app");
      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        features: { ...content.features, "network.http.client.tls": false },
      }), temporary, "feature");
      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        network: {
          ...content.network!,
          policy: {
            ...content.network!.policy,
            connect: [{
              protocol: "https",
              host: "wrong.pocketjs.test",
              port: { min: 8443, max: 8443 },
            }],
          },
        },
      }), temporary, "policy");
      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        network: {
          ...content.network!,
          providers: {
            ...content.network!.providers,
            tlsByRole: {
              "http.client": { source: "provider", id: "pocketjs.net.attacker" },
            },
          },
        },
      }), temporary, "provider");
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  });
});
