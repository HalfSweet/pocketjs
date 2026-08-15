import { NetworkError } from "./index.ts";

/**
 * SDK-side ceilings used before a Host-admitted limit table is attached.
 * Targets may only lower these values through the private binding metadata.
 */
export const HTTP_BODY_CHUNK_BYTES = 64 * 1024;
export const HTTP_BODY_TEE_BRANCH_BYTES = 256 * 1024;
export const HTTP_BODY_HELPER_BYTES = 8 * 1024 * 1024;
const HTTP_BUFFERED_BODY_INPUT_BYTES = 8 * 1024 * 1024;
const HTTP_BODY_EMPTY_CHUNK_LIMIT = 1024;
const HTTP_BODY_CLONE_BRANCHES = 8;
const HTTP_BODY_TEE_SEGMENT_BYTES = HTTP_BODY_CHUNK_BYTES;

export interface BodyStream extends AsyncIterable<Uint8Array> {
  readInto(destination: Uint8Array): Promise<{ bytes: number; done: boolean }>;
  cancel(reason?: unknown): Promise<void>;
}

export interface HttpBodyProducer {
  pull(maxBytes: number): Promise<Uint8Array | null>;
  cancel(reason?: unknown): Promise<void>;
}

interface BodyCloneGroup {
  branches: number;
  readonly controllers: Set<BodyController>;
  readonly terminalCallbacks: Set<() => void>;
}

interface BodySource {
  pull(maxBytes: number): Promise<Uint8Array | null>;
  cancel(reason?: unknown): Promise<void>;
}

const typedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype) as object;
const typedArrayByteLength = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  "byteLength",
)!.get!;
const typedArrayByteOffset = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  "byteOffset",
)!.get!;
const typedArrayBuffer = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  "buffer",
)!.get!;
const typedArrayTag = Object.getOwnPropertyDescriptor(
  typedArrayPrototype,
  Symbol.toStringTag,
)!.get!;
const dataViewByteLength = Object.getOwnPropertyDescriptor(
  DataView.prototype,
  "byteLength",
)!.get!;
const dataViewByteOffset = Object.getOwnPropertyDescriptor(
  DataView.prototype,
  "byteOffset",
)!.get!;
const dataViewBuffer = Object.getOwnPropertyDescriptor(
  DataView.prototype,
  "buffer",
)!.get!;
const arrayBufferByteLength = Object.getOwnPropertyDescriptor(
  ArrayBuffer.prototype,
  "byteLength",
)!.get!;
const uint8ArraySet = Uint8Array.prototype.set;
const arrayBufferIsView = ArrayBuffer.isView;
const textEncoder = new TextEncoder();
const textEncoderEncode = TextEncoder.prototype.encode;

interface IntrinsicViewSnapshot {
  readonly buffer: ArrayBuffer;
  readonly byteOffset: number;
  readonly byteLength: number;
}

function intrinsicUint8ArraySnapshot(
  value: unknown,
  label: string,
): IntrinsicViewSnapshot {
  let tag: unknown;
  let snapshot: IntrinsicViewSnapshot;
  try {
    tag = typedArrayTag.call(value);
    snapshot = intrinsicViewSnapshot(value as ArrayBufferView);
  } catch {
    throw new TypeError(`${label} must be a Uint8Array`);
  }
  if (tag !== "Uint8Array") throw new TypeError(`${label} must be a Uint8Array`);
  return snapshot;
}

function intrinsicViewSnapshot(view: ArrayBufferView): IntrinsicViewSnapshot {
  try {
    return {
      buffer: typedArrayBuffer.call(view) as ArrayBuffer,
      byteOffset: typedArrayByteOffset.call(view) as number,
      byteLength: typedArrayByteLength.call(view) as number,
    };
  } catch {
    return {
      buffer: dataViewBuffer.call(view) as ArrayBuffer,
      byteOffset: dataViewByteOffset.call(view) as number,
      byteLength: dataViewByteLength.call(view) as number,
    };
  }
}

