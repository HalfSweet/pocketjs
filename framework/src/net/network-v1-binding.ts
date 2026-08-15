/**
 * Owner-thread adapter from the formal private network ABI to the Fetch SDK's
 * small high-level binding seam. This module is compiler-owned and is not a
 * package export.
 */

import {
  NETWORK_V1_ABI_MAJOR,
  NETWORK_V1_ABI_MINOR,
  NETWORK_V1_ERROR_CATEGORY_NAME_BY_ID,
  NETWORK_V1_ERROR_NAME_BY_ID,
  NETWORK_V1_FEATURE_CAPABILITY_BY_ID,
  NETWORK_V1_LIMIT_PROTOCOL_ANY,
  NETWORK_V1_LIMIT_ROLE_ANY,
  NETWORK_V1_SEQUENCE_MAX,
  NETWORK_V1_UINT32_MAX,
  NetworkV1BorrowedInputKind,
  NetworkV1ClientCertificateMode,
  NetworkV1CommandOpcode,
  NetworkV1CompletionPollStatus,
  NetworkV1DispatchStatus,
  NetworkV1ErrorCategory,
  NetworkV1ErrorCode,
  NetworkV1EventCode,
  NetworkV1FeatureId,
  NetworkV1HttpRedirectMode,
  NetworkV1LimitProtocol,
  NetworkV1LimitRole,
  NetworkV1ServiceTurnKind,
  NetworkV1ServiceTurnStatus,
  NetworkV1TlsRevocation,
  NetworkV1TlsVerification,
  NetworkV1TlsVersion,
  assertNetworkV1BindingTable,
  assertNetworkV1CompletionPollResult,
  assertNetworkV1NextSequence,
  assertNetworkV1RuntimeGeneration,
  assertNetworkV1ServiceTurnRequest,
  assertNetworkV1ServiceTurnResult,
  networkV1HandleIsAbsent,
  networkV1SameHandle,
  snapshotNetworkV1Limits,
  type NetworkV1AsyncCommand,
  type NetworkV1BindingTable,
  type NetworkV1BufferLeaseReadIntoCommand,
  type NetworkV1BufferLeaseReleaseCommand,
  type NetworkV1BufferLeaseTakeCommand,
  type NetworkV1CommandIdentity,
  type NetworkV1Completion,
  type NetworkV1CompletionPollResult,
  type NetworkV1CompletionIdentity,
  type NetworkV1ErrorMetadata,
  type NetworkV1Handle,
  type NetworkV1Handshake,
  type NetworkV1HttpRequestMetadata,
  type NetworkV1HttpResponseMetadata,
  type NetworkV1ServiceDispatcher,
  type NetworkV1ServiceTurnRequest,
  type NetworkV1ServiceTurnResult,
} from "../../../contracts/spec/network/network-v1.ts";
import { NetworkError } from "./index.ts";
import { HTTP_BODY_CHUNK_BYTES, snapshotUint8Array } from "./http-body.ts";
import { canonicalizeHttpUrl } from "./http-url.ts";
import {
  installHttpClientBindingForRuntime,
  type BodyCancelCommand,
  type HttpBindingHeader,
  type HttpClientBindingOperation,
  type HttpClientPrivateBinding,
  type HttpRequestErrorEvent,
  type HttpRequestStartCommand,
  type HttpResponseHeadersEvent,
  type OperationCancelCommand,
} from "./http-binding.ts";
import type { BodyStream, HttpBodyProducer } from "./http-body.ts";
import { installNetworkLimitsProvider } from "./network-limits.ts";

const MAX_OPERATIONS = 8;
const MAX_HEADERS = 128;
const MAX_HEADER_NAME_BYTES = 256;
const MAX_HEADER_VALUE_BYTES = 16 * 1024;
const MAX_HEADER_BLOCK_BYTES = 64 * 1024;
const MAX_LIMIT_OVERRIDES = 32;
const MAX_LIMIT_NAME_BYTES = 64;
const MAX_ALPN_TOKENS = 16;
const MAX_ALPN_BYTES = 1024;
const MAX_CA_BYTES = 64 * 1024;
const MAX_URL_BYTES = 8192;
const MAX_OPERATION_LABEL_BYTES = 64;
const MAX_CAUSE_CODE_BYTES = 64;

const objectFreeze = Object.freeze;
const objectKeys = Object.keys;
const objectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
const objectIsFrozen = Object.isFrozen;
const arrayIsArray = Array.isArray;
const numberIsSafeInteger = Number.isSafeInteger;
const numberIsInteger = Number.isInteger;
const reflectApply = Reflect.apply;
const PromiseIntrinsic = Promise;
const promiseResolve = Promise.resolve;
const promiseThen = Promise.prototype.then;
const uint8ArraySlice = Uint8Array.prototype.slice;
const typedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype) as object;
const typedArrayByteLength = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  "byteLength",
)!.get!;

const ABSENT_HANDLE: Readonly<NetworkV1Handle> = objectFreeze({ id: 0, generation: 0 });
const HTTP_RESOURCE: Readonly<NetworkV1Handle> = objectFreeze({ id: 1, generation: 1 });

export interface NetworkV1CompiledExpectation {
  readonly planHashBytes: readonly number[];
  readonly featureIds: readonly number[];
}

interface PendingBodyRead {
  readonly destination: Uint8Array;
  readonly capacity: number;
  readonly resolve: (result: { bytes: number; done: boolean }) => void;
  readonly reject: (error: unknown) => void;
}

interface ResponseBodyState {
  phase: "open" | "ended" | "errored";
  error?: unknown;
}

type SlotPhase = "headers" | "response" | "retired";

interface OperationSlot {
  readonly slotId: number;
  generation: number;
  phase: SlotPhase;
  highOperationId: number;
  operation: Readonly<NetworkV1Handle>;
  requestBody: Readonly<NetworkV1Handle>;
  responseBody: Readonly<NetworkV1Handle>;
  producer: HttpBodyProducer | null;
  uploadCredit: number;
  uploadPullPending: boolean;
  uploadTerminal: boolean;
  pendingRead?: PendingBodyRead;
  responseBodyState?: ResponseBodyState;
  responseResolve?: (event: HttpResponseHeadersEvent) => void;
  responseReject?: (error: unknown) => void;
  cancelSent: boolean;
}

interface SafeBindingMethods {
  readonly getLimits: NetworkV1BindingTable["getLimits"];
  readonly dispatch: NetworkV1BindingTable["dispatch"];
  readonly nextCompletion: NetworkV1BindingTable["nextCompletion"];
  readonly leaseTake: NetworkV1BindingTable["leaseTake"];
  readonly leaseReadInto: NetworkV1BindingTable["leaseReadInto"];
  readonly leaseRelease: NetworkV1BindingTable["leaseRelease"];
  readonly registerServiceDispatcher: NetworkV1BindingTable["registerServiceDispatcher"];
}

interface AdapterTestOptions {
  readonly maxOperations?: number;
  readonly initialSlotGeneration?: number;
}

function abiFault(detail: string): TypeError {
  return new TypeError(`PocketJS network ABI adapter: ${detail}`);
}

function integer(
  value: unknown,
  minimum: number,
  maximum: number,
  label: string,
): number {
  if (!numberIsSafeInteger(value) || (value as number) < minimum ||
    (value as number) > maximum) {
    throw abiFault(`${label} is outside [${minimum}, ${maximum}]`);
  }
  return value as number;
}

function byteString(value: unknown, maximum: number, label: string): string {
  if (typeof value !== "string" || value.length > maximum) {
    throw abiFault(`${label} is not a bounded string`);
  }
  for (let index = 0; index < value.length; index += 1) {
    if (value.charCodeAt(index) > 0xff) throw abiFault(`${label} is not ByteString`);
  }
  return value;
}

function snapshotHandle(value: unknown, label: string): Readonly<NetworkV1Handle> {
  if (typeof value !== "object" || value === null) throw abiFault(`${label} is missing`);
  const candidate = value as Partial<NetworkV1Handle>;
  const id = integer(candidate.id, 0, NETWORK_V1_UINT32_MAX, `${label}.id`);
  const generation = integer(
    candidate.generation,
    0,
    NETWORK_V1_UINT32_MAX,
    `${label}.generation`,
  );
  if ((id === 0) !== (generation === 0)) throw abiFault(`${label} is a partial handle`);
  return objectFreeze({ id, generation });
}

