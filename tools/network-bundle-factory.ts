import {
  verifyPlanHash,
  type ResolvedBuildPlan,
} from "../framework/src/manifest/plan.ts";

/** Private framework identifier replaced by Bun only for network factories. */
export const NETWORK_BINDING_DEFINE = "__POCKET_NETWORK_BINDING_V1__";

/** Lexical name introduced by the post-bundle factory wrapper. */
export const NETWORK_BINDING_FACTORY_PARAMETER =
  "__pocketNetworkBindingV1FactoryParameter";

export type BundleArtifactMode = "iife" | "network-factory";

/**
 * Select the artifact ABI after checking the plan checksum. Network authority
 * and an explicitly selected factory-aware loader must agree in both
 * directions; legacy loaders must never receive a callable artifact by
 * accident.
 */
export function selectBundleArtifactMode(
  plan: ResolvedBuildPlan | undefined,
  networkFactoryRequested: boolean,
): BundleArtifactMode {
  if (plan !== undefined && !verifyPlanHash(plan)) {
    throw new TypeError("PocketJS network factory: invalid ResolvedBuildPlan checksum");
  }

  const hasNetworkPlan = plan?.network !== undefined;
  if (hasNetworkPlan && !networkFactoryRequested) {
    throw new TypeError(
      "PocketJS build: a network ResolvedBuildPlan requires a factory-aware loader (--network-factory)",
    );
  }
  if (!hasNetworkPlan && networkFactoryRequested) {
    throw new TypeError(
      "PocketJS build: --network-factory requires a ResolvedBuildPlan with network admission",
    );
  }
  return hasNetworkPlan ? "network-factory" : "iife";
}

/** Bun define entries required by the private lexical binding. */
export function networkFactoryDefines(
  mode: BundleArtifactMode,
): Readonly<Record<string, string>> {
  return mode === "network-factory"
    ? { [NETWORK_BINDING_DEFINE]: NETWORK_BINDING_FACTORY_PARAMETER }
    : {};
}

/**
 * Wrap Bun's IIFE without evaluating it. The returned factory is deliberately
 * one-shot: its first call consumes the artifact before validation or app
 * initialization, so an exception can never be retried against altered Host
 * state. Re-evaluation is required to obtain a fresh factory.
 */
export function wrapNetworkBundleFactory(bundle: string): string {
  if (typeof bundle !== "string" || bundle.length === 0) {
    throw new TypeError("PocketJS network factory: bundle source must not be empty");
  }

  const body = bundle
    .split("\n")
    .map((line) => `    ${line}`)
    .join("\n");

  return `(function () {
  "use strict";
  let __pocketNetworkFactoryConsumedV1 = false;
  return function pocketNetworkBundleFactory(${NETWORK_BINDING_FACTORY_PARAMETER}) {
    if (__pocketNetworkFactoryConsumedV1) {
      throw new TypeError("PocketJS network bundle factory was already invoked");
    }
    __pocketNetworkFactoryConsumedV1 = true;
    if (arguments.length !== 1) {
      throw new TypeError("PocketJS network bundle factory requires exactly one binding argument");
    }
    if (
      ${NETWORK_BINDING_FACTORY_PARAMETER} === null ||
      typeof ${NETWORK_BINDING_FACTORY_PARAMETER} !== "object" ||
      !Object.isFrozen(${NETWORK_BINDING_FACTORY_PARAMETER})
    ) {
      throw new TypeError("PocketJS network bundle factory requires a frozen binding table");
    }
${body}
    return undefined;
  };
})()`;
}

/** Preserve legacy artifacts byte-for-byte; wrap only the admitted mode. */
export function finalizeBundleArtifact(
  bundle: string,
  mode: BundleArtifactMode,
): string {
  return mode === "network-factory" ? wrapNetworkBundleFactory(bundle) : bundle;
}
