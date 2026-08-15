import type {
  AbortSignal,
  NetworkLimitOverrides,
  TlsOptions,
} from "./index.ts";
import type { BodyStream, HttpBodyProducer } from "./http-body.ts";
import {
  NETWORK_V1_ABI_MAJOR,
  NETWORK_V1_ABI_MINOR,
  NetworkV1CommandOpcode as FormalNetworkV1CommandOpcode,
  NetworkV1EventCode as FormalNetworkV1EventCode,
} from "../../../contracts/spec/network/network-v1.ts";

/**
 * High-level seam consumed by the Fetch implementation. Production installs
 * an adapter over the formal `pocketjs:internal/network-v1` table before any
 * application initializer. Tests may still install this seam directly without
 * manufacturing native completion and BufferLease traffic.
 */
export { NETWORK_V1_ABI_MAJOR, NETWORK_V1_ABI_MINOR };

/** Formal append-only command ids, retained under the SDK seam name. */
export enum NetworkV1CommandOpcode {
  OperationCancel = 0x0001,
  BodyPull = 0x0010,
  BodyChunk = 0x0011,
  BodyEnd = 0x0012,
  BodyError = 0x0013,
  BodyCancel = 0x0014,
  HttpRequestStart = 0x0100,
}

/** Formal append-only completion ids, retained under the SDK seam name. */
export enum NetworkV1EventCode {
  BodyPull = 0x0010,
  BodyChunk = 0x0011,
  BodyEnd = 0x0012,
  BodyError = 0x0013,
  BodyCancel = 0x0014,
  HttpResponseHeaders = 0x0100,
  HttpRequestError = 0x0101,
}

// Compile-time/runtime drift guard for the compatibility-shaped high-level seam.
if (
  NetworkV1CommandOpcode.OperationCancel !== FormalNetworkV1CommandOpcode.OperationCancel ||
  NetworkV1CommandOpcode.BodyPull !== FormalNetworkV1CommandOpcode.BodyPull ||
  NetworkV1CommandOpcode.BodyChunk !== FormalNetworkV1CommandOpcode.BodyChunk ||
  NetworkV1CommandOpcode.BodyEnd !== FormalNetworkV1CommandOpcode.BodyEnd ||
  NetworkV1CommandOpcode.BodyError !== FormalNetworkV1CommandOpcode.BodyError ||
  NetworkV1CommandOpcode.BodyCancel !== FormalNetworkV1CommandOpcode.BodyCancel ||
  NetworkV1CommandOpcode.HttpRequestStart !== FormalNetworkV1CommandOpcode.HttpRequestStart ||
  NetworkV1EventCode.BodyPull !== FormalNetworkV1EventCode.BodyPull ||
  NetworkV1EventCode.BodyChunk !== FormalNetworkV1EventCode.BodyChunk ||
  NetworkV1EventCode.BodyEnd !== FormalNetworkV1EventCode.BodyEnd ||
  NetworkV1EventCode.BodyError !== FormalNetworkV1EventCode.BodyError ||
  NetworkV1EventCode.BodyCancel !== FormalNetworkV1EventCode.BodyCancel ||
  NetworkV1EventCode.HttpResponseHeaders !== FormalNetworkV1EventCode.HttpResponseHeaders ||
  NetworkV1EventCode.HttpRequestError !== FormalNetworkV1EventCode.HttpRequestError
) {
  throw new TypeError("PocketJS HTTP high-level seam drifted from network ABI v1");
}

export interface HttpBindingHeader {
  readonly name: string;
  readonly value: string;
}

export interface HttpRequestStartCommand {
  readonly opcode: NetworkV1CommandOpcode.HttpRequestStart;
  readonly operationId: number;
  readonly url: string;
  readonly method: string;
  readonly headers: readonly HttpBindingHeader[];
  readonly hasBody: boolean;
  readonly redirect: "follow" | "manual" | "error";
  readonly timeouts?: Readonly<{
    connect?: number;
    headers?: number;
    idle?: number;
    total?: number;
  }>;
  readonly maxRedirects: number;
  readonly tls?: TlsOptions;
  readonly limits?: NetworkLimitOverrides;
  readonly ref: boolean;
}

export interface OperationCancelCommand {
  readonly opcode: NetworkV1CommandOpcode.OperationCancel;
  readonly operationId: number;
}

