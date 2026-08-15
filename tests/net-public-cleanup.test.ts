import { describe, expect, test } from "bun:test";

const webEngine = await Bun.file(
  new URL("../hosts/web/engine.js", import.meta.url),
).text();
const simRuntime = await Bun.file(
  new URL("../hosts/sim/sim.ts", import.meta.url),
).text();

describe("NET v1 public cleanup", () => {
  test("the stock web host removes rather than publishes globalThis.net", () => {
    expect(webEngine).not.toContain('from "./net.js"');
    expect(webEngine).not.toMatch(/globalThis\.net\s*=/);
    expect(webEngine).toContain('Reflect.deleteProperty(globalThis, "net")');
  });

  test("the stock sim removes a stale net global before application eval", () => {
    expect(simRuntime).not.toMatch(/g\.net\s*=/);
    expect(simRuntime).toContain('Reflect.deleteProperty(g, "net")');
    expect(simRuntime.indexOf('Reflect.deleteProperty(g, "net")')).toBeLessThan(
      simRuntime.indexOf("(0, eval)(src)"),
    );
  });
});
