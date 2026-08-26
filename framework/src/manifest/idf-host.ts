import { createHash } from "node:crypto";
import {
  POCKET_IDF_HOST_ABI,
  pocketIdfHostSchema,
  type PocketIdfHostProfile,
} from "../../../contracts/spec/idf-host.ts";
import {
  POCKET_CAPABILITIES,
  definePlatformContractRegistry,
  defineTargetRegistry,
  type PlatformContractRegistry,
} from "../../../contracts/spec/platforms.ts";
import { canonicalJson } from "./plan.ts";
import { validateSchema, type ContractDiagnostic, type ValidationResult } from "./validate.ts";

export function validatePocketIdfHostProfile(input: unknown): ValidationResult<PocketIdfHostProfile> {
  const diagnostics: ContractDiagnostic[] = [];
  validateSchema(input, pocketIdfHostSchema, "", diagnostics);
  if (diagnostics.length === 0) {
    const profile = input as PocketIdfHostProfile;
    const encoded = new TextEncoder().encode(profile.id);
    if (encoded.length >= 16) {
      diagnostics.push({
        code: "idfHost.targetTooLong",
        path: "/id",
        message: "target id must occupy at most 15 UTF-8 bytes",
      });
    }
    if (profile.capabilities.includes("input.touch")) {
      profile.display.logicalViewports.forEach((viewport, index) => {
        if (viewport[0] > 512 || viewport[1] > 512) {
          diagnostics.push({
            code: "idfHost.touchViewportTooLarge",
            path: `/display/logicalViewports/${index}`,
            message: "touch-capable logical viewports must fit the 9-bit coordinate contract",
          });
        }
      });
    }
  }
  if (diagnostics.length > 0) return { ok: false, diagnostics };
  return { ok: true, value: input as PocketIdfHostProfile };
}

export function hashPocketIdfHostProfile(profile: PocketIdfHostProfile): string {
  return `sha256:${createHash("sha256").update(canonicalJson(profile)).digest("hex")}`;
}

export function pocketIdfHostRegistry(profile: PocketIdfHostProfile): PlatformContractRegistry {
  return definePlatformContractRegistry(
    POCKET_CAPABILITIES,
    defineTargetRegistry({
      [profile.id]: {
        hostAbi: POCKET_IDF_HOST_ABI,
        platform: profile.platform,
        form: profile.form,
        display: profile.display,
        capabilities: profile.capabilities,
      },
    }),
  );
}