export interface BodyPullCommand {
  readonly opcode: NetworkV1CommandOpcode.BodyPull;
  readonly operationId: number;
  readonly bodyId: number;
  readonly maxBytes: number;
}

export interface BodyChunkCommand {
  readonly opcode: NetworkV1CommandOpcode.BodyChunk;
  readonly operationId: number;
  readonly bodyId: number;
  readonly bytes: Uint8Array;
}

export interface BodyEndCommand {
  readonly opcode: NetworkV1CommandOpcode.BodyEnd;
  readonly operationId: number;
  readonly bodyId: number;
}

export interface BodyErrorCommand {
  readonly opcode: NetworkV1CommandOpcode.BodyError;
  readonly operationId: number;
  readonly bodyId: number;
  readonly code: string;
}

export interface BodyCancelCommand {
  readonly opcode: NetworkV1CommandOpcode.BodyCancel;
  readonly operationId: number;
  readonly bodyId: number;
}

export type NetworkV1HttpCommand =
  | HttpRequestStartCommand
  | OperationCancelCommand
  | BodyPullCommand
  | BodyChunkCommand
  | BodyEndCommand
  | BodyErrorCommand
  | BodyCancelCommand;

export interface HttpResponseHeadersEvent {
  readonly eventCode: NetworkV1EventCode.HttpResponseHeaders;
  readonly operationId: number;
  readonly status: number;
  readonly statusText: string;
  readonly headers: readonly HttpBindingHeader[];
  readonly url: string;
  readonly redirected: boolean;
  /** Always wrapped as the PocketJS BodyStream identity before publication. */
  readonly body?: BodyStream | null;
  /** Host-admitted aggregation ceiling; the SDK ceiling can only lower it. */
  readonly bufferedBodyBytes?: number;
}

export interface HttpRequestErrorEvent {
  readonly eventCode: NetworkV1EventCode.HttpRequestError;
  readonly operationId: number;
  readonly category: "runtime" | "resolver" | "transport" | "tls" | "protocol";
  readonly code: string;
  readonly message: string;
  readonly temporary?: boolean;
  readonly causeCode?: string;
  readonly reasonCode?: number;
}

export interface BodyPullEvent {
  readonly eventCode: NetworkV1EventCode.BodyPull;
  readonly operationId: number;
  readonly bodyId: number;
  readonly maxBytes: number;
}

export interface BodyChunkEvent {
  readonly eventCode: NetworkV1EventCode.BodyChunk;
  readonly operationId: number;
  readonly bodyId: number;
  readonly leaseId: number;
  readonly byteLength: number;
}

export interface BodyEndEvent {
  readonly eventCode: NetworkV1EventCode.BodyEnd;
  readonly operationId: number;
  readonly bodyId: number;
}

export interface BodyErrorEvent {
  readonly eventCode: NetworkV1EventCode.BodyError;
  readonly operationId: number;
  readonly bodyId: number;
  readonly code: string;
}

export interface BodyCancelEvent {
  readonly eventCode: NetworkV1EventCode.BodyCancel;
  readonly operationId: number;
  readonly bodyId: number;
}

export type NetworkV1HttpEvent =
  | HttpResponseHeadersEvent
  | HttpRequestErrorEvent
  | BodyPullEvent
  | BodyChunkEvent
  | BodyEndEvent
  | BodyErrorEvent
  | BodyCancelEvent;

export interface HttpClientBindingOperation {
  /**
   * Resolving response headers is the terminal claim for the request-upload
   * direction. Before resolution, the binding must stop BODY_PULL credit and
   * cancel a producer that has not reached EOF. The response body has its own
   * lifetime and remains live until EOF, cancellation, or operation abort.
   */
  readonly response: Promise<HttpResponseHeadersEvent>;
  cancel(command: OperationCancelCommand): void;
}

export interface HttpClientPrivateBinding {
  readonly abiMajor: number;
  readonly abiMinor: number;
  readonly featureSet: readonly string[];
  /** Effective ALPN intersection for this admitted HTTP Client role. */
  readonly alpnProtocols?: readonly string[];
  start(
    command: HttpRequestStartCommand,
    requestBody: HttpBodyProducer | null,
    signal: AbortSignal,
  ): HttpClientBindingOperation;
}