function copyIntrinsicBytes(snapshot: IntrinsicViewSnapshot): Uint8Array {
  const source = new Uint8Array(
    snapshot.buffer,
    snapshot.byteOffset,
    snapshot.byteLength,
  );
  const copy = new Uint8Array(snapshot.byteLength);
  uint8ArraySet.call(copy, source);
  return copy;
}

/** Snapshot a genuine Uint8Array without invoking user-visible getters or iteration. */
export function snapshotUint8Array(
  value: unknown,
  maximumBytes: number,
  label: string,
): Uint8Array {
  const snapshot = intrinsicUint8ArraySnapshot(value, label);
  if (snapshot.byteLength > maximumBytes) {
    throw bodyError(
      "resource_limit",
      "http.body",
      `${label} exceeds ${maximumBytes} bytes`,
    );
  }
  try {
    return copyIntrinsicBytes(snapshot);
  } catch {
    throw bodyError(
      "invalid_state",
      "http.body",
      `${label} uses a detached buffer`,
    );
  }
}

/** Return the backing buffer of an SDK-owned, exact-length byte array. */
export function ownedUint8ArrayBuffer(value: Uint8Array): ArrayBuffer {
  return typedArrayBuffer.call(value) as ArrayBuffer;
}

type ReaderKind = "readInto" | "iterator" | "helper" | "binding";

function bodyError(
  code: "busy" | "invalid_state" | "resource_limit",
  operation: string,
  message: string,
): NetworkError {
  return new NetworkError(message, {
    category: "runtime",
    code,
    operation,
    protocol: "http",
  });
}

function assertPositiveCapacity(maxBytes: number, operation: string): void {
  if (!Number.isSafeInteger(maxBytes) || maxBytes <= 0) {
    throw bodyError(
      "invalid_state",
      operation,
      "HTTP body capacity must be a positive safe integer",
    );
  }
}

function copyView(view: ArrayBufferView): Uint8Array {
  try {
    return copyIntrinsicBytes(intrinsicViewSnapshot(view));
  } catch {
    throw bodyError(
      "invalid_state",
      "http.body",
      "HTTP body input uses a detached buffer",
    );
  }
}

function copyArrayBuffer(buffer: ArrayBuffer): Uint8Array {
  try {
    const byteLength = arrayBufferByteLength.call(buffer) as number;
    return copyIntrinsicBytes({ buffer, byteOffset: 0, byteLength });
  } catch {
    throw bodyError(
      "invalid_state",
      "http.body",
      "HTTP body input uses a detached buffer",
    );
  }
}

class MemoryBodySource implements BodySource {
  #offset = 0;

  constructor(private readonly bytes: Uint8Array) {}

