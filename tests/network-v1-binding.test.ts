import { describe, expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import {
  NETWORK_V1_ABI_MAJOR,
  NETWORK_V1_ABI_MINOR,
  NETWORK_V1_LIMIT_PROTOCOL_ANY,
  NETWORK_V1_LIMIT_ROLE_ANY,
  NETWORK_V1_UINT32_MAX,
  NetworkV1CommandOpcode,
  NetworkV1CompletionPollStatus,
  NetworkV1DispatchStatus,
  NetworkV1ErrorCategory,
  NetworkV1ErrorCode,
  NetworkV1EventCode,
  NetworkV1FeatureId,
  NetworkV1LimitProtocol,
  NetworkV1LimitRole,
  NetworkV1ServiceTurnKind,
  NetworkV1ServiceTurnStatus,
  type NetworkV1AsyncCommand,
  type NetworkV1BindingTable,
  type NetworkV1BufferLeaseReadIntoCommand,
  type NetworkV1BufferLeaseReleaseCommand,
  type NetworkV1BufferLeaseTakeCommand,
  type NetworkV1Completion,
  type NetworkV1CompletionIdentity,
  type NetworkV1Handle,
  type NetworkV1LimitsQuery,
  type NetworkV1ServiceDispatcher,
} from "../contracts/spec/network/network-v1.ts";
import {
  createNetworkV1HttpBindingAdapterForTesting,
  type NetworkV1CompiledExpectation,
} from "../framework/src/net/network-v1-binding.ts";
import {
  NetworkV1CommandOpcode as HighCommandOpcode,
  type HttpRequestStartCommand,
} from "../framework/src/net/http-binding.ts";
import { AbortController } from "../framework/src/net/abort.ts";
import type { BodyStream, HttpBodyProducer } from "../framework/src/net/http-body.ts";

const RUNTIME_GENERATION = 7;
const PLAN_HASH = Object.freeze(new Array<number>(32).fill(0x5a));
const FEATURE_IDS = Object.freeze([NetworkV1FeatureId.HttpClient]);
const EXPECTED: NetworkV1CompiledExpectation = Object.freeze({
  planHashBytes: PLAN_HASH,
  featureIds: FEATURE_IDS,
});
const ABSENT = Object.freeze({ id: 0, generation: 0 });

function failure(
  code: NetworkV1ErrorCode = NetworkV1ErrorCode.SystemError,
) {
  return Object.freeze({
    category: code === NetworkV1ErrorCode.HttpProtocolError
      ? NetworkV1ErrorCategory.Protocol
      : NetworkV1ErrorCategory.Runtime,
    code,
    operation: "http.fetch",
    temporary: false,
  });
}

function highStart(operationId: number, hasBody = false): HttpRequestStartCommand {
  return Object.freeze({
    opcode: HighCommandOpcode.HttpRequestStart,
    operationId,
    url: "http://example.test/",
    method: "POST",
    headers: Object.freeze([]),
    hasBody,
    redirect: "follow",
    maxRedirects: 5,
    ref: true,
  });
}

function completionIdentity(
  start: Extract<NetworkV1AsyncCommand, {
    opcode: typeof NetworkV1CommandOpcode.HttpRequestStart;
  }>,
  body: NetworkV1Handle,
  sequence: number,
): NetworkV1CompletionIdentity {
  return Object.freeze({
    runtimeGeneration: RUNTIME_GENERATION,
    resource: start.identity.resource,
    operation: start.identity.operation,
    body,
    sequence,
  });
}

class FakeHost {
  readonly commands: NetworkV1AsyncCommand[] = [];
  readonly completions: NetworkV1Completion[] = [];
  readonly leases = new Map<string, { bytes: Uint8Array; state: "queued" | "taken" | "released" }>();
  readonly table: NetworkV1BindingTable;
  dispatcher?: NetworkV1ServiceDispatcher;
  registerCount = 0;
  releaseCount = 0;
  #turnId = 0;
  #leaseId = 0;

  constructor(
    featureIds: readonly NetworkV1FeatureId[] = FEATURE_IDS,
    planHash: readonly number[] = PLAN_HASH,
  ) {
    const handshake = Object.freeze({
      abiMajor: NETWORK_V1_ABI_MAJOR,
      abiMinor: NETWORK_V1_ABI_MINOR,
      runtimeGeneration: RUNTIME_GENERATION,
      planHash: Uint8Array.from(planHash),
      featureIds: Object.freeze(Array.from(featureIds)),
    });
    this.table = Object.freeze({
      handshake,
      getLimits: (query: NetworkV1LimitsQuery) => this.getLimits(query),
      dispatch: (command: NetworkV1AsyncCommand) => {
        this.commands.push(command);
        return Object.freeze({ status: NetworkV1DispatchStatus.Accepted });
      },
      nextCompletion: ({ maxPayloadBytes }: { maxPayloadBytes: number }) => {
        const completion = this.completions[0];
        if (!completion) {
          return Object.freeze({
            status: NetworkV1CompletionPollStatus.Drained,
            payloadBytesDelivered: 0 as const,
          });
        }
        const payload = completion.eventCode === NetworkV1EventCode.BodyChunk
          ? completion.payload.byteLength
          : 0;
        if (maxPayloadBytes === 0 || payload > maxPayloadBytes) {
          return Object.freeze({
            status: NetworkV1CompletionPollStatus.BudgetExhausted,
            payloadBytesDelivered: 0 as const,
          });
        }
        this.completions.shift();
        return Object.freeze({
          status: NetworkV1CompletionPollStatus.Item,
          completion,
          payloadBytesDelivered: payload,
        });
      },
      leaseTake: (command: NetworkV1BufferLeaseTakeCommand) => {
        const lease = this.leases.get(this.leaseKey(command.lease));
        if (!lease || lease.state !== "queued" ||
          lease.bytes.byteLength !== command.byteLength) {
          return Object.freeze({
            status: NetworkV1DispatchStatus.Refused,
            error: failure(NetworkV1ErrorCode.InvalidState),
          });
        }
        lease.state = "taken";
        return Object.freeze({
          status: NetworkV1DispatchStatus.Completed,
          byteLength: lease.bytes.byteLength,
        });
      },
      leaseReadInto: (
        command: NetworkV1BufferLeaseReadIntoCommand,
        destination: Uint8Array,
      ) => {
        const lease = this.leases.get(this.leaseKey(command.lease));
        if (!lease || lease.state !== "taken") {
          return Object.freeze({
            status: NetworkV1DispatchStatus.Refused,
            error: failure(NetworkV1ErrorCode.InvalidState),
          });
        }
        const count = Math.min(destination.byteLength, lease.bytes.byteLength - command.offset);
        destination.set(lease.bytes.subarray(command.offset, command.offset + count));
        return Object.freeze({
          status: NetworkV1DispatchStatus.Completed,
          bytesCopied: count,
        });
      },
      leaseRelease: (command: NetworkV1BufferLeaseReleaseCommand) => {
        const lease = this.leases.get(this.leaseKey(command.lease));
        if (!lease || lease.state !== "taken") {
          return Object.freeze({
            status: NetworkV1DispatchStatus.Refused,
            error: failure(NetworkV1ErrorCode.InvalidState),
          });
        }
        lease.state = "released";
        this.releaseCount++;
        return Object.freeze({ status: NetworkV1DispatchStatus.Completed });
      },
      registerServiceDispatcher: (dispatcher: NetworkV1ServiceDispatcher) => {
        this.registerCount++;
        this.dispatcher = dispatcher;
      },
    });
  }

  get startCommand() {
    return this.commands.find((command): command is Extract<NetworkV1AsyncCommand, {
      opcode: typeof NetworkV1CommandOpcode.HttpRequestStart;
    }> => command.opcode === NetworkV1CommandOpcode.HttpRequestStart)!;
  }

  getLimits(query: NetworkV1LimitsQuery) {
    const http = query.protocol === NETWORK_V1_LIMIT_PROTOCOL_ANY ||
      query.protocol === NetworkV1LimitProtocol.Http;
    const client = query.role === NETWORK_V1_LIMIT_ROLE_ANY ||
      query.role === NetworkV1LimitRole.Client;
    const featureIds = http && client ? FEATURE_IDS : Object.freeze([]);
    const values = featureIds.length === 0
      ? Object.freeze([])
      : Object.freeze([
          Object.freeze({
            name: "http.maxBodyChunkBytes",
            default: 2048,
            hard: 4096,
            minimum: 512,
          }),
        ]);
    return Object.freeze({
      runtimeGeneration: RUNTIME_GENERATION,
      protocol: query.protocol,
      role: query.role,
      values,
      featureIds,
    });
  }

  headers(
    body: NetworkV1Handle = ABSENT,
    sequence = 1,
  ): NetworkV1Completion {
    return Object.freeze({
      eventCode: NetworkV1EventCode.HttpResponseHeaders,
      identity: completionIdentity(this.startCommand, body, sequence),
      metadata: Object.freeze({
        status: 200,
        statusText: "OK",
        headers: Object.freeze([Object.freeze({ name: "content-type", value: "text/plain" })]),
        url: "http://example.test/",
        redirected: false,
        bufferedBodyBytes: 4096,
      }),
    });
  }

  chunk(body: NetworkV1Handle, bytes: readonly number[], sequence: number): NetworkV1Completion {
    const lease = Object.freeze({ id: ++this.#leaseId, generation: 1 });
    this.leases.set(this.leaseKey(lease), { bytes: Uint8Array.from(bytes), state: "queued" });
    return Object.freeze({
      eventCode: NetworkV1EventCode.BodyChunk,
      identity: completionIdentity(this.startCommand, body, sequence),
      payload: Object.freeze({
        runtimeGeneration: RUNTIME_GENERATION,
        lease,
        byteLength: bytes.length,
      }),
    });
  }

  run(maxEvents = 8, maxPayloadBytes = 65_536) {
    return this.dispatcher!(Object.freeze({
      runtimeGeneration: RUNTIME_GENERATION,
      turnId: ++this.#turnId,
      kind: NetworkV1ServiceTurnKind.Network,
      maxEvents,
      maxPayloadBytes,
    }));
  }

  private leaseKey(handle: NetworkV1Handle): string {
    return `${handle.id}:${handle.generation}`;
  }
}

describe("formal network v1 mount", () => {
  test("adopts only the non-zero Host runtime generation and registers once", () => {
    const host = new FakeHost();
    const adapter = createNetworkV1HttpBindingAdapterForTesting(host.table, EXPECTED);
    expect(host.registerCount).toBe(1);
    adapter.binding.start(
      highStart(1),
      null,
      new AbortController().signal,
    );
    expect(host.startCommand.identity.runtimeGeneration).toBe(RUNTIME_GENERATION);
    expect(host.startCommand.identity.commandSequence).toBe(1);
    expect(adapter.binding.featureSet).toEqual(["network.http.client"]);
  });

  test("rejects plan/feature mismatch before dispatcher registration", () => {
    const wrongPlan = new FakeHost(FEATURE_IDS, Object.freeze(new Array(32).fill(0)));
    expect(() => createNetworkV1HttpBindingAdapterForTesting(
      wrongPlan.table,
      EXPECTED,
    )).toThrow("Build Plan hash");
    expect(wrongPlan.registerCount).toBe(0);

    const wrongFeatures = new FakeHost(Object.freeze([
      NetworkV1FeatureId.HttpClient,
      NetworkV1FeatureId.HttpClientTls,
    ]));
    expect(() => createNetworkV1HttpBindingAdapterForTesting(
      wrongFeatures.table,
      EXPECTED,
    )).toThrow("feature set");
    expect(wrongFeatures.registerCount).toBe(0);
  });

  test("never invokes hostile table accessors", () => {
    const base = new FakeHost();
    let reads = 0;
    const hostile = Object.freeze(Object.defineProperties({}, {
      handshake: { value: base.table.handshake, enumerable: true },
      getLimits: { value: base.table.getLimits, enumerable: true },
      dispatch: {
        enumerable: true,
        get() {
          reads++;
          return base.table.dispatch;
        },
      },
      nextCompletion: { value: base.table.nextCompletion, enumerable: true },
      leaseTake: { value: base.table.leaseTake, enumerable: true },
      leaseReadInto: { value: base.table.leaseReadInto, enumerable: true },
      leaseRelease: { value: base.table.leaseRelease, enumerable: true },
      registerServiceDispatcher: {
        value: base.table.registerServiceDispatcher,
        enumerable: true,
      },
    }));
    expect(() => createNetworkV1HttpBindingAdapterForTesting(
      hostile as NetworkV1BindingTable,
      EXPECTED,
    )).toThrow("data property");
    expect(reads).toBe(0);
  });
});

describe("formal HTTP command/completion adapter", () => {
  test("publishes headers first and copies a response lease under BODY credit", async () => {
    const host = new FakeHost();
    const { binding } = createNetworkV1HttpBindingAdapterForTesting(host.table, EXPECTED);
    const operation = binding.start(highStart(1), null, new AbortController().signal);
    const responseBody = Object.freeze({ id: 101, generation: 3 });
    host.completions.push(host.headers(responseBody));
    expect(host.run()).toMatchObject({
      status: NetworkV1ServiceTurnStatus.Drained,
      eventsDelivered: 1,
      payloadBytesDelivered: 0,
      lastSequence: 1,
    });
    const response = await operation.response;
    expect(response.status).toBe(200);
    expect(response.body).toBeDefined();

    const destination = new Uint8Array(8);
    const read = response.body!.readInto(destination);
    expect(host.commands.at(-1)).toMatchObject({
      opcode: NetworkV1CommandOpcode.BodyPull,
      identity: { body: responseBody },
      maxBytes: 8,
    });
    host.completions.push(host.chunk(responseBody, [1, 2, 3], 2));
    expect(host.run()).toMatchObject({ eventsDelivered: 1, payloadBytesDelivered: 3 });
    expect(await read).toEqual({ bytes: 3, done: false });
    expect([...destination.subarray(0, 3)]).toEqual([1, 2, 3]);
    expect(host.releaseCount).toBe(1);

    const eof = response.body!.readInto(destination);
    host.completions.push(Object.freeze({
      eventCode: NetworkV1EventCode.BodyEnd,
      identity: completionIdentity(host.startCommand, responseBody, 3),
    }));
    host.run();
    expect(await eof).toEqual({ bytes: 0, done: true });
  });

  test("honors event/payload budgets without dequeuing a readiness probe", async () => {
    const host = new FakeHost();
    const { binding } = createNetworkV1HttpBindingAdapterForTesting(host.table, EXPECTED);
    const operation = binding.start(highStart(1), null, new AbortController().signal);
    const body = Object.freeze({ id: 9, generation: 1 });
    host.completions.push(host.headers(body));
    expect(host.run(1, 32)).toMatchObject({ status: NetworkV1ServiceTurnStatus.Drained });
    const response = await operation.response;
    const read = response.body!.readInto(new Uint8Array(4));
    host.completions.push(host.chunk(body, [1, 2, 3, 4], 2));
    expect(host.run(8, 3)).toEqual({
      status: NetworkV1ServiceTurnStatus.MoreReady,
      eventsDelivered: 0,
      payloadBytesDelivered: 0,
      lastSequence: 0,
    });
    expect(host.completions).toHaveLength(1);
    host.run(8, 4);
    expect(await read).toEqual({ bytes: 4, done: false });
  });

  test("cleans stale and out-of-order selected leases without delivery", async () => {
    const staleHost = new FakeHost();
    const staleAdapter = createNetworkV1HttpBindingAdapterForTesting(staleHost.table, EXPECTED);
    const staleOperation = staleAdapter.binding.start(
      highStart(1),
      null,
      new AbortController().signal,
    );
    const staleBody = Object.freeze({ id: 8, generation: 1 });
    staleHost.completions.push(staleHost.headers(staleBody));
    staleHost.run();
    const staleResponse = await staleOperation.response;
    await staleResponse.body!.cancel();
    staleHost.completions.push(staleHost.chunk(staleBody, [9], 2));
    expect(staleHost.run()).toMatchObject({ eventsDelivered: 1 });
    expect(staleHost.releaseCount).toBe(1);

    const orderedHost = new FakeHost();
    const orderedAdapter = createNetworkV1HttpBindingAdapterForTesting(orderedHost.table, EXPECTED);
    const orderedOperation = orderedAdapter.binding.start(
      highStart(2),
      null,
      new AbortController().signal,
    );
    const orderedBody = Object.freeze({ id: 12, generation: 1 });
    orderedHost.completions.push(orderedHost.headers(orderedBody, 2));
    orderedHost.run();
    const orderedResponse = await orderedOperation.response;
    const pending = orderedResponse.body!.readInto(new Uint8Array(2));
    pending.catch(() => {});
    orderedHost.completions.push(orderedHost.chunk(orderedBody, [4, 5], 1));
    expect(() => orderedHost.run()).toThrow("strictly monotonic");
    expect(orderedHost.releaseCount).toBe(1);
    await expect(pending).rejects.toBeInstanceOf(Error);
  });

  test("maps abort to one exact cancel command and a stable numeric error", async () => {
    const host = new FakeHost();
    const { binding } = createNetworkV1HttpBindingAdapterForTesting(host.table, EXPECTED);
    const operation = binding.start(highStart(22), null, new AbortController().signal);
    const cancel = Object.freeze({
      opcode: HighCommandOpcode.OperationCancel,
      operationId: 22,
    });
    operation.cancel(cancel);
    operation.cancel(cancel);
    expect(host.commands.filter(
      (command) => command.opcode === NetworkV1CommandOpcode.OperationCancel,
    )).toHaveLength(1);
    host.completions.push(Object.freeze({
      eventCode: NetworkV1EventCode.HttpRequestError,
      identity: completionIdentity(host.startCommand, ABSENT, 1),
      error: failure(NetworkV1ErrorCode.Aborted),
    }));
    host.run();
    await expect(operation.response).rejects.toMatchObject({
      category: "runtime",
      code: "aborted",
      operationId: 22,
    });
  });

  test("pulls a Guest request producer once per credit and stops it at headers", async () => {
    const host = new FakeHost();
    const { binding } = createNetworkV1HttpBindingAdapterForTesting(host.table, EXPECTED);
    let pulls = 0;
    let cancels = 0;
    const producer: HttpBodyProducer = Object.freeze({
      async pull(maxBytes: number) {
        pulls++;
        expect(maxBytes).toBe(3);
        return Uint8Array.from([7, 8, 9]);
      },
      async cancel() { cancels++; },
    });
    const operation = binding.start(highStart(5, true), producer, new AbortController().signal);
    const start = host.startCommand;
    expect(start.identity.body.id).not.toBe(0);
    host.completions.push(Object.freeze({
      eventCode: NetworkV1EventCode.BodyPull,
      identity: completionIdentity(start, start.identity.body, 1),
      maxBytes: 3,
    }));
    host.run();
    await Promise.resolve();
    await Promise.resolve();
    const chunk = host.commands.find(
      (command) => command.opcode === NetworkV1CommandOpcode.BodyChunk,
    );
    expect(chunk).toMatchObject({ input: { kind: 2 } });
    if (chunk?.opcode === NetworkV1CommandOpcode.BodyChunk) {
      expect([...chunk.input.bytes]).toEqual([7, 8, 9]);
    }
    expect(pulls).toBe(1);
    host.completions.push(host.headers(ABSENT, 2));
    host.run();
    await operation.response;
    await Promise.resolve();
    expect(cancels).toBe(1);
  });

  test("bounds operation slots, advances generation, and never wraps", async () => {
    const host = new FakeHost();
    const { binding } = createNetworkV1HttpBindingAdapterForTesting(
      host.table,
      EXPECTED,
      { maxOperations: 1 },
    );
    const first = binding.start(highStart(1), null, new AbortController().signal);
    const firstGeneration = host.startCommand.identity.operation.generation;
    expect(() => binding.start(
      highStart(2),
      null,
      new AbortController().signal,
    )).toThrow("capacity");
    first.response.catch(() => {});
    host.completions.push(Object.freeze({
      eventCode: NetworkV1EventCode.HttpRequestError,
      identity: completionIdentity(host.startCommand, ABSENT, 1),
      error: failure(),
    }));
    host.run();
    await expect(first.response).rejects.toBeDefined();
    binding.start(highStart(3), null, new AbortController().signal);
    const starts = host.commands.filter((command) =>
      command.opcode === NetworkV1CommandOpcode.HttpRequestStart
    );
    expect(starts[1]!.identity.operation.generation).toBe(firstGeneration + 1);

    const exhaustedHost = new FakeHost();
    const exhausted = createNetworkV1HttpBindingAdapterForTesting(
      exhaustedHost.table,
      EXPECTED,
      { maxOperations: 1, initialSlotGeneration: NETWORK_V1_UINT32_MAX },
    );
    expect(() => exhausted.binding.start(
      highStart(4),
      null,
      new AbortController().signal,
    )).toThrow("capacity");
  });
});

describe("formal limits projection", () => {
  test("validates the Host snapshot and keeps unscoped capabilities empty", () => {
    const host = new FakeHost();
    const adapter = createNetworkV1HttpBindingAdapterForTesting(host.table, EXPECTED);
    expect(adapter.limits("http", "client")).toEqual({
      values: [{
        name: "http.maxBodyChunkBytes",
        default: 2048,
        hard: 4096,
        minimum: 512,
      }],
      features: ["network.http.client"],
    });
    expect(adapter.limits("http", "server").features).toEqual([]);
  });

  test("returns a recursively frozen null-prototype public snapshot", async () => {
    const directory = await mkdtemp(join(tmpdir(), "pocketjs-network-limits-"));
    const resultPath = join(directory, "result.json");
    const limitsUrl = new URL(
      "../framework/src/net/network-limits.ts",
      import.meta.url,
    ).href;
    const netUrl = new URL("../framework/src/net/index.ts", import.meta.url).href;
    const source = `
      const limits = await import(${JSON.stringify(limitsUrl)});
      const net = await import(${JSON.stringify(netUrl)});
      limits.installNetworkLimitsProvider((protocol, role) => ({
        values: protocol === "http" && role === "client"
          ? [{ name: "http.maxBodyChunkBytes", default: 2, hard: 4, minimum: 1 }]
          : [],
        features: protocol === "http" && role === "client"
          ? ["network.http.client"]
          : [],
      }));
      const snapshot = net.getNetworkLimits("http", "client");
      let unadmitted = "";
      try { net.getNetworkLimits("http", "server"); } catch (error) {
        unadmitted = error.message;
      }
      let invalid = "";
      try { net.getNetworkLimits("invalid"); } catch (error) {
        invalid = error.constructor.name;
      }
      await Bun.write(${JSON.stringify(resultPath)}, JSON.stringify({
        valuesNull: Object.getPrototypeOf(snapshot.values) === null,
        featuresNull: Object.getPrototypeOf(snapshot.features) === null,
        entryNull: Object.getPrototypeOf(snapshot.values["http.maxBodyChunkBytes"]) === null,
        frozen: Object.isFrozen(snapshot) && Object.isFrozen(snapshot.values) &&
          Object.isFrozen(snapshot.features) &&
          Object.isFrozen(snapshot.values["http.maxBodyChunkBytes"]),
        feature: snapshot.features["network.http.client"],
        unadmitted,
        invalid,
      }));
    `;
    try {
      const script = join(directory, "limits.ts");
      await Bun.write(script, source);
      const child = Bun.spawn([process.execPath, script], {
        stdout: "ignore",
        stderr: "pipe",
      });
      const [exitCode, stderr] = await Promise.all([
        child.exited,
        new Response(child.stderr).text(),
      ]);
      expect(exitCode, stderr).toBe(0);
      expect(await Bun.file(resultPath).json()).toEqual({
        valuesNull: true,
        featuresNull: true,
        entryNull: true,
        frozen: true,
        feature: true,
        unadmitted: "Network limits are unavailable for an unadmitted scope",
        invalid: "TypeError",
      });
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });
});
