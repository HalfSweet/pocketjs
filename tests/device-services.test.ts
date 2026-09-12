import { describe, expect, test } from "bun:test";
import { scanQr, clipboard, onAppActivity, type DeviceOps, type QrResult } from "../framework/src/device.ts";
import { runServicePumps } from "../framework/src/services.ts";
function fixture() {
  let pending = false, result: QrResult | null = null, activity = 0;
  const ns: DeviceOps = {
    scanStart() { if (pending) return 1; pending = true; return 0; },
    scanPoll() { if (!result) return undefined; const out = JSON.stringify(result); result = null; pending = false; return out; },
    scanCancel() { if (pending) result = { status: "cancelled" }; },
    clipboardRead: () => '{"ok":false,"code":"unavailable"}',
    clipboardWrite: () => '{"ok":false,"code":"unavailable"}',
    activity: () => activity,
  };
  (globalThis as { device?: DeviceOps }).device = ns;
  return { result: (next: QrResult) => { result = next; }, activity: (next: number) => { activity = next; } };
}
describe("frame-boundary device effects", () => {
  test("scan result is single-delivery and waits until the frame pump", () => {
    const host = fixture(), delivered: QrResult[] = [];
    const cancel = scanQr(value => delivered.push(value));
    host.result({ status: "decoded", text: "test fixture" }); expect(delivered).toHaveLength(0);
    runServicePumps(); runServicePumps(); cancel(); runServicePumps();
    expect(delivered).toEqual([{ status: "decoded", text: "test fixture" }]);
  });
  test("caller cancellation and busy scans cannot generate a decoded result", () => {
    fixture(); const delivered: QrResult[] = [];
    const cancel = scanQr(value => delivered.push(value)); scanQr(value => delivered.push(value)); cancel(); runServicePumps();
    expect(delivered).toEqual([{ status: "cancelled" }, { status: "error", code: "busy" }]);
  });
  test("clipboard failures are observable", () => {
    fixture(); expect(clipboard.readText().ok).toBe(false); expect(clipboard.writeText("123456").ok).toBe(false);
  });
  test("cancelling a busy request still gives one terminal result", () => {
    fixture(); const delivered: QrResult[] = [];
    const first = scanQr(value => delivered.push(value));
    const second = scanQr(value => delivered.push(value));
    second(); first(); runServicePumps(); runServicePumps();
    expect(delivered).toEqual([{ status: "cancelled" }, { status: "cancelled" }]);
  });
  test("suspended interval is delivered even without an inactive frame", () => {
    const host = fixture(), events: boolean[] = []; const off = onAppActivity(active => events.push(active));
    host.activity(2); runServicePumps(); expect(events).toEqual([false, true]);
    off(); host.activity(3); runServicePumps(); expect(events).toEqual([false, true]);
  });
});
