// Headless runtime entry: the frame transaction without a UI root.
//
// A host without a display (or a display it does not drive from PocketJS)
// still ticks the guest once per host tick through `globalThis.frame(...)`.
// `mountHeadless()` installs a frame handler that runs the same fixed
// prefix of the frame transaction the UI entries run — virtual clock →
// service pumps (network delivery) → effect delivery → app hook — and
// nothing else: no renderer, no input edge detection, no `globalThis.ui`
// requirement. Promise reactions raised inside the pumps run in the host's
// job drain after `frame()` returns, exactly as under `render()`.
//
// This is what the network smoke firmware and headless daemons use; a UI
// app keeps using `render()`/`mount()` from the framework entry.

import { __advanceClock, resetClock } from "./clock.ts";
import { __drainEffects, resetEffects } from "./effects.ts";
import { installFrameHandler } from "./host.ts";
import { runServicePumps } from "./services.ts";

export interface HeadlessOptions {
  /** Called every frame after service pumps and effect delivery. */
  frame?: (buttons: number, analog: number) => void;
}

/** Install the headless frame handler. Returns a disposer that uninstalls it. */
export function mountHeadless(options: HeadlessOptions = {}): () => void {
  resetClock(); // latches the host's __simHz clock policy (docs/DETERMINISM.md)
  resetEffects();
  const hook = options.frame;
  installFrameHandler((buttons: number, analog?: number) => {
    __advanceClock(); // virtual frame++, fire due after() timers
    runServicePumps(); // only modules with pending async work register here
    __drainEffects(); // frame-boundary deliveries enter the world first
    if (hook) hook(buttons, analog ?? 0);
  });
  return () => {
    (globalThis as { frame?: unknown }).frame = undefined;
  };
}
