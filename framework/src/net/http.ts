import type {
  AbortSignal,
  NetworkData,
  NetworkLimitOverrides,
  TlsOptions,
  URL,
} from "./index.ts";
import {
  unsupportedNetworkOperation,
  unsupportedNetworkPromise,
} from "./internal.ts";
import type { WebSocketUpgrade } from "./websocket.ts";

export {
  AbortController,
  AbortSignal,
  NetworkError,
  URL,
} from "./index.ts";
export type {
  NetworkAddress,
  NetworkData,
  NetworkErrorCategory,
  NetworkErrorCode,
  NetworkErrorOptions,
  NetworkLimit,
  NetworkLimitOverrides,
  NetworkLimits,
  NetworkProtocol,
  NetworkRole,
  TlsOptions,
} from "./index.ts";

export type HeadersInit =
  | Headers
  | Record<string, string>
  | Iterable<readonly [string, string]>;

export interface BodyStream extends AsyncIterable<Uint8Array> {
  readInto(destination: Uint8Array): Promise<{ bytes: number; done: boolean }>;
  cancel(reason?: unknown): Promise<void>;
}

export type BodyInit = NetworkData | BodyStream | AsyncIterable<Uint8Array> | null;
export type RequestRedirect = "follow" | "manual" | "error";

export interface HttpTimeouts {
  readonly connect?: number;
  readonly headers?: number;
  readonly idle?: number;
  readonly total?: number;
}

export interface RequestInit {
  readonly method?: string;
  readonly headers?: HeadersInit;
  readonly body?: BodyInit;
  readonly signal?: AbortSignal;
  readonly redirect?: RequestRedirect;
  readonly timeouts?: HttpTimeouts;
  readonly maxRedirects?: number;
  readonly tls?: TlsOptions;
  readonly limits?: NetworkLimitOverrides;
  readonly ref?: boolean;
}

export interface ResponseInit {
  readonly status?: number;
  readonly statusText?: string;
  readonly headers?: HeadersInit;
}

/**
 * Fetch objects are declared now so applications can type-check against the
 * target catalog. Their conformance implementation lands with HTTP admission.
 */
export class Headers implements Iterable<[string, string]> {
  constructor(_init?: HeadersInit) {
    throw unsupportedNetworkOperation("http.Headers", "http");
  }

  append(_name: string, _value: string): void {
    throw unsupportedNetworkOperation("http.Headers.append", "http");
  }

  delete(_name: string): void {
    throw unsupportedNetworkOperation("http.Headers.delete", "http");
  }

  get(_name: string): string | null {
    throw unsupportedNetworkOperation("http.Headers.get", "http");
  }

  has(_name: string): boolean {
    throw unsupportedNetworkOperation("http.Headers.has", "http");
  }

  set(_name: string, _value: string): void {
    throw unsupportedNetworkOperation("http.Headers.set", "http");
  }

  entries(): IterableIterator<[string, string]> {
    throw unsupportedNetworkOperation("http.Headers.entries", "http");
  }

  keys(): IterableIterator<string> {
    throw unsupportedNetworkOperation("http.Headers.keys", "http");
  }

  values(): IterableIterator<string> {
    throw unsupportedNetworkOperation("http.Headers.values", "http");
  }

  forEach(
    _callback: (value: string, key: string, headers: Headers) => void,
    _thisArg?: unknown,
  ): void {
    throw unsupportedNetworkOperation("http.Headers.forEach", "http");
  }

  getSetCookie(): string[] {
    throw unsupportedNetworkOperation("http.Headers.getSetCookie", "http");
  }

  [Symbol.iterator](): IterableIterator<[string, string]> {
    return this.entries();
  }
}

export class Request {
  declare readonly method: string;
  declare readonly url: string;
  declare readonly headers: Headers;
  declare readonly body: BodyStream | null;
  declare readonly bodyUsed: boolean;
  declare readonly signal: AbortSignal;
  declare readonly redirect: RequestRedirect;

  constructor(_input: string | URL | Request, _init?: RequestInit) {
    throw unsupportedNetworkOperation("http.Request", "http");
  }

  clone(): Request {
    throw unsupportedNetworkOperation("http.Request.clone", "http");
  }

  arrayBuffer(): Promise<ArrayBuffer> {
    return unsupportedNetworkPromise("http.Request.arrayBuffer", "http");
  }

  text(): Promise<string> {
    return unsupportedNetworkPromise("http.Request.text", "http");
  }

  json(): Promise<unknown> {
    return unsupportedNetworkPromise("http.Request.json", "http");
  }
}

export class Response {
  declare readonly status: number;
  declare readonly statusText: string;
  declare readonly ok: boolean;
  declare readonly headers: Headers;
  declare readonly body: BodyStream | null;
  declare readonly bodyUsed: boolean;
  declare readonly url: string;
  declare readonly redirected: boolean;

  constructor(_body?: BodyInit, _init?: ResponseInit) {
    throw unsupportedNetworkOperation("http.Response", "http");
  }

  clone(): Response {
    throw unsupportedNetworkOperation("http.Response.clone", "http");
  }

  arrayBuffer(): Promise<ArrayBuffer> {
    return unsupportedNetworkPromise("http.Response.arrayBuffer", "http");
  }

  text(): Promise<string> {
    return unsupportedNetworkPromise("http.Response.text", "http");
  }

  json(): Promise<unknown> {
    return unsupportedNetworkPromise("http.Response.json", "http");
  }

  static json(_data: unknown, _init?: ResponseInit): Response {
    throw unsupportedNetworkOperation("http.Response.json", "http");
  }

  static redirect(_url: string | URL, _status?: number): Response {
    throw unsupportedNetworkOperation("http.Response.redirect", "http");
  }
}

export interface HttpServerStopOptions {
  readonly graceful?: boolean;
  readonly timeout?: number;
}

export interface HttpServer {
  readonly address: import("./index.ts").NetworkAddress;
  stop(options?: HttpServerStopOptions): Promise<void>;
  ref(): this;
  unref(): this;
}

export type HttpFetchResult = Response | WebSocketUpgrade;

export interface HttpServeOptions {
  readonly hostname?: string;
  readonly port: number;
  readonly tls?: TlsOptions;
  readonly limits?: NetworkLimitOverrides;
  readonly ref?: boolean;
  readonly fetch: (
    request: Request,
    server: HttpServer,
  ) => HttpFetchResult | Promise<HttpFetchResult>;
  readonly error?: (error: unknown) => Response | Promise<Response>;
}

export function fetch(
  _input: string | URL | Request,
  _init?: RequestInit,
): Promise<Response> {
  return unsupportedNetworkPromise("http.fetch", "http");
}

export function serve(_options: HttpServeOptions): Promise<HttpServer> {
  return unsupportedNetworkPromise("http.serve", "http");
}