  async pull(maxBytes: number): Promise<Uint8Array | null> {
    if (this.#offset === this.bytes.byteLength) return null;
    const end = Math.min(this.bytes.byteLength, this.#offset + maxBytes);
    const result = this.bytes.subarray(this.#offset, end);
    this.#offset = end;
    return result;
  }

  async cancel(): Promise<void> {
    this.#offset = this.bytes.byteLength;
  }
}

class AsyncIterableBodySource implements BodySource {
  #iterator: AsyncIterator<Uint8Array> | undefined;
  #remainder: Uint8Array | undefined;
  #done = false;
  #returnCalled = false;

  constructor(
    private readonly iterable: AsyncIterable<Uint8Array>,
    private readonly iteratorFactory: () => AsyncIterator<Uint8Array>,
  ) {}

  async pull(maxBytes: number): Promise<Uint8Array | null> {
    if (this.#done) return null;
    if (this.#remainder) return this.#takeRemainder(maxBytes);

    this.#iterator ??= this.iteratorFactory.call(this.iterable);
    let emptyChunks = 0;
    for (;;) {
      const item = await this.#iterator.next();
      if (typeof item !== "object" || item === null) {
        throw bodyError(
          "invalid_state",
          "http.body.pull",
          "HTTP async body iterator returned an invalid result",
        );
      }
      const done = Boolean(item.done);
      if (done) {
        this.#done = true;
        return null;
      }
      const value = item.value;
      const chunk = snapshotUint8Array(value, HTTP_BODY_CHUNK_BYTES, "HTTP body chunk");
      if (chunk.byteLength === 0) {
        emptyChunks++;
        if (emptyChunks > HTTP_BODY_EMPTY_CHUNK_LIMIT) {
          throw bodyError(
            "resource_limit",
            "http.body.pull",
            "HTTP async body produced too many empty chunks",
          );
        }
        continue;
      }
      this.#remainder = chunk;
      return this.#takeRemainder(maxBytes);
    }
  }

  async cancel(reason?: unknown): Promise<void> {
    if (this.#done || this.#returnCalled) return;
    this.#done = true;
    this.#remainder = undefined;
    const iterator = this.#iterator;
    if (!iterator || typeof iterator.return !== "function") return;
    this.#returnCalled = true;
    // A producer return failure is diagnostic-only and never replaces the
    // cancellation or transport failure which caused it.
    try {
      await iterator.return(reason as never);
    } catch {
      // Deliberately ignored at this public SDK boundary.
    }
  }

  #takeRemainder(maxBytes: number): Uint8Array {
    const remainder = this.#remainder!;
    const count = Math.min(remainder.byteLength, maxBytes);
    const result = remainder.subarray(0, count);
    this.#remainder = count === remainder.byteLength
      ? undefined
      : remainder.subarray(count);
    return result;
  }
}

class ExternalBodyStreamSource implements BodySource {
  #done = false;
  #cancelled = false;

  constructor(
    private readonly stream: BodyStream,
    private readonly readIntoMethod: BodyStream["readInto"],
    private readonly cancelMethod: BodyStream["cancel"],
  ) {}

  async pull(maxBytes: number): Promise<Uint8Array | null> {
    if (this.#done) return null;
    const destination = new Uint8Array(maxBytes);
    const result = await this.readIntoMethod.call(this.stream, destination);
    if (typeof result !== "object" || result === null) {
      throw bodyError(
        "invalid_state",
        "http.body.pull",
        "HTTP BodyStream returned an invalid readInto result",
      );
    }
    const bytes = result.bytes;
    const done = result.done;
    if (
      !Number.isSafeInteger(bytes) ||
      bytes < 0 ||
      bytes > maxBytes ||
      typeof done !== "boolean" ||
      (done && bytes !== 0) ||
      (!done && bytes === 0)
    ) {
      throw bodyError(
        "invalid_state",
        "http.body.pull",
        "HTTP BodyStream returned an invalid readInto result",
      );
    }
    if (done) {
      this.#done = true;
      return null;
    }
    return destination.subarray(0, bytes);
  }

  async cancel(reason?: unknown): Promise<void> {
    if (this.#cancelled || this.#done) return;
    this.#cancelled = true;
    this.#done = true;
    await this.cancelMethod.call(this.stream, reason);
  }
}

interface TeeSegment {
  readonly bytes: Uint8Array;
  start: number;
  end: number;
}

interface TeeBranchState {
  readonly queue: TeeSegment[];
  bufferedBytes: number;
  cancelled: boolean;
}

interface TeeState {
  readonly source: BodySource;
  readonly branches: readonly [TeeBranchState, TeeBranchState];
  readonly waiters: Set<() => void>;
  readPromise?: Promise<void>;
  done: boolean;
  hasError: boolean;
  error: unknown;
  sourceCancelCalled: boolean;
}

function newTeeBranchState(): TeeBranchState {
  return {
    queue: [],
    bufferedBytes: 0,
    cancelled: false,
  };
}

function notifyTee(state: TeeState): void {
  const waiters = [...state.waiters];
  state.waiters.clear();
  for (const wake of waiters) wake();
}

function waitForTeeChange(state: TeeState): Promise<void> {
  return new Promise((resolve) => state.waiters.add(resolve));
}

function dequeueTee(branch: TeeBranchState, maxBytes: number): Uint8Array {
  const first = branch.queue[0]!;
  const available = first.end - first.start;
  const count = Math.min(available, maxBytes);
  const result = first.bytes.subarray(first.start, first.start + count);
  first.start += count;
  branch.bufferedBytes -= count;
  if (first.start === first.end) {
    branch.queue.shift();
  }
  return result;
}

function enqueueTee(branch: TeeBranchState, chunk: Uint8Array): void {
  let offset = 0;
  while (offset < chunk.byteLength) {
    let tail = branch.queue[branch.queue.length - 1];
    if (!tail || tail.end === tail.bytes.byteLength) {
      tail = {
        bytes: new Uint8Array(HTTP_BODY_TEE_SEGMENT_BYTES),
        start: 0,
        end: 0,
      };
      branch.queue.push(tail);
    }
    const count = Math.min(tail.bytes.byteLength - tail.end, chunk.byteLength - offset);
    uint8ArraySet.call(tail.bytes, chunk.subarray(offset, offset + count), tail.end);
    tail.end += count;
    offset += count;
  }
  branch.bufferedBytes += chunk.byteLength;
}

async function fillTee(state: TeeState): Promise<void> {
  if (state.readPromise) return state.readPromise;
  const active = state.branches.filter((branch) => !branch.cancelled);
  if (active.length === 0 || state.done || state.hasError) return;
  const credit = Math.min(
    HTTP_BODY_CHUNK_BYTES,
    ...active.map((branch) => HTTP_BODY_TEE_BRANCH_BYTES - branch.bufferedBytes),
  );
  if (credit <= 0) return;

  state.readPromise = (async () => {
    try {
      const chunk = await state.source.pull(credit);
      if (chunk === null) {
        state.done = true;
      } else if (chunk.byteLength === 0 || chunk.byteLength > credit) {
        state.hasError = true;
        state.error = bodyError(
          chunk.byteLength > credit ? "resource_limit" : "invalid_state",
          "http.body.tee",
          "HTTP body source violated bounded tee credit",
        );
      } else {
        for (const branch of active) {
          if (branch.cancelled) continue;
          enqueueTee(branch, chunk);
        }
      }
    } catch (error) {
      state.hasError = true;
      state.error = error;
      if (!state.sourceCancelCalled) {
        state.sourceCancelCalled = true;
        try {
          void Promise.resolve(state.source.cancel(error)).catch(() => {});
        } catch {
          // The source error remains authoritative.
        }
      }
    } finally {
      state.readPromise = undefined;
      notifyTee(state);
    }
  })();
  return state.readPromise;
}

class TeeBodySource implements BodySource {
  constructor(
    private readonly state: TeeState,
    private readonly branchIndex: 0 | 1,
  ) {}

  async pull(maxBytes: number): Promise<Uint8Array | null> {
    const branch = this.state.branches[this.branchIndex];
    for (;;) {
      if (branch.cancelled) return null;
      if (branch.bufferedBytes > 0) {
        const chunk = dequeueTee(branch, maxBytes);
        notifyTee(this.state);
        return chunk;
      }
      if (this.state.hasError) throw this.state.error;
      if (this.state.done) return null;

      const active = this.state.branches.filter((candidate) => !candidate.cancelled);
      const canRead = active.length > 0 && active.every(
        (candidate) => candidate.bufferedBytes < HTTP_BODY_TEE_BRANCH_BYTES,
      );
      if (this.state.readPromise || canRead) {
        await fillTee(this.state);
        continue;
      }
      // The other live branch is at its fixed ceiling. Waiting here is the
      // required backpressure; consuming or cancelling that branch resumes us.
      await waitForTeeChange(this.state);
    }
  }

  async cancel(reason?: unknown): Promise<void> {
    const branch = this.state.branches[this.branchIndex];
    if (branch.cancelled) return;
    branch.cancelled = true;
    branch.queue.length = 0;
    branch.bufferedBytes = 0;
    notifyTee(this.state);
    if (
      !this.state.sourceCancelCalled &&
      this.state.branches.every((candidate) => candidate.cancelled)
    ) {
      this.state.sourceCancelCalled = true;
      await this.state.source.cancel(reason);
    }
  }
}

function teeBodySource(source: BodySource): readonly [BodySource, BodySource] {
  const state: TeeState = {
    source,
    branches: [newTeeBranchState(), newTeeBranchState()],
    waiters: new Set(),
    done: false,
    hasError: false,
    error: undefined,
    sourceCancelCalled: false,
  };
  return [new TeeBodySource(state, 0), new TeeBodySource(state, 1)];
}

class BodyAsyncIterator implements AsyncIterableIterator<Uint8Array> {
  #closed = false;

  constructor(private readonly controller: BodyController) {}

  [Symbol.asyncIterator](): AsyncIterableIterator<Uint8Array> {
    return this;
  }

  async next(): Promise<IteratorResult<Uint8Array>> {
    if (this.#closed) return { value: undefined, done: true };
    const destination = new Uint8Array(HTTP_BODY_CHUNK_BYTES);
    const result = await this.controller.readInto("iterator", destination);
    if (result.done) {
      this.#closed = true;
      return { value: undefined, done: true };
    }
    return { value: destination.slice(0, result.bytes), done: false };
  }

  async return(): Promise<IteratorResult<Uint8Array>> {
    if (!this.#closed) {
      this.#closed = true;
      await this.controller.cancel();
    }
    return { value: undefined, done: true };
  }
}

class BodyStreamValue implements BodyStream {
  constructor(private readonly controller: BodyController) {}

  readInto(destination: Uint8Array): Promise<{ bytes: number; done: boolean }> {
    return this.controller.readInto("readInto", destination);
  }

  cancel(reason?: unknown): Promise<void> {
    return this.controller.cancel(reason);
  }

  [Symbol.asyncIterator](): AsyncIterableIterator<Uint8Array> {
    return this.controller.createIterator();
  }
}

export class BodyController {
  readonly stream: BodyStream;
  readonly aggregateLimit: number;
  #source: BodySource;
  #readerKind: ReaderKind | undefined;
  #pending = false;
  #used = false;
  #terminal = false;
  #cancelled = false;
  #iteratorCreated = false;
  #helperCreated = false;
  #producerCreated = false;
  #leftover: Uint8Array | undefined;
  readonly #cloneGroup: BodyCloneGroup;

  constructor(
    source: BodySource,
    aggregateLimit = HTTP_BODY_HELPER_BYTES,
    cloneGroup: BodyCloneGroup = {
      branches: 1,
      controllers: new Set(),
      terminalCallbacks: new Set(),
    },
  ) {
    if (!Number.isSafeInteger(aggregateLimit) || aggregateLimit <= 0) {
      throw bodyError(
        "resource_limit",
        "http.body",
        "HTTP buffered body limit must be a positive safe integer",
      );
    }
    this.#source = source;
    this.#cloneGroup = cloneGroup;
    this.#cloneGroup.controllers.add(this);
    this.aggregateLimit = Math.min(aggregateLimit, HTTP_BODY_HELPER_BYTES);
    this.stream = new BodyStreamValue(this);
  }

  get bodyUsed(): boolean {
    return this.#used;
  }

  get unusable(): boolean {
    return this.#readerKind !== undefined || this.#used;
  }

  onTerminal(callback: () => void): () => void {
    if (this.#cloneGroup.controllers.size === 0) {
      callback();
      return () => {};
    }
    this.#cloneGroup.terminalCallbacks.add(callback);
    return () => this.#cloneGroup.terminalCallbacks.delete(callback);
  }

  async cancelGraph(reason?: unknown): Promise<void> {
    const controllers = [...this.#cloneGroup.controllers];
    const results = await Promise.allSettled(
      controllers.map((controller) => controller.cancel(reason)),
    );
    const rejected = results.find(
      (result): result is PromiseRejectedResult => result.status === "rejected",
    );
    if (rejected) throw rejected.reason;
  }

  createIterator(): AsyncIterableIterator<Uint8Array> {
    this.#claim("iterator");
    if (this.#iteratorCreated) {
      throw bodyError(
        "invalid_state",
        "http.body.iterator",
        "HTTP body already has an async iterator",
      );
    }
    this.#iteratorCreated = true;
    return new BodyAsyncIterator(this);
  }

  async readInto(
    kind: ReaderKind,
    destination: Uint8Array,
  ): Promise<{ bytes: number; done: boolean }> {
    let destinationSnapshot: IntrinsicViewSnapshot;
    try {
      destinationSnapshot = intrinsicUint8ArraySnapshot(
        destination,
        "HTTP body destination",
      );
      // Constructing an intrinsic view detects a detached backing buffer in
      // runtimes where the typed-array slot getters still report zero.
      new Uint8Array(
        destinationSnapshot.buffer,
        destinationSnapshot.byteOffset,
        destinationSnapshot.byteLength,
      );
    } catch {
      throw bodyError(
        "invalid_state",
        "http.body.readInto",
        "HTTP body destination uses a detached buffer",
      );
    }
    const destinationLength = destinationSnapshot.byteLength;
    if (destinationLength === 0) {
      throw bodyError(
        "invalid_state",
        "http.body.readInto",
        "HTTP body destination must not be empty",
      );
    }
    this.#claim(kind);
    this.#used = true;
    if (this.#pending) {
      throw bodyError(
        "busy",
        "http.body.readInto",
        "HTTP body already has a pending read",
      );
    }
    if (this.#terminal || this.#cancelled) return { bytes: 0, done: true };

    this.#pending = true;
    try {
      let emptyChunks = 0;
      for (;;) {
        if (this.#leftover) {
          const count = Math.min(destinationLength, this.#leftover.byteLength);
          uint8ArraySet.call(destination, this.#leftover.subarray(0, count), 0);
          this.#leftover = count === this.#leftover.byteLength
            ? undefined
            : this.#leftover.subarray(count);
          return { bytes: count, done: false };
        }
        const chunk = await this.#source.pull(
          Math.min(destinationLength, HTTP_BODY_CHUNK_BYTES),
        );
        if (this.#cancelled) return { bytes: 0, done: true };
        if (chunk === null) {
          this.#markTerminal();
          return { bytes: 0, done: true };
        }
        if (chunk.byteLength === 0) {
          emptyChunks++;
          if (emptyChunks > HTTP_BODY_EMPTY_CHUNK_LIMIT) {
            throw bodyError(
              "resource_limit",
              "http.body.readInto",
              "HTTP body source produced too many empty chunks",
            );
          }
          continue;
        }
        if (chunk.byteLength > HTTP_BODY_CHUNK_BYTES) {
          throw bodyError(
            "resource_limit",
            "http.body.readInto",
            `HTTP body chunk exceeds ${HTTP_BODY_CHUNK_BYTES} bytes`,
          );
        }
        const count = Math.min(destinationLength, chunk.byteLength);
        uint8ArraySet.call(destination, chunk.subarray(0, count), 0);
        if (count < chunk.byteLength) this.#leftover = chunk.subarray(count);
        return { bytes: count, done: false };
      }
    } catch (error) {
      this.#markTerminal();
      try {
        void Promise.resolve(this.#source.cancel(error)).catch(() => {});
      } catch {
        // The source error remains authoritative.
      }
      throw error;
    } finally {
      this.#pending = false;
    }
  }

  async cancel(reason?: unknown): Promise<void> {
    if (this.#cancelled || this.#terminal) return;
    this.#used = true;
    this.#cancelled = true;
    this.#leftover = undefined;
    try {
      await this.#source.cancel(reason);
    } finally {
      this.#markTerminal();
    }
  }

  createProducer(): HttpBodyProducer {
    if (this.#producerCreated) {
      throw bodyError(
        "invalid_state",
        "http.body.binding",
        "HTTP body already has a binding producer",
      );
    }
    this.#producerCreated = true;
    this.#claim("binding");
    return Object.freeze({
      pull: async (maxBytes: number) => {
        assertPositiveCapacity(maxBytes, "http.body.pull");
        const capacity = Math.min(maxBytes, HTTP_BODY_CHUNK_BYTES);
        const destination = new Uint8Array(capacity);
        const result = await this.readInto("binding", destination);
        return result.done ? null : destination.slice(0, result.bytes);
      },
      cancel: (reason?: unknown) => this.cancel(reason),
    });
  }

  tee(): BodyController {
    if (this.unusable) {
      throw bodyError(
        "invalid_state",
        "http.body.clone",
        "Cannot clone a locked or consumed HTTP body",
      );
    }
    if (this.#cloneGroup.branches >= HTTP_BODY_CLONE_BRANCHES) {
      throw bodyError(
        "resource_limit",
        "http.body.clone",
        `HTTP body clone graph exceeds ${HTTP_BODY_CLONE_BRANCHES} branches`,
      );
    }
    this.#cloneGroup.branches++;
    const [first, second] = teeBodySource(this.#source);
    this.#source = first;
    return new BodyController(second, this.aggregateLimit, this.#cloneGroup);
  }

  transfer(): BodyController {
    if (this.unusable) {
      throw bodyError(
        "invalid_state",
        "http.Request",
        "Cannot construct from a locked or consumed Request body",
      );
    }
    const source = this.#source;
    const transferred = new BodyController(
      source,
      this.aggregateLimit,
      this.#cloneGroup,
    );
    this.#readerKind = "binding";
    this.#used = true;
    this.#source = new MemoryBodySource(new Uint8Array());
    this.#markTerminal();
    return transferred;
  }

  async aggregate(operation: string): Promise<Uint8Array> {
    if (this.#helperCreated) {
      throw bodyError(
        "invalid_state",
        operation,
        "HTTP body was already consumed by an aggregation helper",
      );
    }
    this.#helperCreated = true;
    this.#claim("helper");
    const maximum = this.aggregateLimit;
    let output = new Uint8Array(Math.min(HTTP_BODY_CHUNK_BYTES, maximum + 1));
    let total = 0;
    for (;;) {
      if (total === output.byteLength) {
        const nextLength = Math.min(
          maximum + 1,
          Math.max(output.byteLength + 1, output.byteLength * 2),
        );
        const grown = new Uint8Array(nextLength);
        uint8ArraySet.call(grown, output);
        output = grown;
      }
      const destination = output.subarray(
        total,
        Math.min(output.byteLength, total + HTTP_BODY_CHUNK_BYTES),
      );
      const result = await this.readInto("helper", destination);
      if (result.done) break;
      total += result.bytes;
      if (total > maximum) {
        await this.cancel();
        throw bodyError(
          "resource_limit",
          operation,
          `HTTP body helper exceeds ${maximum} bytes`,
        );
      }
    }
    if (total === output.byteLength) return output;
    const exact = new Uint8Array(total);
    uint8ArraySet.call(exact, output.subarray(0, total));
    return exact;
  }

  #claim(kind: ReaderKind): void {
    if (this.#readerKind === undefined) {
      this.#readerKind = kind;
      return;
    }
    if (this.#readerKind !== kind) {
      throw bodyError(
        "invalid_state",
        `http.body.${kind}`,
        "HTTP body is locked by another reader",
      );
    }
  }

  #markTerminal(): void {
    if (this.#terminal) return;
    this.#terminal = true;
    this.#cloneGroup.controllers.delete(this);
    if (this.#cloneGroup.controllers.size !== 0) return;
    const callbacks = [...this.#cloneGroup.terminalCallbacks];
    this.#cloneGroup.terminalCallbacks.clear();
    for (const callback of callbacks) callback();
  }
}

export interface ExtractedBody {
  readonly controller: BodyController;
  readonly contentType?: string;
}

function bufferedBodySource(bytes: Uint8Array, aggregateLimit: number): BodyController {
  if (bytes.byteLength > HTTP_BUFFERED_BODY_INPUT_BYTES) {
    throw bodyError(
      "resource_limit",
      "http.body",
      `Buffered HTTP body exceeds ${HTTP_BUFFERED_BODY_INPUT_BYTES} bytes`,
    );
  }
  return new BodyController(new MemoryBodySource(bytes), aggregateLimit);
}

function assertBufferedBodySize(byteLength: number): void {
  if (byteLength > HTTP_BUFFERED_BODY_INPUT_BYTES) {
    throw bodyError(
      "resource_limit",
      "http.body",
      `Buffered HTTP body exceeds ${HTTP_BUFFERED_BODY_INPUT_BYTES} bytes`,
    );
  }
}

function assertEncodedStringSize(value: string): void {
  let bytes = 0;
  for (let index = 0; index < value.length; index++) {
    const code = value.charCodeAt(index);
    if (code <= 0x7f) bytes += 1;
    else if (code <= 0x7ff) bytes += 2;
    else if (code >= 0xd800 && code <= 0xdbff && index + 1 < value.length) {
      const low = value.charCodeAt(index + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        bytes += 4;
        index++;
      } else {
        bytes += 3;
      }
    } else {
      bytes += 3;
    }
    if (bytes > HTTP_BUFFERED_BODY_INPUT_BYTES) {
      assertBufferedBodySize(bytes);
    }
  }
}

interface BodyStreamMethods {
  readonly readInto: BodyStream["readInto"];
  readonly cancel: BodyStream["cancel"];
}

function bodyStreamMethods(value: object): BodyStreamMethods | null {
  const readInto = (value as Partial<BodyStream>).readInto;
  const cancel = (value as Partial<BodyStream>).cancel;
  const iterator = (value as Partial<BodyStream>)[Symbol.asyncIterator];
  return typeof readInto === "function" &&
      typeof cancel === "function" &&
      typeof iterator === "function"
    ? { readInto, cancel }
    : null;
}

export function extractBody(
  input: string | ArrayBuffer | ArrayBufferView | BodyStream | AsyncIterable<Uint8Array>,
  aggregateLimit = HTTP_BODY_HELPER_BYTES,
): ExtractedBody {
  if (typeof input === "string") {
    assertEncodedStringSize(input);
    return {
      controller: bufferedBodySource(textEncoderEncode.call(textEncoder, input), aggregateLimit),
      contentType: "text/plain;charset=UTF-8",
    };
  }
  let arrayBufferLength: number | undefined;
  try {
    arrayBufferLength = arrayBufferByteLength.call(input) as number;
  } catch {
    arrayBufferLength = undefined;
  }
  if (arrayBufferLength !== undefined) {
    const buffer = input as ArrayBuffer;
    const byteLength = arrayBufferLength;
    assertBufferedBodySize(byteLength);
    return {
      controller: bufferedBodySource(copyArrayBuffer(buffer), aggregateLimit),
    };
  }
  if (arrayBufferIsView.call(ArrayBuffer, input)) {
    const snapshot = intrinsicViewSnapshot(input as ArrayBufferView);
    assertBufferedBodySize(snapshot.byteLength);
    return {
      controller: bufferedBodySource(copyIntrinsicBytes(snapshot), aggregateLimit),
    };
  }
  if (typeof input === "object" && input !== null) {
    const methods = bodyStreamMethods(input);
    if (methods) {
      return {
        controller: new BodyController(
          new ExternalBodyStreamSource(
            input as BodyStream,
            methods.readInto,
            methods.cancel,
          ),
          aggregateLimit,
        ),
      };
    }
    const iteratorFactory = (input as Partial<AsyncIterable<Uint8Array>>)[Symbol.asyncIterator];
    if (typeof iteratorFactory !== "function") {
      throw new TypeError(
        "HTTP body must be a string, ArrayBuffer, ArrayBufferView, BodyStream, or AsyncIterable",
      );
    }
    return {
      controller: new BodyController(
        new AsyncIterableBodySource(
          input as AsyncIterable<Uint8Array>,
          iteratorFactory as () => AsyncIterator<Uint8Array>,
        ),
        aggregateLimit,
      ),
    };
  }
  throw new TypeError(
    "HTTP body must be a string, ArrayBuffer, ArrayBufferView, BodyStream, or AsyncIterable",
  );
}

export function bodyFromBinding(
  input: BodyStream,
  aggregateLimit?: number,
): BodyController {
  if (typeof input !== "object" || input === null) {
    throw new TypeError("Private HTTP binding response body must be a BodyStream");
  }
  const methods = bodyStreamMethods(input);
  if (!methods) throw new TypeError("Private HTTP binding response body must be a BodyStream");
  return new BodyController(
    new ExternalBodyStreamSource(input, methods.readInto, methods.cancel),
    aggregateLimit,
  );
}