function validateBinding(value: unknown): HttpClientPrivateBinding {
  if (typeof value !== "object" || value === null) {
    throw new TypeError("PocketJS HTTP client binding table is missing");
  }
  const binding = value as HttpClientPrivateBinding;
  if (!Object.isFrozen(binding)) {
    throw new TypeError("PocketJS HTTP client binding table must be frozen");
  }
  const abiMajor = binding.abiMajor;
  const abiMinor = binding.abiMinor;
  const featureSetValue = binding.featureSet;
  const alpnProtocolsValue = binding.alpnProtocols;
  const start = binding.start;

  const snapshotTokens = (
    input: unknown,
    label: string,
    maximumCount: number,
    maximumTokenBytes: number,
  ): readonly string[] => {
    if (!Array.isArray(input) || !Object.isFrozen(input)) {
      throw new TypeError(`PocketJS HTTP client binding ${label} must be a frozen array`);
    }
    const count = input.length;
    if (!Number.isSafeInteger(count) || count > maximumCount) {
      throw new TypeError(`PocketJS HTTP client binding ${label} exceeds its ceiling`);
    }
    const output: string[] = [];
    const seen = new Set<string>();
    for (let index = 0; index < count; index++) {
      const token = input[index];
      if (
        typeof token !== "string" ||
        token.length === 0 ||
        token.length > maximumTokenBytes ||
        seen.has(token)
      ) {
        throw new TypeError(`PocketJS HTTP client binding ${label} is invalid`);
      }
      seen.add(token);
      output.push(token);
    }
    return Object.freeze(output);
  };
  const featureSet = snapshotTokens(featureSetValue, "feature set", 64, 128);
  const alpnProtocols = alpnProtocolsValue === undefined
    ? undefined
    : snapshotTokens(alpnProtocolsValue, "ALPN set", 16, 255);
  if (
    featureSet.some((feature) => !/^[a-z0-9][a-z0-9._-]*$/.test(feature))
  ) {
    throw new TypeError("PocketJS HTTP client binding feature set is invalid");
  }
  if (
    abiMajor !== NETWORK_V1_ABI_MAJOR ||
    !Number.isSafeInteger(abiMinor) ||
    abiMinor < NETWORK_V1_ABI_MINOR
  ) {
    throw new TypeError(
      `PocketJS network ABI mismatch: expected ${NETWORK_V1_ABI_MAJOR}.${NETWORK_V1_ABI_MINOR}`,
    );
  }
  if (typeof start !== "function") {
    throw new TypeError("PocketJS HTTP client binding has no start function");
  }
  return Object.freeze({
    abiMajor,
    abiMinor,
    featureSet,
    ...(alpnProtocols === undefined ? {} : { alpnProtocols }),
    start: (
      command: HttpRequestStartCommand,
      requestBody: HttpBodyProducer | null,
      signal: AbortSignal,
    ) => start.call(binding, command, requestBody, signal),
  });
}

let capturedHttpClientBinding: HttpClientPrivateBinding | undefined;

/** Compiler-only runtime mount target; the raw formal table never enters here. */
export function installHttpClientBindingForRuntime(
  binding: HttpClientPrivateBinding,
): void {
  if (capturedHttpClientBinding) {
    throw new TypeError("PocketJS HTTP client binding is already installed");
  }
  capturedHttpClientBinding = validateBinding(binding);
}

/**
 * Compiler/Host seam. This module is not a package export: public application
 * code receives only the values re-exported by `net/http`.
 *
 * The runtime installation is one-shot. The returned cleanup exists solely so
 * isolated conformance tests can restore the unbound staged profile.
 */
export function installHttpClientBindingForTesting(
  binding: HttpClientPrivateBinding,
): () => void {
  if (capturedHttpClientBinding) {
    throw new TypeError("PocketJS HTTP client binding is already installed");
  }
  const captured = validateBinding(binding);
  capturedHttpClientBinding = captured;
  return () => {
    if (capturedHttpClientBinding !== captured) {
      throw new TypeError("PocketJS HTTP client binding identity changed");
    }
    capturedHttpClientBinding = undefined;
  };
}

export function getHttpClientBinding(): HttpClientPrivateBinding | undefined {
  return capturedHttpClientBinding;
}
