import readline from "readline";
import { spawn } from "child_process";
import * as fs from "fs";
import * as path from "path";

import { assert, spawnChecked, toNumber, getBackendDir } from "./common.mjs";

export async function readSymbols(file, pdbFile) {
  let symbols = {};
  if (process.platform == "win32") {
    if (!pdbFile) {
      throw new Error("Need PDB file to read symbols on windows");
    }

    const pdbPath = path.join(getBackendDir(), "lib", "symextract.exe");
    const process = spawn(pdbPath, ["read_symbols", pdbFile]);
    process.on("error", (error) => {
      console.error(`spawn error: ${error}`);
    });

    const lines = readline.createInterface({
      input: process.stdout,
      crlfDelay: Infinity,
    });
    let allLines = [];
    for await (const line of lines) {
      allLines.push(line);
    }

    // TODO: this just converts a string to an object, and then
    // immediately converts it back (in the caller.)  Fix this.
    symbols = JSON.parse(allLines.join(''));
  } else {
    const nmProcess = spawn("/usr/bin/nm", [file], { maxBuffer: 1e100 });

    nmProcess.on("error", (error) => {
      console.error(`spawn error: ${error}`);
    });

    let stdout = "";
    nmProcess.stdout.on("data", (data) => {
      stdout += data;
    });

    await new Promise((resolve, reject) => {
      nmProcess.on("exit", resolve);
      nmProcess.on("error", reject);
    });

    const lines = stdout.split("\n");
    for (const line of lines) {
      const arr = /^(.*?) [t|T|W] (.*)/.exec(line);
      if (arr) {
        try {
          symbols[toNumber(`0x${arr[1]}`)] = arr[2];
        } catch (e) {
          console.log(arr[2], e);
        }
      }
    }
  }
  return symbols;
}

// Writes <buildId>[-arm].symbols.tgz into the current directory and returns its
// name: one <buildId>[-arm].symbols.json mapping each library's base name to its
// symbols. The backend looks a build's symbols up under this name (symbols/ in
// storage, or REPLAY_SYMBOLS_DIR).
export async function buildSymbolsArchive(
  buildId,
  objectDirectory,
  libraries,
  useArm,
  pdbs = []
) {
  const json = {};
  for (let i = 0; i < libraries.length; i++) {
    const lib = libraries[i];
    const name = path.basename(lib);
    const file = path.join(objectDirectory, lib);
    assert(fs.existsSync(file), `Missing binary for symbols archive ${file}`);
    let pdbFile;
    if (pdbs[i]) {
      pdbFile = path.join(objectDirectory, pdbs[i]);
      assert(
        fs.existsSync(pdbFile),
        `Missing PDB for symbols archive ${pdbFile}`
      );
    }
    const symbols = await readSymbols(file, pdbFile);
    json[name] = symbols;
  }

  const archSuffix = useArm ? "-arm" : "";

  const jsonFile = `${buildId}${archSuffix}.symbols.json`;
  const archiveFile = `${buildId}${archSuffix}.symbols.tgz`;
  fs.writeFileSync(jsonFile, JSON.stringify(json));
  spawnChecked("tar", ["-czf", archiveFile, jsonFile]);
  fs.unlinkSync(jsonFile);

  return archiveFile;
}

// Get the start virtual address of the text section from a PDB file.
// Symbol addresses are relative to the start of this section.
// eslint-disable-next-line no-unused-vars
async function getTextSectionAddress(pdbFile) {
  const pdbPath = path.join(getBackendDir(), "lib", "llvm-pdbutil.exe");
  const pdbProcess = spawn(pdbPath, ["dump", "-section-headers", pdbFile]);

  pdbProcess.on("error", (error) => {
    console.error(`spawn error: ${error}`);
  });

  let stdout = "";
  pdbProcess.stdout.on("data", (data) => {
    stdout += data;
  });

  await new Promise((resolve, reject) => {
    pdbProcess.on("exit", resolve);
    pdbProcess.on("error", reject);
  });

  const lines = stdout.toString().split("\n");

  let inTextSection = false;
  for (const line of lines) {
    if (line.includes(".text name")) {
      inTextSection = true;
    }
    const match = /([0-9A-F]+) virtual address/.exec(line);
    if (match) {
      if (!inTextSection) {
        throw new Error("Expected first section to be text section");
      }
      return parseInt(`0x${match[1]}`);
    }
  }
  throw new Error("Could not find start of text section");
}