function snapshotIdentity(
  value: unknown,
  label: string,
): Readonly<NetworkV1CompletionIdentity> {
  if (typeof value !== "object" || value === null) throw abiFault(`${label} is missing`);
  const candidate = value as Partial<NetworkV1CompletionIdentity>;
  const runtimeGeneration = integer(
    candidate.runtimeGeneration,
    1,
    NETWORK_V1_UINT32_MAX,
    `${label}.runtimeGeneration`,
  );
  const resource = snapshotHandle(candidate.resource, `${label}.resource`);
  const operation = snapshotHandle(candidate.operation, `${label}.operation`);
  const body = snapshotHandle(candidate.body, `${label}.body`);
  const sequence = integer(candidate.sequence, 1, NETWORK_V1_SEQUENCE_MAX, `${label}.sequence`);
  return objectFreeze({ runtimeGeneration, resource, operation, body, sequence });
}

function snapshotHeaders(value: unknown, label: string): readonly HttpBindingHeader[] {
  if (!arrayIsArray(value)) throw abiFault(`${label} is not an array`);
  const length = integer(value.length, 0, MAX_HEADERS, `${label}.length`);
  const headers: HttpBindingHeader[] = [];
  let blockBytes = 0;
  for (let index = 0; index < length; index += 1) {
    const entry = value[index] as unknown;
    if (typeof entry !== "object" || entry === null) throw abiFault(`${label}[${index}] is invalid`);
    const candidate = entry as Partial<HttpBindingHeader>;
    const name = byteString(candidate.name, MAX_HEADER_NAME_BYTES, `${label}[${index}].name`);
    const headerValue = byteString(
      candidate.value,
      MAX_HEADER_VALUE_BYTES,
      `${label}[${index}].value`,
    );
    blockBytes += name.length + headerValue.length;
    if (blockBytes > MAX_HEADER_BLOCK_BYTES) throw abiFault(`${label} exceeds its block ceiling`);
    headers.push(objectFreeze({ name, value: headerValue }));
  }
  return objectFreeze(headers);
}

function snapshotError(value: unknown, label: string): Readonly<NetworkV1ErrorMetadata> {
  if (typeof value !== "object" || value === null) throw abiFault(`${label} is missing`);
  const candidate = value as Partial<NetworkV1ErrorMetadata>;
  const category = integer(candidate.category, 1, 5, `${label}.category`) as NetworkV1ErrorMetadata["category"];
  const code = integer(candidate.code, 1, 0x7fff, `${label}.code`) as NetworkV1ErrorMetadata["code"];
  if ((NETWORK_V1_ERROR_CATEGORY_NAME_BY_ID as Readonly<Record<number, string>>)[category] === undefined ||
    (NETWORK_V1_ERROR_NAME_BY_ID as Readonly<Record<number, string>>)[code] === undefined) {
    throw abiFault(`${label} uses an unknown category or code`);
  }
  const operation = byteString(
    candidate.operation,
    MAX_OPERATION_LABEL_BYTES,
    `${label}.operation`,
  );
  if (operation.length === 0 || typeof candidate.temporary !== "boolean") {
    throw abiFault(`${label} has invalid stable metadata`);
  }
  const address = candidate.address === undefined
    ? undefined
    : byteString(candidate.address, 253, `${label}.address`);
  const port = candidate.port === undefined
    ? undefined
    : integer(candidate.port, 1, 65_535, `${label}.port`);
  const causeCode = candidate.causeCode === undefined
    ? undefined
    : byteString(candidate.causeCode, MAX_CAUSE_CODE_BYTES, `${label}.causeCode`);
  const reasonCode = candidate.reasonCode === undefined
    ? undefined
    : integer(candidate.reasonCode, 0, NETWORK_V1_UINT32_MAX, `${label}.reasonCode`);
  return objectFreeze({
    category,
    code,
    operation,
    temporary: candidate.temporary,
    ...(address === undefined ? {} : { address }),
    ...(port === undefined ? {} : { port }),
    ...(causeCode === undefined ? {} : { causeCode }),
    ...(reasonCode === undefined ? {} : { reasonCode }),
  });
}

function snapshotResponseMetadata(value: unknown): Readonly<NetworkV1HttpResponseMetadata> {
  if (typeof value !== "object" || value === null) throw abiFault("response metadata is missing");
  const candidate = value as Partial<NetworkV1HttpResponseMetadata>;
  const status = integer(candidate.status, 200, 599, "response.status");
  const statusText = byteString(candidate.statusText, 1024, "response.statusText");
  const headers = snapshotHeaders(candidate.headers, "response.headers");
  const url = byteString(candidate.url, MAX_URL_BYTES, "response.url");
  if (typeof candidate.redirected !== "boolean") throw abiFault("response.redirected is invalid");
  const bufferedBodyBytes = integer(
    candidate.bufferedBodyBytes,
    1,
    NETWORK_V1_UINT32_MAX,
    "response.bufferedBodyBytes",
  );
  return objectFreeze({
    status,
    statusText,
    headers,
    url,
    redirected: candidate.redirected,
    bufferedBodyBytes,
  });
}

function snapshotCompletion(value: unknown): NetworkV1Completion {
  if (typeof value !== "object" || value === null) throw abiFault("completion is missing");
  const candidate = value as Partial<NetworkV1Completion>;
  const eventCode = candidate.eventCode;
  const identity = snapshotIdentity(candidate.identity, "completion.identity");
  switch (eventCode) {
    case NetworkV1EventCode.BodyPull:
      return objectFreeze({
        eventCode,
        identity,
        maxBytes: integer(
          (candidate as Partial<Extract<NetworkV1Completion, { eventCode: typeof eventCode }>>).maxBytes,
          1,
          HTTP_BODY_CHUNK_BYTES,
          "BODY_PULL.maxBytes",
        ),
      });
    case NetworkV1EventCode.BodyChunk: {
      const rawPayload = (candidate as Partial<Extract<NetworkV1Completion, { eventCode: typeof eventCode }>>).payload;
      if (typeof rawPayload !== "object" || rawPayload === null) {
        throw abiFault("BODY_CHUNK payload is missing");
      }
      const payloadCandidate = rawPayload as {
        readonly runtimeGeneration?: unknown;
        readonly lease?: unknown;
        readonly byteLength?: unknown;
      };
      const payload = objectFreeze({
        runtimeGeneration: integer(
          payloadCandidate.runtimeGeneration,
          1,
          NETWORK_V1_UINT32_MAX,
          "BODY_CHUNK.runtimeGeneration",
        ),
        lease: snapshotHandle(payloadCandidate.lease, "BODY_CHUNK.lease"),
        byteLength: integer(
          payloadCandidate.byteLength,
          1,
          NETWORK_V1_UINT32_MAX,
          "BODY_CHUNK.byteLength",
        ),
      });
      return objectFreeze({ eventCode, identity, payload });
    }
    case NetworkV1EventCode.BodyEnd:
    case NetworkV1EventCode.BodyCancel:
      return objectFreeze({ eventCode, identity }) as NetworkV1Completion;
    case NetworkV1EventCode.BodyError:
      return objectFreeze({
        eventCode,
        identity,
        error: snapshotError(
          (candidate as Partial<Extract<NetworkV1Completion, { eventCode: typeof eventCode }>>).error,
          "BODY_ERROR.error",
        ),
      });
    case NetworkV1EventCode.HttpResponseHeaders:
      return objectFreeze({
        eventCode,
        identity,
        metadata: snapshotResponseMetadata(
          (candidate as Partial<Extract<NetworkV1Completion, { eventCode: typeof eventCode }>>).metadata,
        ),
      });
    case NetworkV1EventCode.HttpRequestError:
      return objectFreeze({
        eventCode,
        identity,
        error: snapshotError(
          (candidate as Partial<Extract<NetworkV1Completion, { eventCode: typeof eventCode }>>).error,
          "HTTP_REQUEST_ERROR.error",
        ),
      });
    default:
      throw abiFault("completion uses an unknown event code");
  }
}

function actualUint8ArrayLength(value: unknown, label: string): number {
  try {
    return typedArrayByteLength.call(value) as number;
  } catch {
    throw abiFault(`${label} must be a Uint8Array`);
  }
}

