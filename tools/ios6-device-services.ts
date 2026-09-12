import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import pins from "./cli/ios6-device-services.json";

/** These images come from the already pinned iOS 6.1.3 shared cache. Only
 * locally generated link-time stubs are used; no Apple binary is shipped. */
export function prepareIOS6DeviceServiceStubs(sysroot: string): string[] {
  const stubs: string[] = [];
  for (const [name, expected] of Object.entries(pins.frameworks)) {
    const relative = `System/Library/Frameworks/${name}.framework/${name}`;
    const binary = join(sysroot, relative);
    if (createHash("sha256").update(readFileSync(binary)).digest("hex") !== expected) {
      throw new Error(`iOS 6: unexpected ${name} image in the validated sysroot`);
    }
    const result = Bun.spawnSync(["xcrun", "nm-classic", "-gU", binary]);
    if (result.exitCode !== 0) throw new Error(`iOS 6: cannot read ${name} exports`);
    const symbols = [...new Set(result.stdout.toString().split("\n").flatMap(line => {
      const match = line.match(/\s[ASTDBIWV]\s(\S+)$/);
      return match ? [match[1]] : [];
    }))].sort();
    const path = binary + ".tbd";
    writeFileSync(path, ["--- !tapi-tbd", "tbd-version: 4", "targets: [ armv7-ios ]",
      `install-name: ${JSON.stringify("/" + relative)}`, "exports:", "  - targets: [ armv7-ios ]",
      `    symbols: [ ${symbols.map(s => JSON.stringify(s)).join(", ")} ]`, "...", ""].join("\n"));
    stubs.push(path);
  }
  return stubs;
}

export function verifyQuircSources(directory: string): void {
  const source = JSON.parse(readFileSync(join(directory, "SOURCE.json"), "utf8")) as { sha256: Record<string,string> };
  for (const [file, expected] of Object.entries(source.sha256)) {
    if (!/^[A-Za-z0-9_.-]+$/.test(file) || createHash("sha256").update(readFileSync(join(directory,file))).digest("hex") !== expected) {
      throw new Error("The pinned quirc source verification failed");
    }
  }
}
