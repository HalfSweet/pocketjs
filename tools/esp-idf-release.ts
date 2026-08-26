#!/usr/bin/env bun

import { cpSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { basename, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(fileURLToPath(new URL("..", import.meta.url)));
const COMPONENTS = join(ROOT, "hosts/esp-idf/components");
const names = [
  "pocketjs_package",
  "pocketjs_guest",
  "pocketjs_ui_core",
  "pocketjs_ui_qjs",
  "pocketjs_render_rgb565",
  "pocketjs_esp32p4_ppa",
  "pocketjs_runner",
] as const;

const args = Bun.argv.slice(2);
let output = join(ROOT, "dist/esp-idf-components");
while (args.length) {
  const option = args.shift();
  if (option === "--output") output = resolve(args.shift() ?? "");
  else throw new Error(`unknown option ${option}`);
}

const ignored = (source: string): boolean => {
  const name = basename(source);
  return name === "target" || name.startsWith("target-") || name === "__pycache__" || name === ".DS_Store";
};

if (existsSync(output)) throw new Error(`output already exists: ${output}`);
mkdirSync(output, { recursive: true });
for (const name of names) {
  cpSync(join(COMPONENTS, name), join(output, name), {
    recursive: true,
    filter: (source) => !ignored(source),
  });
  cpSync(join(ROOT, "LICENSE"), join(output, name, "LICENSE"));
}

const ui = join(output, "pocketjs_ui_core");
for (const target of ["esp32p4", "esp32s3"]) {
  for (const file of ["libpocketjs_idf_native.a", "build-receipt.json"]) {
    if (!existsSync(join(ui, "lib", target, file))) {
      throw new Error(`missing ${target}/${file}; run tools/esp-idf-native.ts for both targets first`);
    }
  }
}
mkdirSync(join(ui, "vendor"), { recursive: true });
cpSync(join(ROOT, "hosts/esp-idf/native"), join(ui, "vendor/native"), {
  recursive: true,
  filter: (source) => !ignored(source),
});
cpSync(join(ROOT, "engine/core"), join(ui, "vendor/core"), {
  recursive: true,
  filter: (source) => !ignored(source),
});
cpSync(join(ROOT, "engine/backends/esp32p4-ppa"), join(ui, "vendor/esp32p4-ppa"), {
  recursive: true,
  filter: (source) => !ignored(source),
});
const manifestPath = join(ui, "vendor/native/Cargo.toml");
const manifest = readFileSync(manifestPath, "utf8")
  .replace('path = "../../../engine/core"', 'path = "../core"')
  .replace('path = "../../../engine/backends/esp32p4-ppa"', 'path = "../esp32p4-ppa"');
writeFileSync(manifestPath, manifest);
const rendererManifestPath = join(ui, "vendor/esp32p4-ppa/Cargo.toml");
const rendererManifest = readFileSync(rendererManifestPath, "utf8")
  .replaceAll('path = "../../core"', 'path = "../core"');
writeFileSync(rendererManifestPath, rendererManifest);

for (const name of names) {
  const root = join(output, name);
  const forbidden: string[] = [];
  for await (const path of new Bun.Glob("**/{target,target-*,build,managed_components,__pycache__}/**").scan({
    cwd: root,
    absolute: false,
    onlyFiles: false,
  })) forbidden.push(path);
  if (forbidden.length) throw new Error(`${name} contains generated paths: ${forbidden.join(", ")}`);
}
console.log(`ESP-IDF component staging ready: ${output}`);