function snapshotExpectedHandshake(
  value: NetworkV1CompiledExpectation,
  runtimeGeneration: number,
): Readonly<NetworkV1Handshake> {
  if (typeof value !== "object" || value === null) throw abiFault("compiled expectation is missing");
  if (!arrayIsArray(value.planHashBytes) || value.planHashBytes.length !== 32) {
    throw abiFault("compiled plan hash must contain 32 bytes");
  }
  const planHash = new Uint8Array(32);
  for (let index = 0; index < 32; index += 1) {
    planHash[index] = integer(value.planHashBytes[index], 0, 0xff, `planHash[${index}]`);
  }
  if (!arrayIsArray(value.featureIds)) throw abiFault("compiled feature ids must be an array");
  const featureIds: NetworkV1FeatureId[] = [];
  let previous = 0;
  for (let index = 0; index < value.featureIds.length; index += 1) {
    const id = integer(value.featureIds[index], 1, 0xffff, `featureIds[${index}]`);
    if (id <= previous ||
      (NETWORK_V1_FEATURE_CAPABILITY_BY_ID as Readonly<Record<number, string>>)[id] === undefined) {
      throw abiFault("compiled feature ids are unknown, duplicate, or unsorted");
    }
    featureIds.push(id as NetworkV1FeatureId);
    previous = id;
  }
  return objectFreeze({
    abiMajor: NETWORK_V1_ABI_MAJOR,
    abiMinor: NETWORK_V1_ABI_MINOR,
    runtimeGeneration,
    planHash,
    featureIds: objectFreeze(featureIds),
  });
}

function adoptRuntimeGeneration(value: unknown): number {
  if (typeof value !== "object" || value === null || !objectIsFrozen(value)) {
    throw abiFault("binding table must be frozen");
  }
  const handshakeDescriptor = objectGetOwnPropertyDescriptor(value, "handshake");
  if (!handshakeDescriptor || !("value" in handshakeDescriptor)) {
    throw abiFault("binding handshake must be an own data property");
  }
  const handshake = handshakeDescriptor.value;
  if (typeof handshake !== "object" || handshake === null || !objectIsFrozen(handshake)) {
    throw abiFault("binding handshake must be frozen");
  }
  const runtimeDescriptor = objectGetOwnPropertyDescriptor(handshake, "runtimeGeneration");
  if (!runtimeDescriptor || !("value" in runtimeDescriptor)) {
    throw abiFault("runtimeGeneration must be an own data property");
  }
  assertNetworkV1RuntimeGeneration(runtimeDescriptor.value);
  return runtimeDescriptor.value as number;
}

function captureMethods(table: NetworkV1BindingTable): Readonly<SafeBindingMethods> {
  const method = <Key extends keyof SafeBindingMethods>(key: Key): SafeBindingMethods[Key] => {
    const descriptor = objectGetOwnPropertyDescriptor(table, key);
    if (!descriptor || !("value" in descriptor) || typeof descriptor.value !== "function") {
      throw abiFault(`bindingTable.${key} changed after handshake`);
    }
    return descriptor.value as SafeBindingMethods[Key];
  };
  return objectFreeze({
    getLimits: method("getLimits"),
    dispatch: method("dispatch"),
    nextCompletion: method("nextCompletion"),
    leaseTake: method("leaseTake"),
    leaseReadInto: method("leaseReadInto"),
    leaseRelease: method("leaseRelease"),
    registerServiceDispatcher: method("registerServiceDispatcher"),
  });
}

function httpErrorCompatibility(category: number, code: number): boolean {
  if (category === NetworkV1ErrorCategory.Runtime) {
    return code >= NetworkV1ErrorCode.Aborted && code <= NetworkV1ErrorCode.PermissionDenied ||
      code === NetworkV1ErrorCode.SystemError;
  }
  if (category === NetworkV1ErrorCategory.Resolver) {
    return code >= NetworkV1ErrorCode.DnsNotFound && code <= NetworkV1ErrorCode.DnsRefused;
  }
  if (category === NetworkV1ErrorCategory.Transport) {
    return code >= NetworkV1ErrorCode.ConnectionRefused && code <= NetworkV1ErrorCode.BrokenPipe;
  }
  if (category === NetworkV1ErrorCategory.Tls) {
    return code >= NetworkV1ErrorCode.TlsCertificateInvalid && code <= NetworkV1ErrorCode.TlsAlert;
  }
  return category === NetworkV1ErrorCategory.Protocol &&
    (code === NetworkV1ErrorCode.HttpProtocolError || code === NetworkV1ErrorCode.MessageTooLarge);
}

function publicError(metadata: NetworkV1ErrorMetadata): NetworkError {
  const category = (NETWORK_V1_ERROR_CATEGORY_NAME_BY_ID as Readonly<Record<number, string>>)[metadata.category];
  const code = (NETWORK_V1_ERROR_NAME_BY_ID as Readonly<Record<number, string>>)[metadata.code];
  if (!category || !code || !httpErrorCompatibility(metadata.category, metadata.code)) {
    return new NetworkError("Invalid error metadata from private network binding", {
      category: "protocol",
      code: "http_protocol_error",
      operation: "http.fetch",
      protocol: "http",
    });
  }
  return new NetworkError(`HTTP request failed with ${code}`, {
    category: category as ConstructorParameters<typeof NetworkError>[1]["category"],
    code: code as ConstructorParameters<typeof NetworkError>[1]["code"],
    operation: metadata.operation,
    temporary: metadata.temporary,
    address: metadata.address,
    port: metadata.port,
    protocol: "http",
    causeCode: metadata.causeCode,
    reasonCode: metadata.reasonCode,
  });
}

function highErrorEvent(
  metadata: NetworkV1ErrorMetadata,
  operationId: number,
): Readonly<HttpRequestErrorEvent> {
  const category = (NETWORK_V1_ERROR_CATEGORY_NAME_BY_ID as Readonly<Record<number, string>>)[metadata.category];
  const code = (NETWORK_V1_ERROR_NAME_BY_ID as Readonly<Record<number, string>>)[metadata.code];
  if (!category || !code || !httpErrorCompatibility(metadata.category, metadata.code)) {
    return objectFreeze({
      eventCode: NetworkV1EventCode.HttpRequestError,
      operationId,
      category: "protocol",
      code: "http_protocol_error",
      message: "HTTP request failed with http_protocol_error",
      temporary: false,
    });
  }
  return objectFreeze({
    eventCode: NetworkV1EventCode.HttpRequestError,
    operationId,
    category: category as HttpRequestErrorEvent["category"],
    code,
    message: `HTTP request failed with ${code}`,
    temporary: metadata.temporary,
    ...(metadata.causeCode === undefined ? {} : { causeCode: metadata.causeCode }),
    ...(metadata.reasonCode === undefined ? {} : { reasonCode: metadata.reasonCode }),
  });
}

function localErrorMetadata(
  code: typeof NetworkV1ErrorCode.ResourceLimit | typeof NetworkV1ErrorCode.SystemError |
    typeof NetworkV1ErrorCode.InvalidState | typeof NetworkV1ErrorCode.Closed,
  operation: string,
): Readonly<NetworkV1ErrorMetadata> {
  return objectFreeze({
    category: NetworkV1ErrorCategory.Runtime,
    code,
    operation,
    temporary: false,
  });
}

class NetworkV1HttpAdapter {
  readonly binding: HttpClientPrivateBinding;
  readonly dispatcher: NetworkV1ServiceDispatcher;
  readonly #table: NetworkV1BindingTable;
  readonly #methods: Readonly<SafeBindingMethods>;
  readonly #runtimeGeneration: number;
  readonly #handshake: Readonly<NetworkV1Handshake>;
  readonly #slots: OperationSlot[];
  #commandSequence = 0;
  #completionSequence = 0;
  #inServiceTurn = false;
  #poisoned = false;
  #poisonError: unknown;

  constructor(
    table: NetworkV1BindingTable,
    expected: NetworkV1CompiledExpectation,
    options: AdapterTestOptions = {},
  ) {
    this.#runtimeGeneration = adoptRuntimeGeneration(table);
    const expectedHandshake = snapshotExpectedHandshake(expected, this.#runtimeGeneration);
    assertNetworkV1BindingTable(table, expectedHandshake);
    this.#handshake = expectedHandshake;
    this.#table = table;
    this.#methods = captureMethods(table);

    const maxOperations = options.maxOperations === undefined
      ? MAX_OPERATIONS
      : integer(options.maxOperations, 1, MAX_OPERATIONS, "maxOperations");
    const initialGeneration = options.initialSlotGeneration === undefined
      ? 0
      : integer(
          options.initialSlotGeneration,
          0,
          NETWORK_V1_UINT32_MAX,
          "initialSlotGeneration",
        );
    this.#slots = [];
    for (let index = 0; index < maxOperations; index += 1) {
      this.#slots.push({
        slotId: index + 1,
        generation: initialGeneration,
        phase: "retired",
        highOperationId: 0,
        operation: ABSENT_HANDLE,
        requestBody: ABSENT_HANDLE,
        responseBody: ABSENT_HANDLE,
        producer: null,
        uploadCredit: 0,
        uploadPullPending: false,
        uploadTerminal: true,
        cancelSent: false,
      });
    }

