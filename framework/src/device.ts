/** Device effects. Native work is polled at frame boundaries, never from a
 * camera queue into the JS runtime. Hosts/tests inject the same namespace. */
import { registerServicePump } from "./services.ts";

export type QrResult =
  | { status: "decoded"; text: string }
  | { status: "cancelled" }
  | { status: "error"; code: string };
export type ClipboardResult = { ok: true; text?: string } | { ok: false; code: string };
export interface DeviceOps {
  scanStart(): number;
  scanPoll(): string | undefined;
  scanCancel(): void;
  clipboardRead(): string;
  clipboardWrite(text: string): string;
  /** Monotonically increasing transition sequence, including an inactive
   * interval entirely between guest frames. Even = active; odd = inactive. */
  activity(): number;
}

function host(): DeviceOps {
  const ns = (globalThis as { device?: DeviceOps }).device;
  if (!ns) throw new Error("Device services are unavailable on this host");
  return ns;
}

/** Only one scan may be pending. Cancel is idempotent and delivers cancelled
 * on the next frame, just like a native Cancel button. */
export function scanQr(onResult: (result: QrResult) => void): () => void {
  const ns = host();
  if (ns.scanStart() !== 0) {
    let cancelled = false;
    const off = registerServicePump(() => {
      off();
      onResult(cancelled ? { status: "cancelled" } : { status: "error", code: "busy" });
    });
    return () => { cancelled = true; };
  }
  let finished = false;
  const off = registerServicePump(() => {
    const line = ns.scanPoll();
    if (!line) return;
    finished = true;
    off();
    onResult(JSON.parse(line) as QrResult);
  });
  return () => { if (!finished) ns.scanCancel(); };
}

export const clipboard = {
  readText(): ClipboardResult { return JSON.parse(host().clipboardRead()); },
  writeText(text: string): ClipboardResult { return JSON.parse(host().clipboardWrite(text)); },
};

/** If iOS suspended the frame pump during the whole inactive interval, report
 * inactive first so callers still discard drafts before rendering again. */
export function onAppActivity(listener: (active: boolean) => void): () => void {
  const ns = host();
  let sequence = ns.activity();
  return registerServicePump(() => {
    const next = ns.activity();
    if (next === sequence) return;
    if (next - sequence > 1 && (sequence & 1) === 0) listener(false);
    sequence = next;
    listener((sequence & 1) === 0);
  });
}
