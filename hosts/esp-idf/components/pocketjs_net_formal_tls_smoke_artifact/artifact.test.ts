import { createHash, X509Certificate } from "node:crypto";
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
  "sha256:9240cfa29c1678b49b6fed67104a39b2ad32f5dedab372af1c2a0bde3d602654";
const EXPECTED_CA_DER_SHA256 =
  "sha256:318ae57f0fb82d12cf86431571fb6ec3556ecb74f530a5be6f741a482b5447af";

type Permit = "none" | "http" | "https" | "both";

async function readPlan(): Promise<ResolvedBuildPlan> {
  return await Bun.file(join(GENERATED, "resolved-plan.json")).json();
}

async function spawnBuild(
  planPath: string,
  output: string,
  permit: Permit,
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
  const path = join(temporary, `${label}.json`);
  await Bun.write(path, `${JSON.stringify(plan)}\n`);
  const result = await spawnBuild(path, join(temporary, label), "https");
  expect(result.exitCode).not.toBe(0);
  expect(result.output).toContain(
    "restricted to the exact ESP formal TLS network smoke plan and entry",
  );
}

describe("ESP formal TLS network smoke artifact", () => {
  test("rejects legacy and ambiguous Host IPv4 spellings", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "pocketjs-tls-ipv4-test-"));
    try {
      const binary = join(temporary, "ipv4-validation-test");
      const child = Bun.spawn({
        cmd: [
          "clang",
          "-std=c11",
          "-Wall",
          "-Wextra",
          "-Werror",
          `-I${join(COMPONENT, "test_host/fake_include")}`,
          `-I${join(COMPONENT, "private")}`,
          join(COMPONENT, "src/formal_tls_smoke_ipv4.c"),
          join(COMPONENT, "test_host/ipv4_validation_test.c"),
          "-o",
          binary,
        ],
        cwd: ROOT,
        stdout: "inherit",
        stderr: "inherit",
      });
      expect(await child.exited).toBe(0);
      expect(await Bun.spawn({ cmd: [binary] }).exited).toBe(0);
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  });

  test("is reproducible and carries exact TLS, CA, and factory metadata", async () => {
    await generate(true);
    const [plan, metadata, binary, metadataSource, pem] = await Promise.all([
      readPlan(),
      Bun.file(join(GENERATED, "metadata.json")).json(),
      Bun.file(join(GENERATED, "factory.js.bin")).arrayBuffer(),
      Bun.file(join(GENERATED, "formal_tls_smoke_metadata.c")).text(),
      Bun.file(join(COMPONENT, "fixtures/ca.cert.pem")).text(),
    ]);
    expect(verifyPlanHash(plan)).toBe(true);
    expect(plan.planHash).toBe(EXPECTED_PLAN_HASH);
    expect(metadata.planHashBytes).toEqual(Array.from(networkV1PlanHashBytes(plan.planHash)));
    expect(metadata.featureIds).toEqual(Array.from(
      networkV1FeatureIdsFromBuildPlan(plan.features),
    ));
    expect(metadata.featureIds).toEqual([0x0100, 0x0101]);
    expect(metadata.providers).toEqual({
      httpClientBackendId: "pocketjs.net.http-client-core.v1.experimental",
      netDriverId: "pocketjs.net.esp-idf.transport.v1.experimental",
      tlsProviderId: "pocketjs.net.esp-idf.esp-tls.v1.experimental",
    });
    expect(plan.network?.policy.connect).toEqual([{
      protocol: "https",
      host: "pocketjs.test",
      port: { min: 8443, max: 8443 },
    }]);
    expect(plan.network?.providers).toEqual({
      backendByRole: {
        "http.client": "pocketjs.net.http-client-core.v1.experimental",
      },
      tlsByRole: {
        "http.client": {
          source: "provider",
          id: "pocketjs.net.esp-idf.esp-tls.v1.experimental",
        },
      },
      netDriverId: "pocketjs.net.esp-idf.transport.v1.experimental",
    });
    expect(metadata.tls).toMatchObject({
      trustSource: "host-pinned-ca",
      minVersion: "1.2",
      maxVersion: "1.2",
      verification: "full",
      revocation: "host-default",
      caDerSha256: EXPECTED_CA_DER_SHA256,
    });
    const certificate = new X509Certificate(pem);
    expect(certificate.ca).toBe(true);
    expect(`sha256:${createHash("sha256").update(certificate.raw).digest("hex")}`)
      .toBe(EXPECTED_CA_DER_SHA256);
    expect(metadata.tls.caPemBytes).toBe(Buffer.byteLength(pem));
    expect(metadataSource).toContain("pocketjs_net_formal_tls_smoke_ca_pem");
    expect(metadataSource).toContain(
      "pocketjs_net_formal_tls_smoke_http_client_backend_id",
    );
    expect(metadataSource).toContain(
      "pocketjs_net_formal_tls_smoke_net_driver_id",
    );
    expect(metadataSource).toContain(EXPECTED_CA_DER_SHA256);
    expect(metadata.reportGlobal).toBe("__pocketjsFormalNetworkTlsSmokeReportV1");
    expect(metadata.cancelGlobal).toBe("__pocketjsFormalNetworkTlsSmokeCancelV1");
    expect(metadataSource).toContain("pocketjs_net_formal_tls_smoke_cancel_global");

    const storage = new Uint8Array(binary);
    expect(storage.at(-1)).toBe(0);
    expect(storage.subarray(0, -1).includes(0)).toBe(false);
    expect(storage.length).toBe(metadata.factory.storageBytes);
    expect(storage.length - 1).toBe(metadata.factory.sourceBytes);
    expect(`sha256:${createHash("sha256").update(storage.subarray(0, -1)).digest("hex")}`)
      .toBe(metadata.factory.sha256);
  });

  test("opens only fetch for the distinct TLS permit", async () => {
    const plan = await readPlan();
    const normal = createNetworkFactoryBuildContext(plan);
    expect(normal.testOnlyStagedHttpClientFetch).toBeUndefined();
    expect(normal.testOnlyStagedHttpsClientFetch).toBeUndefined();
    await expect(transformFile(
      "/virtual/formal-tls-smoke-default.ts",
      'import { fetch } from "@pocketjs/framework/net/http"; void fetch;',
      "solid",
      { features: plan.features, networkPrivate: normal },
    )).rejects.toThrow("staged surface");

    const permitted = Object.freeze({
      ...normal,
      testOnlyStagedHttpsClientFetch: true as const,
    });
    await expect(transformFile(
      "/virtual/formal-tls-smoke-permitted.ts",
      'import { fetch } from "@pocketjs/framework/net/http"; void fetch;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).resolves.toBeDefined();
    await expect(transformFile(
      "/virtual/formal-tls-smoke-headers-attack.ts",
      'import { Headers } from "@pocketjs/framework/net/http"; void Headers;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).rejects.toThrow("staged surface");
    await expect(transformFile(
      "/virtual/formal-tls-smoke-serve-attack.ts",
      'import { serve } from "@pocketjs/framework/net/http"; void serve;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).rejects.toThrow("network.http.server");
    await expect(transformFile(
      "/virtual/formal-tls-smoke-namespace-attack.ts",
      'import * as http from "@pocketjs/framework/net/http"; void http.fetch;',
      "solid",
      { features: plan.features, networkPrivate: permitted },
    )).rejects.toThrow();
  });

  test("rejects default, cross-permit, and all exact-plan attacks", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "pocketjs-tls-smoke-gate-"));
    try {
      const planPath = join(GENERATED, "resolved-plan.json");
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

      const plan = await readPlan();
      const { planHash: _ignored, ...content } = plan;
      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        target: { ...content.target, id: "esp-formal-network-tls-smoke-wrong" },
      }), temporary, "wrong-target");

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
      }), temporary, "wrong-policy");

      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        network: {
          ...content.network!,
          providers: {
            ...content.network!.providers,
            tlsByRole: {
              "http.client": {
                source: "provider",
                id: "pocketjs.net.attacker.tls.v1",
              },
            },
          },
        },
      }), temporary, "wrong-provider");

      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        features: {
          ...content.features,
          "network.http.client.tls": false,
        },
      }), temporary, "missing-tls-feature");

      await expectPermitRefusal(finalizeBuildPlan({
        ...content,
        features: {
          ...content.features,
          "network.http.client.tls.custom-ca": true,
        },
      }), temporary, "extra-feature");
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  });

  test("mounts only at factory invocation and publishes a bounded TLS refusal report", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "pocketjs-tls-smoke-probe-"));
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