    const featureSet: string[] = [];
    for (const id of expectedHandshake.featureIds) {
      featureSet.push(
        (NETWORK_V1_FEATURE_CAPABILITY_BY_ID as Readonly<Record<number, string>>)[id]!,
      );
    }
    this.binding = objectFreeze({
      abiMajor: NETWORK_V1_ABI_MAJOR,
      abiMinor: NETWORK_V1_ABI_MINOR,
      featureSet: objectFreeze(featureSet),
      // v1.0 has no selected-provider ALPN snapshot. Keeping this absent makes
      // custom ALPN fail before I/O instead of trusting Host implementation data.
      start: (
        command: HttpRequestStartCommand,
        requestBody: HttpBodyProducer | null,
      ): HttpClientBindingOperation => this.#start(command, requestBody),
    });
    this.dispatcher = (request) => this.#serviceTurn(request);
    reflectApply(this.#methods.registerServiceDispatcher, this.#table, [this.dispatcher]);
  }

  limits(
    protocol: "http" | "websocket" | "mqtt" | "tcp" | "udp" | undefined,
    role: "client" | "server" | undefined,
  ): Readonly<{
    values: readonly Readonly<{
      name: string;
      default: number;
      hard: number;
      minimum: number;
    }>[];
    features: readonly string[];
  }> {
    this.#assertHealthy();
    const protocolId = protocol === undefined
      ? NETWORK_V1_LIMIT_PROTOCOL_ANY
      : protocol === "http"
        ? NetworkV1LimitProtocol.Http
        : protocol === "websocket"
          ? NetworkV1LimitProtocol.WebSocket
          : protocol === "mqtt"
            ? NetworkV1LimitProtocol.Mqtt
            : protocol === "tcp"
              ? NetworkV1LimitProtocol.Tcp
              : NetworkV1LimitProtocol.Udp;
    const roleId = role === undefined
      ? NETWORK_V1_LIMIT_ROLE_ANY
      : role === "client"
        ? NetworkV1LimitRole.Client
        : NetworkV1LimitRole.Server;
    const query = objectFreeze({
      runtimeGeneration: this.#runtimeGeneration,
      protocol: protocolId,
      role: roleId,
    });
    let snapshot: ReturnType<typeof snapshotNetworkV1Limits>;
    try {
      const raw = reflectApply(this.#methods.getLimits, this.#table, [query]);
      snapshot = snapshotNetworkV1Limits(query, raw, this.#handshake);
    } catch (error) {
      throw this.#poison(error);
    }
    const features: string[] = [];
    for (const id of snapshot.featureIds) {
      features.push(
        (NETWORK_V1_FEATURE_CAPABILITY_BY_ID as Readonly<Record<number, string>>)[id]!,
      );
    }
    return objectFreeze({
      values: snapshot.values,
      features: objectFreeze(features),
    });
  }

  #nextCommandSequence(): number {
    if (this.#commandSequence >= NETWORK_V1_SEQUENCE_MAX) {
      const error = new NetworkError("Network command sequence is exhausted", {
        category: "runtime",
        code: "resource_limit",
        operation: "network.dispatch",
        protocol: "http",
      });
      throw this.#poison(error);
    }
    this.#commandSequence += 1;
    return this.#commandSequence;
  }

  #identity(
    slot: OperationSlot,
    body: Readonly<NetworkV1Handle>,
  ): Readonly<NetworkV1CommandIdentity> {
    return objectFreeze({
      runtimeGeneration: this.#runtimeGeneration,
      resource: HTTP_RESOURCE,
      operation: slot.operation,
      body,
      commandSequence: this.#nextCommandSequence(),
    });
  }

