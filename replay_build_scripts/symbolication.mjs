import readline from "readline";
import { fileURLToPath } from "url";
import { dirname } from "path";

import { toNumber, spawnChecked } from "./common.mjs";

// NOTE(dmiller): see https://stackoverflow.com/a/62892482 for explanation
const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

export async function readSymbols(file, pdbFile) {
  const symbols = {};
  if (process.platform === "win32") {
    if (!pdbFile) {
      throw new Error("Need PDB file to read symbols on windows");
    }
    const textStart = getTextSectionAddress(pdbFile);

    const process = spawnChecked(
      `${__dirname}\\..\\..\\lib\\llvm-pdbutil.exe`,
      ["dump", "-symbols", pdbFile],
      { maxBuffer: 1e100 }
    );

    const lines = readline.createInterface({
      input: process.stdout,
      crlfDelay: Infinity,
    });
    let currentFunction;
    const addrRegex = /addr = 0001:(\d+)/;
    const functionRegex = /(S_GPROC32|S_LPROC32).*`(.*)`/;
    for await (const line of lines) {
      if (currentFunction) {
        const match = addrRegex.exec(line);
        if (match) {
          const addr = textStart + +match[1];
          symbols[addr] = currentFunction;
        }
        currentFunction = undefined;
      } else {
        const match = functionRegex.exec(line);
        if (match) {
          currentFunction = match[2];
        }
      }
    }
  } else {
    const lines = spawnChecked("/usr/bin/nm", [file], { maxBuffer: 1e100 })
      .stdout.toString()
      .split("\n");
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

// Get the start virtual address of the text section from a PDB file.
// Symbol addresses are relative to the start of this section.
function getTextSectionAddress(pdbFile) {
  const lines = spawnChecked(`${__dirname}\\..\\..\\lib\\llvm-pdbutil.exe`, [
    "dump",
    "-section-headers",
    pdbFile,
  ])
    .stdout.toString()
    .split("\n");

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
