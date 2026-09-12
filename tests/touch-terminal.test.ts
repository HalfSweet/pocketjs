import { beforeEach, describe, expect, test } from "bun:test";
import { attachGesture, __runGestures, resetGestures } from "../framework/src/gesture.ts";
import { __packTouch, __packTouchCancellation, __packTouchEnd, __setTouches, __resetTouches, createTouchHitFacts, touches } from "../framework/src/touch.ts";
import { resetClock, __advanceClock } from "../framework/src/clock.ts";
function frame(words?: number[]) { __advanceClock(); __setTouches(words); __runGestures(); }
beforeEach(() => { resetClock(); resetGestures(); __resetTouches(); });
describe("host terminal touch facts", () => {
  test("cancel ends a right swipe without its commit handler", () => {
    let commits = 0, cancelled = 0, taps = 0;
    attachGesture({ axis: "x", onPanEnd: () => commits++, onPanMove: () => {}, onCancel: () => cancelled++, onTap: () => taps++ });
    frame([__packTouch(0, 10, 50)]); frame([__packTouch(0, 130, 50)]);
    frame([__packTouchCancellation(0, 130, 50)]); frame();
    expect({ commits, cancelled, taps }).toEqual({ commits: 0, cancelled: 1, taps: 0 });
    expect(touches()).toHaveLength(0);
  });
  test("unseen short cancellation never becomes a tap", () => {
    let taps = 0; attachGesture({ onTap: () => taps++ }); frame([__packTouchCancellation(0)]); frame(); expect(taps).toBe(0);
  });
  test("release carries the final retreat geometry to pan handlers", () => {
    const deltas: number[] = []; let final = -1, taps = 0;
    attachGesture({ axis: "x", onPanMove: c => deltas.push(c.dx), onPanEnd: c => { final = c.dx; }, onTap: () => taps++ });
    frame([__packTouch(0, 10, 40)]); frame([__packTouch(0, 140, 40)]);
    frame([__packTouchEnd(0, 35, 40)]); frame();
    expect(final).toBe(25); expect(deltas.at(-1)).toBe(25); expect(taps).toBe(0);
  });
  test("release beyond tap slop does not activate a button", () => {
    let taps = 0; attachGesture({ onTap: () => taps++ });
    frame([__packTouch(0, 10, 40)]); frame([__packTouchEnd(0, 130, 40)]); frame(); expect(taps).toBe(0);
  });
  test("release in place single-fires, and contact id reuse resolves a fresh hit", () => {
    let taps = 0, queries = 0; const hits = createTouchHitFacts(x => { queries++; return x; });
    attachGesture({ onTap: () => taps++ });
    frame([__packTouch(0,10,40)]); frame([__packTouchEnd(0,10,40)]); frame(); expect(taps).toBe(1);
    expect(hits([__packTouch(0,10,40)])).toEqual([10]);
    expect(hits([__packTouchEnd(0,10,40)])).toEqual([10]);
    expect(hits([__packTouch(0,200,40)])).toEqual([200]); expect(queries).toBe(2);
  });
});