  #allocate(highOperationId: number): OperationSlot {
    integer(highOperationId, 1, NETWORK_V1_SEQUENCE_MAX, "operationId");
    for (const active of this.#slots) {
      if (active.phase !== "retired" && active.highOperationId === highOperationId) {
        throw new NetworkError("HTTP operation id is already active", {
          category: "runtime",
          code: "invalid_state",
          operation: "http.fetch",
          protocol: "http",
        });
      }
    }
    for (const slot of this.#slots) {
      if (slot.phase !== "retired" || slot.generation === NETWORK_V1_UINT32_MAX) continue;
      slot.generation += 1;
      slot.phase = "headers";
      slot.highOperationId = highOperationId;
      slot.operation = objectFreeze({ id: slot.slotId, generation: slot.generation });
      slot.requestBody = ABSENT_HANDLE;
      slot.responseBody = ABSENT_HANDLE;
      slot.producer = null;
      slot.uploadCredit = 0;
      slot.uploadPullPending = false;
      slot.uploadTerminal = true;
      slot.pendingRead = undefined;
      slot.responseBodyState = undefined;
      slot.responseResolve = undefined;
      slot.responseReject = undefined;
      slot.cancelSent = false;
      return slot;
    }
    throw new NetworkError("HTTP operation capacity is exhausted", {
      category: "runtime",
      code: "resource_limit",
      operation: "http.fetch",
      protocol: "http",
    });
  }

  #retire(slot: OperationSlot): void {
    if (slot.phase === "retired") return;
    slot.phase = "retired";
    slot.highOperationId = 0;
    slot.operation = ABSENT_HANDLE;
    slot.requestBody = ABSENT_HANDLE;
    slot.responseBody = ABSENT_HANDLE;
    slot.producer = null;
    slot.uploadCredit = 0;
    slot.uploadPullPending = false;
    slot.uploadTerminal = true;
    slot.pendingRead = undefined;
    slot.responseResolve = undefined;
    slot.responseReject = undefined;
    slot.cancelSent = false;
  }

  #start(
    command: HttpRequestStartCommand,
    requestBody: HttpBodyProducer | null,
  ): HttpClientBindingOperation {
    this.#assertHealthy();
    if (command.opcode !== NetworkV1CommandOpcode.HttpRequestStart) {
      throw abiFault("high-level start used the wrong opcode");
    }
    const slot = this.#allocate(command.operationId);
    let prepared: ReturnType<typeof prepareRequestMetadata>;
    try {
      prepared = prepareRequestMetadata(command, this.binding.featureSet);
      if (prepared.metadata.hasBody !== (requestBody !== null)) {
        throw abiFault("request body presence disagrees with metadata");
      }
      if (requestBody !== null) {
        slot.requestBody = objectFreeze({ id: slot.slotId, generation: slot.generation });
        slot.producer = requestBody;
        slot.uploadTerminal = false;
      }
    } catch (error) {
      this.#retire(slot);
      throw error;
    }

    const response = new PromiseIntrinsic<HttpResponseHeadersEvent>((resolve, reject) => {
      slot.responseResolve = resolve;
      slot.responseReject = reject;
    });
    const formalCommand: NetworkV1AsyncCommand = objectFreeze({
      opcode: NetworkV1CommandOpcode.HttpRequestStart,
      identity: this.#identity(slot, slot.requestBody),
      metadata: prepared.metadata,
      ...(prepared.customCa === undefined
        ? {}
        : {
            input: objectFreeze({
              kind: NetworkV1BorrowedInputKind.CustomCa,
              bytes: prepared.customCa,
            }),
          }),
    });
    let refused: NetworkV1ErrorMetadata | undefined;
    try {
      refused = this.#dispatch(formalCommand);
    } catch (error) {
      this.#retire(slot);
      throw this.#poison(error);
    }
    if (refused) {
      const error = highErrorEvent(refused, command.operationId);
      this.#retire(slot);
      throw error;
    }

    return objectFreeze({
      response,
      cancel: (cancel: OperationCancelCommand) => this.#cancel(slot, cancel),
    });
  }

  #dispatch(command: NetworkV1AsyncCommand): NetworkV1ErrorMetadata | undefined {
    const raw = reflectApply(this.#methods.dispatch, this.#table, [command]) as unknown;
    if (typeof raw !== "object" || raw === null) throw abiFault("dispatch result is missing");
    const candidate = raw as { readonly status?: unknown; readonly error?: unknown };
    const status = candidate.status;
    if (status === NetworkV1DispatchStatus.Accepted) return undefined;
    if (status === NetworkV1DispatchStatus.Refused) {
      return snapshotError(candidate.error, "dispatch.error");
    }
    throw abiFault("async dispatch returned an invalid status");
  }

  #cancel(slot: OperationSlot, command: OperationCancelCommand): void {
    this.#assertHealthy();
    if (slot.phase === "retired" || slot.cancelSent) return;
    if (command.opcode !== NetworkV1CommandOpcode.OperationCancel ||
      command.operationId !== slot.highOperationId) {
      throw abiFault("cancel identity does not match its operation");
    }
    slot.cancelSent = true;
    const formal: NetworkV1AsyncCommand = objectFreeze({
      opcode: NetworkV1CommandOpcode.OperationCancel,
      identity: this.#identity(slot, this.#currentBody(slot)),
    });
    let refused: NetworkV1ErrorMetadata | undefined;
    try {
      refused = this.#dispatch(formal);
    } catch (error) {
      throw this.#poison(error);
    }
    if (refused) this.#failSlot(slot, publicError(refused));
  }

  #currentBody(slot: OperationSlot): Readonly<NetworkV1Handle> {
    return slot.phase === "response" ? slot.responseBody : slot.requestBody;
  }

  #serviceTurn(request: NetworkV1ServiceTurnRequest): NetworkV1ServiceTurnResult {
    this.#assertHealthy();
    if (this.#inServiceTurn) throw this.#poison(abiFault("service dispatcher is reentrant"));
    try {
      assertNetworkV1ServiceTurnRequest(request);
      if (request.runtimeGeneration !== this.#runtimeGeneration) {
        throw abiFault("service turn uses a stale runtime generation");
      }
    } catch (error) {
      throw this.#poison(error);
    }
    this.#inServiceTurn = true;
    let eventsDelivered = 0;
    let payloadBytesDelivered = 0;
    let status: typeof NetworkV1ServiceTurnStatus.Drained |
      typeof NetworkV1ServiceTurnStatus.MoreReady = NetworkV1ServiceTurnStatus.MoreReady;
    try {
      if (request.kind === NetworkV1ServiceTurnKind.Shutdown) {
        for (const slot of this.#slots) {
          if (slot.phase === "retired" || slot.cancelSent) continue;
          slot.cancelSent = true;
          const refused = this.#dispatch(objectFreeze({
            opcode: NetworkV1CommandOpcode.OperationCancel,
            identity: this.#identity(slot, this.#currentBody(slot)),
          }));
          if (refused) this.#failSlot(slot, publicError(refused));
        }
      }

      while (eventsDelivered < request.maxEvents) {
        const remainingBytes = request.maxPayloadBytes - payloadBytesDelivered;
        const polled = this.#poll(remainingBytes);
        if (polled.status === NetworkV1CompletionPollStatus.Drained) {
          status = NetworkV1ServiceTurnStatus.Drained;
          break;
        }
        if (polled.status === NetworkV1CompletionPollStatus.BudgetExhausted) {
          status = NetworkV1ServiceTurnStatus.MoreReady;
          break;
        }
        const item = polled as Extract<NetworkV1CompletionPollResult, {
          status: typeof NetworkV1CompletionPollStatus.Item;
        }>;
        if (remainingBytes === 0) {
          this.#cleanupSelectedLease(item.completion);
          throw abiFault("zero-byte readiness probe consumed a completion");
        }
        try {
          assertNetworkV1NextSequence(this.#completionSequence, item.completion.identity.sequence);
        } catch (error) {
          this.#cleanupSelectedLease(item.completion);
          throw error;
        }
        this.#completionSequence = item.completion.identity.sequence;
        payloadBytesDelivered += item.payloadBytesDelivered;
        eventsDelivered += 1;
        this.#deliver(item.completion);
      }

      if (eventsDelivered === request.maxEvents && status !== NetworkV1ServiceTurnStatus.Drained) {
        const probe = this.#poll(0);
        if (probe.status === NetworkV1CompletionPollStatus.Item) {
          this.#cleanupSelectedLease(probe.completion);
          throw abiFault("zero-byte readiness probe consumed a completion");
        }
        status = probe.status === NetworkV1CompletionPollStatus.Drained
          ? NetworkV1ServiceTurnStatus.Drained
          : NetworkV1ServiceTurnStatus.MoreReady;
      }

      const result = objectFreeze({
        status,
        eventsDelivered,
        payloadBytesDelivered,
        lastSequence: eventsDelivered === 0 ? 0 : this.#completionSequence,
      });
      assertNetworkV1ServiceTurnResult(request, result);
      return result;
    } catch (error) {
      throw this.#poison(error);
    } finally {
      this.#inServiceTurn = false;
    }
  }

  #poll(maxPayloadBytes: number): NetworkV1CompletionPollResult {
    const pollRequest = objectFreeze({
      runtimeGeneration: this.#runtimeGeneration,
      maxPayloadBytes,
    });
    const raw = reflectApply(this.#methods.nextCompletion, this.#table, [pollRequest]) as unknown;
    if (typeof raw !== "object" || raw === null) throw abiFault("completion poll result is missing");
    const candidate = raw as {
      readonly status?: unknown;
      readonly completion?: unknown;
      readonly payloadBytesDelivered?: unknown;
    };
    const status = candidate.status;
    let result: NetworkV1CompletionPollResult;
    let selectedCompletion: NetworkV1Completion | undefined;
    if (status === NetworkV1CompletionPollStatus.Item) {
      selectedCompletion = snapshotCompletion(candidate.completion);
      try {
        const payload = candidate.payloadBytesDelivered;
        result = objectFreeze({
          status,
          completion: selectedCompletion,
          payloadBytesDelivered: integer(
            payload,
            0,
            maxPayloadBytes,
            "completionPoll.payloadBytesDelivered",
          ),
        });
        assertNetworkV1CompletionPollResult(pollRequest, result);
      } catch (error) {
        try {
          this.#cleanupSelectedLease(selectedCompletion);
        } catch (releaseError) {
          throw releaseError;
        }
        throw error;
      }
      return result;
    } else if (status === NetworkV1CompletionPollStatus.Drained ||
      status === NetworkV1CompletionPollStatus.BudgetExhausted) {
      const payload = candidate.payloadBytesDelivered;
      if (payload !== 0) throw abiFault("non-item completion poll consumed payload bytes");
      result = objectFreeze({ status, payloadBytesDelivered: 0 });
    } else {
      throw abiFault("completion poll returned an unknown status");
    }
    assertNetworkV1CompletionPollResult(pollRequest, result);
    return result;
  }

  #slotFor(identity: NetworkV1CompletionIdentity): OperationSlot | undefined {
    const index = identity.operation.id - 1;
    if (index < 0 || index >= this.#slots.length) return undefined;
    const slot = this.#slots[index]!;
    if (slot.phase === "retired" || !networkV1SameHandle(identity.operation, slot.operation)) {
      return undefined;
    }
    return slot;
  }

  #deliver(completion: NetworkV1Completion): void {
    const slot = this.#slotFor(completion.identity);
    if (!slot) {
      this.#cleanupSelectedLease(completion);
      return;
    }
    if (!networkV1SameHandle(completion.identity.resource, HTTP_RESOURCE)) {
      this.#cleanupSelectedLease(completion);
      throw abiFault("completion resource does not match the active HTTP client");
    }

    switch (completion.eventCode) {
      case NetworkV1EventCode.BodyPull:
        this.#onUploadPull(slot, completion.identity, completion.maxBytes);
        return;
      case NetworkV1EventCode.BodyCancel:
        this.#onUploadCancel(slot, completion.identity);
        return;
      case NetworkV1EventCode.HttpResponseHeaders:
        this.#onHeaders(slot, completion.identity, completion.metadata);
        return;
      case NetworkV1EventCode.HttpRequestError:
        this.#onRequestError(slot, completion.error);
        return;
      case NetworkV1EventCode.BodyChunk:
        this.#onResponseChunk(slot, completion);
        return;
      case NetworkV1EventCode.BodyEnd:
        this.#onResponseEnd(slot, completion.identity);
        return;
      case NetworkV1EventCode.BodyError:
        this.#onResponseError(slot, completion.identity, completion.error);
        return;
    }
  }

  #onUploadPull(
    slot: OperationSlot,
    identity: NetworkV1CompletionIdentity,
    maxBytes: number,
  ): void {
    if (slot.phase !== "headers" || slot.uploadTerminal || !slot.producer ||
      !networkV1SameHandle(identity.body, slot.requestBody) ||
      slot.uploadCredit !== 0 || slot.uploadPullPending) {
      throw abiFault("BODY_PULL violates request-body credit state");
    }
    slot.uploadCredit = maxBytes;
    slot.uploadPullPending = true;
    let pulled: unknown;
    try {
      pulled = slot.producer.pull(maxBytes);
    } catch (error) {
      this.#completeUploadPull(slot, maxBytes, undefined, error);
      return;
    }
    const normalized = reflectApply(promiseResolve, PromiseIntrinsic, [pulled]) as Promise<unknown>;
    reflectApply(promiseThen, normalized, [
      (chunk: unknown) => this.#completeUploadPull(slot, maxBytes, chunk, undefined),
      (error: unknown) => this.#completeUploadPull(slot, maxBytes, undefined, error),
    ]);
  }

  #completeUploadPull(
    slot: OperationSlot,
    credit: number,
    chunk: unknown,
    pullError: unknown,
  ): void {
    if (slot.phase !== "headers" || slot.uploadTerminal ||
      !slot.uploadPullPending || slot.uploadCredit !== credit) return;
    slot.uploadPullPending = false;
    slot.uploadCredit = 0;
    let command: NetworkV1AsyncCommand;
    if (pullError !== undefined) {
      slot.uploadTerminal = true;
      command = objectFreeze({
        opcode: NetworkV1CommandOpcode.BodyError,
        identity: this.#identity(slot, slot.requestBody),
        error: objectFreeze({
          ...localErrorMetadata(NetworkV1ErrorCode.SystemError, "http.fetch.upload"),
          causeCode: "guest_body",
        }),
      });
    } else if (chunk === null) {
      slot.uploadTerminal = true;
      command = objectFreeze({
        opcode: NetworkV1CommandOpcode.BodyEnd,
        identity: this.#identity(slot, slot.requestBody),
      });
    } else {
      let bytes: Uint8Array;
      try {
        bytes = snapshotUint8Array(chunk, credit, "HTTP request body chunk");
        if (bytes.byteLength === 0) throw abiFault("request BODY_CHUNK must not be empty");
      } catch {
        slot.uploadTerminal = true;
        command = objectFreeze({
          opcode: NetworkV1CommandOpcode.BodyError,
          identity: this.#identity(slot, slot.requestBody),
          error: localErrorMetadata(NetworkV1ErrorCode.InvalidState, "http.fetch.upload"),
        });
        let refused: NetworkV1ErrorMetadata | undefined;
        try {
          refused = this.#dispatch(command);
        } catch (error) {
          this.#poison(error);
          return;
        }
        if (refused) this.#failSlot(slot, publicError(refused));
        return;
      }
      command = objectFreeze({
        opcode: NetworkV1CommandOpcode.BodyChunk,
        identity: this.#identity(slot, slot.requestBody),
        input: objectFreeze({
          kind: NetworkV1BorrowedInputKind.BodyChunk,
          bytes,
        }),
      });
    }
    let refused: NetworkV1ErrorMetadata | undefined;
    try {
      refused = this.#dispatch(command);
    } catch (error) {
      this.#poison(error);
      return;
    }
    if (refused) this.#failSlot(slot, publicError(refused));
  }

  #stopUpload(slot: OperationSlot, reason: unknown): void {
    if (slot.uploadTerminal) return;
    slot.uploadTerminal = true;
    slot.uploadCredit = 0;
    slot.uploadPullPending = false;
    const producer = slot.producer;
    slot.producer = null;
    if (!producer) return;
    try {
      const cancelled = producer.cancel(reason);
      const normalized = reflectApply(promiseResolve, PromiseIntrinsic, [cancelled]) as Promise<unknown>;
      reflectApply(promiseThen, normalized, [() => undefined, () => undefined]);
    } catch {
      // The Core's terminal claim remains authoritative.
    }
  }

  #onUploadCancel(slot: OperationSlot, identity: NetworkV1CompletionIdentity): void {
    if (slot.phase !== "headers" || !networkV1SameHandle(identity.body, slot.requestBody)) {
      throw abiFault("BODY_CANCEL does not identify the request producer");
    }
    this.#stopUpload(slot, publicError(localErrorMetadata(
      NetworkV1ErrorCode.Closed,
      "http.fetch.upload",
    ) as NetworkV1ErrorMetadata));
  }

  #onHeaders(
    slot: OperationSlot,
    identity: NetworkV1CompletionIdentity,
    metadata: NetworkV1HttpResponseMetadata,
  ): void {
    if (slot.phase !== "headers") throw abiFault("response headers were delivered twice");
    const reason = new NetworkError("HTTP request upload ended at response headers", {
      category: "runtime",
      code: "closed",
      operation: "http.fetch.upload",
      protocol: "http",
    });
    this.#stopUpload(slot, reason);
    slot.phase = "response";
    slot.responseBody = identity.body;
    const hasBody = !networkV1HandleIsAbsent(identity.body);
    const responseBodyState: ResponseBodyState | undefined = hasBody
      ? { phase: "open" }
      : undefined;
    slot.responseBodyState = responseBodyState;
    const event: HttpResponseHeadersEvent = objectFreeze({
      eventCode: NetworkV1EventCode.HttpResponseHeaders,
      operationId: slot.highOperationId,
      status: metadata.status,
      statusText: metadata.statusText,
      headers: metadata.headers,
      url: metadata.url,
      redirected: metadata.redirected,
      ...(responseBodyState === undefined
        ? {}
        : { body: this.#responseBodyStream(slot, responseBodyState) }),
      bufferedBodyBytes: metadata.bufferedBodyBytes,
    });
    const resolve = slot.responseResolve;
    slot.responseResolve = undefined;
    slot.responseReject = undefined;
    if (!resolve) throw abiFault("response promise is missing");
    resolve(event);
    if (!hasBody) this.#retire(slot);
  }

  #onRequestError(slot: OperationSlot, metadata: NetworkV1ErrorMetadata): void {
    const event = highErrorEvent(metadata, slot.highOperationId);
    const error = publicError(metadata);
    this.#stopUpload(slot, event);
    if (slot.phase === "headers") slot.responseReject?.(event);
    else {
      this.#failResponseBody(slot, error);
      slot.pendingRead?.reject(error);
    }
    this.#retire(slot);
  }

  #responseBodyStream(slot: OperationSlot, state: ResponseBodyState): BodyStream {
    const readInto = (destination: Uint8Array): Promise<{ bytes: number; done: boolean }> => {
      let capacity: number;
      try {
        capacity = actualUint8ArrayLength(destination, "response body destination");
        integer(capacity, 1, NETWORK_V1_UINT32_MAX, "response body destination length");
      } catch (error) {
        return PromiseIntrinsic.reject(error);
      }
      if (state.phase === "errored") return PromiseIntrinsic.reject(state.error);
      if (state.phase === "ended") {
        return PromiseIntrinsic.resolve(objectFreeze({ bytes: 0, done: true }));
      }
      try {
        this.#assertHealthy();
      } catch (error) {
        return PromiseIntrinsic.reject(error);
      }
      if (slot.phase !== "response" || slot.responseBodyState !== state || slot.pendingRead) {
        return PromiseIntrinsic.reject(new NetworkError("HTTP response body already has a pending read", {
          category: "runtime",
          code: "busy",
          operation: "http.body.readInto",
          protocol: "http",
        }));
      }
      return new PromiseIntrinsic((resolve, reject) => {
        slot.pendingRead = { destination, capacity, resolve, reject };
        let refused: NetworkV1ErrorMetadata | undefined;
        try {
          refused = this.#dispatch(objectFreeze({
            opcode: NetworkV1CommandOpcode.BodyPull,
            identity: this.#identity(slot, slot.responseBody),
            maxBytes: Math.min(capacity, HTTP_BODY_CHUNK_BYTES),
          }));
        } catch (error) {
          reject(this.#poison(error));
          return;
        }
        if (refused) {
          slot.pendingRead = undefined;
          reject(publicError(refused));
        }
      });
    };
    const cancel = (_reason?: unknown): Promise<void> => {
      if (state.phase === "errored") return PromiseIntrinsic.reject(state.error);
      if (state.phase === "ended") return PromiseIntrinsic.resolve();
      try {
        this.#assertHealthy();
      } catch (error) {
        return PromiseIntrinsic.reject(error);
      }
      if (slot.phase !== "response" || slot.responseBodyState !== state) {
        return PromiseIntrinsic.reject(this.#poison(
          abiFault("response body stream lost its active operation"),
        ));
      }
      const pending = slot.pendingRead;
      let refused: NetworkV1ErrorMetadata | undefined;
      try {
        refused = this.#dispatch(objectFreeze({
          opcode: NetworkV1CommandOpcode.BodyCancel,
          identity: this.#identity(slot, slot.responseBody),
        }));
      } catch (error) {
        return PromiseIntrinsic.reject(this.#poison(error));
      }
      if (refused) {
        const error = publicError(refused);
        slot.pendingRead = undefined;
        pending?.reject(error);
        this.#failResponseBody(slot, error);
        this.#retire(slot);
        return PromiseIntrinsic.reject(error);
      }
      slot.pendingRead = undefined;
      pending?.resolve(objectFreeze({ bytes: 0, done: true }));
      state.phase = "ended";
      this.#retire(slot);
      return PromiseIntrinsic.resolve();
    };
    const stream: BodyStream = {
      readInto,
      cancel,
      [Symbol.asyncIterator](): AsyncIterableIterator<Uint8Array> {
        return {
          async next(): Promise<IteratorResult<Uint8Array>> {
            const destination = new Uint8Array(HTTP_BODY_CHUNK_BYTES);
            const result = await readInto(destination);
            return result.done
              ? { value: undefined, done: true }
              : { value: uint8ArraySlice.call(destination, 0, result.bytes), done: false };
          },
          async return(): Promise<IteratorResult<Uint8Array>> {
            await cancel();
            return { value: undefined, done: true };
          },
          [Symbol.asyncIterator]() { return this; },
        };
      },
    };
    return objectFreeze(stream);
  }

  #onResponseChunk(
    slot: OperationSlot,
    completion: Extract<NetworkV1Completion, {
      eventCode: typeof NetworkV1EventCode.BodyChunk;
    }>,
  ): void {
    if (slot.phase !== "response" ||
      !networkV1SameHandle(completion.identity.body, slot.responseBody)) {
      this.#cleanupSelectedLease(completion);
      throw abiFault("response BODY_CHUNK identifies the wrong body");
    }
    const pending = slot.pendingRead;
    if (!pending || completion.payload.byteLength >
      Math.min(pending.capacity, HTTP_BODY_CHUNK_BYTES)) {
      this.#cleanupSelectedLease(completion);
      throw abiFault("response BODY_CHUNK has no matching credit");
    }
    slot.pendingRead = undefined;
    try {
      this.#copyLease(completion, pending.destination);
      pending.resolve(objectFreeze({ bytes: completion.payload.byteLength, done: false }));
    } catch (error) {
      pending.reject(error);
      throw error;
    }
  }

  #onResponseEnd(slot: OperationSlot, identity: NetworkV1CompletionIdentity): void {
    if (slot.phase !== "response" || !networkV1SameHandle(identity.body, slot.responseBody)) {
      throw abiFault("response BODY_END identifies the wrong body");
    }
    const pending = slot.pendingRead;
    slot.pendingRead = undefined;
    if (slot.responseBodyState) slot.responseBodyState.phase = "ended";
    pending?.resolve(objectFreeze({ bytes: 0, done: true }));
    this.#retire(slot);
  }

  #onResponseError(
    slot: OperationSlot,
    identity: NetworkV1CompletionIdentity,
    metadata: NetworkV1ErrorMetadata,
  ): void {
    if (slot.phase !== "response" || !networkV1SameHandle(identity.body, slot.responseBody)) {
      throw abiFault("response BODY_ERROR identifies the wrong body");
    }
    const pending = slot.pendingRead;
    slot.pendingRead = undefined;
    const error = publicError(metadata);
    this.#failResponseBody(slot, error);
    pending?.reject(error);
    this.#retire(slot);
  }

  #leaseIdentity(completion: NetworkV1CompletionIdentity): Readonly<NetworkV1CommandIdentity> {
    return objectFreeze({
      runtimeGeneration: this.#runtimeGeneration,
      resource: completion.resource,
      operation: completion.operation,
      body: completion.body,
      commandSequence: this.#nextCommandSequence(),
    });
  }

  #copyLease(
    completion: Extract<NetworkV1Completion, {
      eventCode: typeof NetworkV1EventCode.BodyChunk;
    }>,
    destination: Uint8Array,
  ): void {
    const take: NetworkV1BufferLeaseTakeCommand = objectFreeze({
      opcode: NetworkV1CommandOpcode.BufferLeaseTake,
      identity: this.#leaseIdentity(completion.identity),
      lease: completion.payload.lease,
      byteLength: completion.payload.byteLength,
    });
    const takeResult = reflectApply(this.#methods.leaseTake, this.#table, [take]) as unknown;
    const takeCompleted = this.#leaseTakeCompleted(takeResult);
    let takenLength = 0;
    let offset = 0;
    let failure: unknown;
    try {
      takenLength = this.#snapshotLeaseTakeResult(takeResult);
      if (takenLength !== completion.payload.byteLength) {
        throw abiFault("lease take length disagrees with its completion");
      }
      while (offset < takenLength) {
        const window = destination.subarray(offset, takenLength);
        const read: NetworkV1BufferLeaseReadIntoCommand = objectFreeze({
          opcode: NetworkV1CommandOpcode.BufferLeaseReadInto,
          identity: this.#leaseIdentity(completion.identity),
          lease: completion.payload.lease,
          offset,
          maxBytes: window.byteLength,
        });
        const raw = reflectApply(this.#methods.leaseReadInto, this.#table, [read, window]) as unknown;
        const copied = this.#snapshotLeaseReadResult(raw, window.byteLength);
        offset += copied;
      }
    } catch (error) {
      failure = error;
    }
    if (takeCompleted) {
      try {
        this.#releaseTakenLease(completion.identity, completion.payload.lease);
      } catch (releaseError) {
        failure = releaseError;
      }
    }
    if (failure !== undefined) throw failure;
  }

  #leaseTakeCompleted(value: unknown): boolean {
    return typeof value === "object" && value !== null &&
      (value as { readonly status?: unknown }).status === NetworkV1DispatchStatus.Completed;
  }

  #snapshotLeaseTakeResult(value: unknown): number {
    if (typeof value !== "object" || value === null) throw abiFault("leaseTake result is missing");
    const candidate = value as { readonly status?: unknown; readonly byteLength?: unknown; readonly error?: unknown };
    const status = candidate.status;
    if (status === NetworkV1DispatchStatus.Refused) throw publicError(snapshotError(candidate.error, "leaseTake.error"));
    if (status !== NetworkV1DispatchStatus.Completed) throw abiFault("leaseTake did not complete synchronously");
    return integer(candidate.byteLength, 1, NETWORK_V1_UINT32_MAX, "leaseTake.byteLength");
  }

  #snapshotLeaseReadResult(value: unknown, maximum: number): number {
    if (typeof value !== "object" || value === null) throw abiFault("leaseReadInto result is missing");
    const candidate = value as { readonly status?: unknown; readonly bytesCopied?: unknown; readonly error?: unknown };
    const status = candidate.status;
    if (status === NetworkV1DispatchStatus.Refused) throw publicError(snapshotError(candidate.error, "leaseReadInto.error"));
    if (status !== NetworkV1DispatchStatus.Completed) throw abiFault("leaseReadInto did not complete synchronously");
    return integer(candidate.bytesCopied, 1, maximum, "leaseReadInto.bytesCopied");
  }

  #releaseTakenLease(
    identity: NetworkV1CompletionIdentity,
    lease: NetworkV1Handle,
  ): void {
    const command: NetworkV1BufferLeaseReleaseCommand = objectFreeze({
      opcode: NetworkV1CommandOpcode.BufferLeaseRelease,
      identity: this.#leaseIdentity(identity),
      lease,
    });
    const raw = reflectApply(this.#methods.leaseRelease, this.#table, [command]) as unknown;
    if (typeof raw !== "object" || raw === null) throw abiFault("leaseRelease result is missing");
    const candidate = raw as { readonly status?: unknown; readonly error?: unknown };
    if (candidate.status === NetworkV1DispatchStatus.Refused) {
      throw publicError(snapshotError(candidate.error, "leaseRelease.error"));
    }
    if (candidate.status !== NetworkV1DispatchStatus.Completed) {
      throw abiFault("leaseRelease did not complete synchronously");
    }
  }

  #cleanupSelectedLease(completion: NetworkV1Completion): void {
    if (completion.eventCode !== NetworkV1EventCode.BodyChunk) return;
    const take: NetworkV1BufferLeaseTakeCommand = objectFreeze({
      opcode: NetworkV1CommandOpcode.BufferLeaseTake,
      identity: this.#leaseIdentity(completion.identity),
      lease: completion.payload.lease,
      byteLength: completion.payload.byteLength,
    });
    const raw = reflectApply(this.#methods.leaseTake, this.#table, [take]) as unknown;
    const takeCompleted = this.#leaseTakeCompleted(raw);
    let failure: unknown;
    try {
      const length = this.#snapshotLeaseTakeResult(raw);
      if (length !== completion.payload.byteLength) {
        throw abiFault("stale lease take length is inconsistent");
      }
    } catch (error) {
      failure = error;
    }
    if (takeCompleted) {
      try {
        this.#releaseTakenLease(completion.identity, completion.payload.lease);
      } catch (releaseError) {
        failure = releaseError;
      }
    }
    if (failure !== undefined) throw failure;
  }

  #failResponseBody(slot: OperationSlot, error: unknown): void {
    const state = slot.responseBodyState;
    if (!state || state.phase !== "open") return;
    state.phase = "errored";
    state.error = error;
  }

  #failSlot(slot: OperationSlot, error: unknown): void {
    this.#stopUpload(slot, error);
    this.#failResponseBody(slot, error);
    slot.responseReject?.(error);
    slot.pendingRead?.reject(error);
    this.#retire(slot);
  }

  #failAll(error: unknown): void {
    for (const slot of this.#slots) {
      if (slot.phase !== "retired") this.#failSlot(slot, error);
    }
  }

  #assertHealthy(): void {
    if (this.#poisoned) throw this.#poisonError;
  }

  #poison(error: unknown): unknown {
    if (!this.#poisoned) {
      this.#poisoned = true;
      this.#poisonError = error;
      this.#failAll(error);
    }
    return this.#poisonError;
  }
}

