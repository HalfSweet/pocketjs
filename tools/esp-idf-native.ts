#!/usr/bin/env bun

import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(fileURLToPath(new URL("..", import.meta.url)));
const NATIVE = join(ROOT, "hosts/esp-idf/native");
const COMPONENT_LIB = join(ROOT, "hosts/esp-idf/components/pocketjs_ui_core/lib");

const args = Bun.argv.slice(2);
const take = (name: string): string | undefined => {
  const index = args.indexOf(`--${name}`);
  if (index < 0) return undefined;
  const value = args[index + 1];
  if (!value) throw new Error(`--${name} requires a value`);
  args.splice(index, 2);
  return value;
};

const target = take("target");
const cargoInput = take("cargo") ?? Bun.which("cargo");
const archiverInput = take("archiver");
const archiver = archiverInput ?? process.env.AR ?? [
  Bun.which("llvm-ar"),
  Bun.which("gcc-ar"),
  Bun.which("riscv32-esp-elf-ar"),
  "/opt/homebrew/opt/llvm/bin/llvm-ar",
  "/usr/local/opt/llvm/bin/llvm-ar",
  Bun.which("ar"),
].find((candidate) => candidate && (candidate === "ar" || existsSync(candidate)));
const cargo = cargoInput ? resolve(cargoInput) : "";
const outputRoot = resolve(take("output-root") ?? COMPONENT_LIB);
if (!target || !["esp32p4", "esp32s3"].includes(target)) {
  throw new Error(
    "usage: bun tools/esp-idf-native.ts --target <esp32p4|esp32s3> " +
      "[--cargo path] [--archiver path] [--output-root dir]",
  );
}
if (!cargo || !existsSync(cargo)) throw new Error("cargo not found; PocketJS does not install Rust");
if (target === "esp32p4" && !archiver) {
  throw new Error("ar not found; pass the target archiver with --archiver");
}
if (args.length) throw new Error(`unknown option ${args[0]}`);

const rustTarget = target === "esp32p4"
  ? "riscv32imafc-unknown-none-elf"
  : "xtensa-esp32s3-none-elf";
const targetDirectory = join(ROOT, "dist/esp-idf-native/cargo", target);
const command = [
  cargo,
  "build",
  "--release",
  "--locked",
  "--no-default-features",
  "--target",
  rustTarget,
  "--manifest-path",
  join(NATIVE, "Cargo.toml"),
];
if (target === "esp32s3") command.splice(2, 0, "-Zbuild-std=core,alloc");
const build = Bun.spawnSync(command, {
  cwd: ROOT,
  stdout: "inherit",
  stderr: "inherit",
  env: {
    ...process.env,
    PATH: `${dirname(cargo)}:${process.env.PATH ?? ""}`,
    CARGO_TARGET_DIR: targetDirectory,
  },
});
if (build.exitCode !== 0) throw new Error(`Rust build failed for ${target}`);

const sourceArchive = join(targetDirectory, rustTarget, "release/libpocketjs_idf_native.a");
const destinationDirectory = join(outputRoot, target);
const destinationArchive = join(destinationDirectory, "libpocketjs_idf_native.a");
mkdirSync(destinationDirectory, { recursive: true });
await Bun.write(destinationArchive, new Uint8Array(readFileSync(sourceArchive)));
if (target === "esp32p4") {
  const cmake = Bun.which("cmake");
  if (!cmake) throw new Error("cmake not found; it is required to prepare the ESP32-P4 archive");
  const prepare = Bun.spawnSync([
    cmake,
    `-DPOCKETJS_ARCHIVE=${destinationArchive}`,
    `-DPOCKETJS_ARCHIVER=${archiver}`,
    `-DPOCKETJS_TARGET=${target}`,
    "-P",
    join(ROOT, "hosts/esp-idf/components/pocketjs_ui_core/prepare_archive.cmake"),
  ], { stdout: "inherit", stderr: "inherit" });
  if (prepare.exitCode !== 0) throw new Error("failed to prepare the ESP32-P4 archive for ESP-IDF");
}
const archive = new Uint8Array(readFileSync(destinationArchive));

const sourceHash = createHash("sha256");
const sourceFiles: string[] = [];
for (const sourceRoot of [
  "engine/core",
  "engine/backends/esp32p4-ppa",
  "hosts/esp-idf/native",
]) {
  const glob = new Bun.Glob("**/*.{rs,toml,lock}");
  for await (const file of glob.scan({ cwd: join(ROOT, sourceRoot), absolute: false })) {
    if (file.split("/").some((part) => part === "target" || part.startsWith("target-"))) continue;
    sourceFiles.push(`${sourceRoot}/${file}`);
  }
}
if (sourceFiles.length === 0) throw new Error("native source hash matched no files");
for (const file of sourceFiles.sort()) {
  sourceHash.update(file).update("\0").update(readFileSync(join(ROOT, file)));
}
const rustc = join(dirname(cargo), "rustc");
const compiler = Bun.spawnSync([rustc, "-Vv"], { stdout: "pipe", stderr: "pipe" });
if (compiler.exitCode !== 0) throw new Error(`rustc next to ${cargo} is not runnable`);
const receipt = {
  schemaVersion: 1,
  target,
  rustTarget,
  compiler: compiler.stdout.toString().trim(),
  sourceSha256: sourceHash.digest("hex"),
  archiveSha256: createHash("sha256").update(archive).digest("hex"),
  archiveBytes: archive.length,
};
await Bun.write(join(destinationDirectory, "build-receipt.json"), JSON.stringify(receipt, null, 2) + "\n");
console.log(`${target}: ${destinationArchive}`);
console.log(`  sha256 ${receipt.archiveSha256} (${receipt.archiveBytes} bytes)`);
