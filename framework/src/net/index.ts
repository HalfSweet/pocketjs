import { canonicalizeHttpUrl } from "./http-url.ts";

const freezeNetworkValue = Object.freeze;
const networkString = String;

/** Shared support values and types for the PocketJS network package namespace.
 *
 * Importing this module does not mount a transport or read a public global.
 * Protocol capabilities live in the sibling protocol modules.
 */

export type NetworkData = string | ArrayBuffer | ArrayBufferView;

export type NetworkProtocol = "http" | "websocket" | "mqtt" | "tcp" | "udp";
export type NetworkRole = "client" | "server";

export interface NetworkAddress {
  readonly family: "ipv4" | "ipv6";
  readonly address: string;
  readonly port: number;
}

export interface TlsOptions {
  readonly serverName?: string;
  readonly minVersion?: "1.2" | "1.3";
  readonly maxVersion?: "1.2" | "1.3";
  readonly alpn?: readonly string[];
  readonly ca?: Uint8Array;
  readonly credential?: string;
  readonly clientCertificate?: "none" | "optional" | "required";
  readonly verification?: "full" | "development-insecure";
  readonly revocation?: "host-default" | "required";
}

/** Per-operation limits can only lower capacities admitted by the build plan. */
export type NetworkLimitOverrides = Readonly<Record<string, number>>;

export interface NetworkLimit {
  readonly default: number;
  readonly hard: number;
  readonly minimum: number;
}

/** A frozen snapshot of capacities admitted for a protocol and role. */
export interface NetworkLimits {
  readonly protocol?: NetworkProtocol;
  readonly role?: NetworkRole;
  readonly values: Readonly<Record<string, Readonly<NetworkLimit>>>;
  readonly features: Readonly<Record<string, boolean>>;
}

export type NetworkErrorCategory =
  | "runtime"
  | "resolver"
  | "transport"
  | "tls"
  | "protocol";

export type NetworkErrorCode =
  | "aborted"
  | "timed_out"
  | "closed"
  | "invalid_state"
  | "busy"
  | "resource_limit"
  | "unsupported"
  | "permission_denied"
  | "dns_not_found"
  | "dns_temporary_failure"
  | "dns_refused"
  | "connection_refused"
  | "connection_reset"
  | "network_unreachable"
  | "address_in_use"
  | "broken_pipe"
  | "tls_certificate_invalid"
  | "tls_hostname_mismatch"
  | "tls_handshake_failed"
  | "tls_version_unsupported"
  | "tls_alert"
  | "http_protocol_error"
  | "websocket_protocol_error"
  | "mqtt_unacceptable_protocol_version"
  | "mqtt_identifier_rejected"
  | "mqtt_server_unavailable"
  | "mqtt_bad_credentials"
  | "mqtt_not_authorized"
  | "mqtt_protocol_error"
  | "message_too_large"
  | "system_error";

export interface NetworkErrorOptions {
  readonly category: NetworkErrorCategory;
  readonly code: NetworkErrorCode;
  readonly operation: string;
  readonly temporary?: boolean;
  readonly address?: string;
  readonly port?: number;
  readonly protocol?: NetworkProtocol;
  readonly causeCode?: string;
  readonly reasonCode?: number;
}

/** Stable public error type shared by every network protocol module. */
export class NetworkError extends Error {
  readonly category: NetworkErrorCategory;
  readonly code: NetworkErrorCode;
  readonly operation: string;
  readonly temporary: boolean;
  readonly address?: string;
  readonly port?: number;
  readonly protocol?: NetworkProtocol;
  readonly causeCode?: string;
  readonly reasonCode?: number;

  constructor(message: string, options: NetworkErrorOptions) {
    super(message);
    this.name = "NetworkError";
    this.category = options.category;
    this.code = options.code;
    this.operation = options.operation;
    this.temporary = options.temporary ?? false;
    this.address = options.address;
    this.port = options.port;
    this.protocol = options.protocol;
    this.causeCode = options.causeCode;
    this.reasonCode = options.reasonCode;
  }
}

/**
 * A canonical absolute HTTP(S) URL value. Relative and non-HTTP URL schemes
 * are outside the first public network profile.
 */
export class URL {
  readonly href: string;

  constructor(input: string | URL) {
    const source = input instanceof URL ? input.href : networkString(input);
    const parsed = canonicalizeHttpUrl(source);
    this.href = `${parsed.href}${parsed.fragment}`;
    freezeNetworkValue(this);
  }

  toString(): string {
    return this.href;
  }

  toJSON(): string {
    return this.href;
  }
}

export {
  AbortController,
  AbortSignal,
  type AbortEvent,
  type AbortListener,
} from "./abort.ts";

/**
 * Returns the build-admitted limits once a Network Guest Binding is present.
 * The public fallback fails explicitly and never probes `globalThis.net`.
 */
export function getNetworkLimits(
  _protocol?: NetworkProtocol,
  _role?: NetworkRole,
): NetworkLimits {
  throw new NetworkError("Network limits are unavailable without an admitted network binding", {
    category: "runtime",
    code: "unsupported",
    operation: "getNetworkLimits",
  });
}