function prepareRequestMetadata(
  command: HttpRequestStartCommand,
  featureSet: readonly string[],
): Readonly<{
  metadata: NetworkV1HttpRequestMetadata;
  customCa?: Uint8Array;
}> {
  const url = byteString(command.url, MAX_URL_BYTES, "request.url");
  const parsed = canonicalizeHttpUrl(url);
  if (parsed.href !== url || parsed.hasFragment ||
    (parsed.scheme !== "http" && parsed.scheme !== "https")) {
    throw abiFault("request URL is not a canonical HTTP(S) URL without a fragment");
  }
  const method = byteString(command.method, 64, "request.method");
  if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(method) ||
    method === "CONNECT" || method === "TRACE" || method === "TRACK") {
    throw abiFault("request method is invalid");
  }
  const headers = snapshotHeaders(command.headers, "request.headers");
  if (typeof command.hasBody !== "boolean" || typeof command.ref !== "boolean") {
    throw abiFault("request boolean metadata is invalid");
  }
  const redirect = command.redirect === "follow"
    ? NetworkV1HttpRedirectMode.Follow
    : command.redirect === "manual"
      ? NetworkV1HttpRedirectMode.Manual
      : command.redirect === "error"
        ? NetworkV1HttpRedirectMode.Error
        : undefined;
  if (redirect === undefined) throw abiFault("request redirect mode is invalid");
  const maxRedirects = integer(command.maxRedirects, 0, 5, "request.maxRedirects");

  const timeoutSource = command.timeouts;
  const timeouts = objectFreeze({
    connectMs: timeoutSource?.connect === undefined
      ? 0
      : integer(timeoutSource.connect, 1, NETWORK_V1_UINT32_MAX, "timeouts.connect"),
    headersMs: timeoutSource?.headers === undefined
      ? 0
      : integer(timeoutSource.headers, 1, NETWORK_V1_UINT32_MAX, "timeouts.headers"),
    idleMs: timeoutSource?.idle === undefined
      ? 0
      : integer(timeoutSource.idle, 1, NETWORK_V1_UINT32_MAX, "timeouts.idle"),
    totalMs: timeoutSource?.total === undefined
      ? 0
      : integer(timeoutSource.total, 1, NETWORK_V1_UINT32_MAX, "timeouts.total"),
  });

  const limits: { name: string; value: number }[] = [];
  if (command.limits !== undefined) {
    const names = objectKeys(command.limits);
    if (names.length > MAX_LIMIT_OVERRIDES) throw abiFault("request limits exceed their count ceiling");
    names.sort();
    for (const name of names) {
      if (name.length === 0 || name.length > MAX_LIMIT_NAME_BYTES ||
        !/^[A-Za-z][A-Za-z0-9._-]*$/.test(name)) {
        throw abiFault("request limit name is invalid");
      }
      limits.push(objectFreeze({
        name,
        value: integer(
          command.limits[name],
          1,
          NETWORK_V1_UINT32_MAX,
          `request limit ${name}`,
        ),
      }));
    }
  }

  let tls: NetworkV1HttpRequestMetadata["tls"] = null;
  let customCa: Uint8Array | undefined;
  if (parsed.scheme === "https") {
    const source = command.tls;
    const hasV13 = featureSet.includes("network.http.client.tls.v1-3");
    const minVersion = source?.minVersion === "1.3"
      ? NetworkV1TlsVersion.V13
      : NetworkV1TlsVersion.V12;
    const maxVersion = source?.maxVersion === "1.2"
      ? NetworkV1TlsVersion.V12
      : source?.maxVersion === "1.3" || hasV13
        ? NetworkV1TlsVersion.V13
        : NetworkV1TlsVersion.V12;
    if (minVersion > maxVersion) throw abiFault("TLS version interval is empty");
    const alpn: string[] = [];
    let alpnBytes = 0;
    if (source?.alpn !== undefined) {
      if (!arrayIsArray(source.alpn) || source.alpn.length > MAX_ALPN_TOKENS) {
        throw abiFault("TLS ALPN list is invalid");
      }
      for (const rawToken of source.alpn) {
        const token = byteString(rawToken, 255, "TLS ALPN token");
        if (token.length === 0 || token.includes("\0") || alpn.includes(token)) {
          throw abiFault("TLS ALPN token is invalid or duplicate");
        }
        alpnBytes += token.length;
        if (alpnBytes > MAX_ALPN_BYTES) throw abiFault("TLS ALPN list exceeds its byte ceiling");
        alpn.push(token);
      }
    }
    if (source?.ca !== undefined) {
      customCa = snapshotUint8Array(source.ca, MAX_CA_BYTES, "TLS custom CA");
      if (customCa.byteLength === 0) throw abiFault("TLS custom CA must not be empty");
    }
    const hostnameIsIp = parsed.hostname.includes(":") || /^\d+(?:\.\d+){3}$/.test(parsed.hostname);
    const serverName = hostnameIsIp
      ? ""
      : byteString(source?.serverName ?? parsed.hostname, 253, "TLS serverName");
    const credential = source?.credential === undefined
      ? ""
      : byteString(source.credential, 128, "TLS credential");
    const clientCertificate = source?.clientCertificate === "optional"
      ? NetworkV1ClientCertificateMode.Optional
      : source?.clientCertificate === "required"
        ? NetworkV1ClientCertificateMode.Required
        : NetworkV1ClientCertificateMode.None;
    const verification = source?.verification === "development-insecure"
      ? NetworkV1TlsVerification.DevelopmentInsecure
      : NetworkV1TlsVerification.Full;
    const revocation = source?.revocation === "required"
      ? NetworkV1TlsRevocation.Required
      : NetworkV1TlsRevocation.HostDefault;
    tls = objectFreeze({
      serverName,
      minVersion,
      maxVersion,
      alpn: objectFreeze(alpn),
      credential,
      clientCertificate,
      verification,
      revocation,
      customCaBytes: customCa?.byteLength ?? 0,
    });
  } else if (command.tls !== undefined) {
    throw abiFault("plaintext HTTP request contains TLS metadata");
  }

  return objectFreeze({
    metadata: objectFreeze({
      url,
      method,
      headers,
      hasBody: command.hasBody,
      redirect,
      timeouts,
      maxRedirects,
      tls,
      limits: objectFreeze(limits),
      ref: command.ref,
    }),
    ...(customCa === undefined ? {} : { customCa }),
  });
}

/** Test-only constructor; it validates and registers but does not install globally. */
export function createNetworkV1HttpBindingAdapterForTesting(
  table: NetworkV1BindingTable,
  expected: NetworkV1CompiledExpectation,
  options?: AdapterTestOptions,
): Readonly<{
  binding: HttpClientPrivateBinding;
  dispatcher: NetworkV1ServiceDispatcher;
  limits: NetworkV1HttpAdapter["limits"];
}> {
  const adapter = new NetworkV1HttpAdapter(table, expected, options);
  return objectFreeze({
    binding: adapter.binding,
    dispatcher: adapter.dispatcher,
    limits: (protocol, role) => adapter.limits(protocol, role),
  });
}

let runtimeMounted = false;

/** Called only by the compiler-generated private module before the app entry. */
export function mountNetworkV1HttpBinding(
  table: unknown,
  expected: NetworkV1CompiledExpectation,
): void {
  if (runtimeMounted) throw abiFault("runtime binding was already mounted");
  runtimeMounted = true;
  const adapter = new NetworkV1HttpAdapter(table as NetworkV1BindingTable, expected);
  installNetworkLimitsProvider((protocol, role) => adapter.limits(protocol, role));
  installHttpClientBindingForRuntime(adapter.binding);
}
