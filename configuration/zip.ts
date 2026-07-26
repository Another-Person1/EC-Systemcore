import type { FileHandle } from "node:fs/promises";
import { basename } from "node:path";

export const MAX_ZIP_ENTRIES = 32;
export const MAX_ZIP_ENTRY_BYTES = 128 * 1024 * 1024;
export const MAX_ZIP_TOTAL_BYTES = 512 * 1024 * 1024;
const ZIP_CHUNK_BYTES = 64 * 1024;
const ZIP_UTF8_DATA_DESCRIPTOR_FLAGS = 0x0808;

export type OpenZipEntry = {
  name: string;
  size: number;
  modifiedAt: number;
  handle: FileHandle;
};

type CentralEntry = {
  name: Uint8Array;
  crc: number;
  size: number;
  offset: number;
  time: number;
  date: number;
};

function u16(value: number): Uint8Array {
  return new Uint8Array([value & 0xff, (value >>> 8) & 0xff]);
}

function u32(value: number): Uint8Array {
  return new Uint8Array([
    value & 0xff,
    (value >>> 8) & 0xff,
    (value >>> 16) & 0xff,
    (value >>> 24) & 0xff
  ]);
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((sum, part) => sum + part.byteLength, 0);
  const output = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.byteLength;
  }
  return output;
}

function zipDateParts(milliseconds: number): { time: number; date: number } {
  const date = new Date(milliseconds);
  const year = Math.max(1980, Math.min(2107, date.getFullYear()));
  return {
    time:
      ((date.getHours() & 0x1f) << 11)
      | ((date.getMinutes() & 0x3f) << 5)
      | Math.floor(date.getSeconds() / 2),
    date:
      (((year - 1980) & 0x7f) << 9)
      | (((date.getMonth() + 1) & 0x0f) << 5)
      | (date.getDate() & 0x1f)
  };
}

const crcTable = new Uint32Array(256).map((_, index) => {
  let value = index;
  for (let bit = 0; bit < 8; bit += 1) {
    value = (value & 1) !== 0 ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
  }
  return value >>> 0;
});

function updateCrc32(crc: number, data: Uint8Array): number {
  let next = crc;
  for (const byte of data) next = crcTable[(next ^ byte) & 0xff]! ^ (next >>> 8);
  return next >>> 0;
}

function localHeader(name: Uint8Array, time: number, date: number): Uint8Array {
  return concat([
    u32(0x04034b50),
    u16(20),
    u16(ZIP_UTF8_DATA_DESCRIPTOR_FLAGS),
    u16(0),
    u16(time),
    u16(date),
    u32(0),
    u32(0),
    u32(0),
    u16(name.byteLength),
    u16(0),
    name
  ]);
}

function dataDescriptor(crc: number, size: number): Uint8Array {
  return concat([u32(0x08074b50), u32(crc), u32(size), u32(size)]);
}

function centralHeader(entry: CentralEntry): Uint8Array {
  return concat([
    u32(0x02014b50),
    u16(20),
    u16(20),
    u16(ZIP_UTF8_DATA_DESCRIPTOR_FLAGS),
    u16(0),
    u16(entry.time),
    u16(entry.date),
    u32(entry.crc),
    u32(entry.size),
    u32(entry.size),
    u16(entry.name.byteLength),
    u16(0),
    u16(0),
    u16(0),
    u16(0),
    u32(0),
    u32(entry.offset),
    entry.name
  ]);
}

function endOfCentralDirectory(
  entryCount: number,
  centralBytes: number,
  centralOffset: number
): Uint8Array {
  return concat([
    u32(0x06054b50),
    u16(0),
    u16(0),
    u16(entryCount),
    u16(entryCount),
    u32(centralBytes),
    u32(centralOffset),
    u16(0)
  ]);
}

function validateEntries(entries: OpenZipEntry[]): void {
  if (entries.length === 0 || entries.length > MAX_ZIP_ENTRIES) {
    throw new Error(`archive must contain 1 to ${MAX_ZIP_ENTRIES} files`);
  }
  let total = 0;
  const names = new Set<string>();
  const handles = new Set<FileHandle>();
  for (const entry of entries) {
    const hasControlCharacter = [...entry.name].some((character) => {
      const code = character.charCodeAt(0);
      return code <= 0x1f || code === 0x7f;
    });
    if (
      basename(entry.name) !== entry.name
      || entry.name.includes("\\")
      || hasControlCharacter
      || names.has(entry.name)
    ) {
      throw new Error("archive filenames must be unique basenames");
    }
    names.add(entry.name);
    if (handles.has(entry.handle)) throw new Error("archive file handles must be unique");
    handles.add(entry.handle);
    if (!Number.isSafeInteger(entry.size) || entry.size < 0 || entry.size > MAX_ZIP_ENTRY_BYTES) {
      throw new Error(`${entry.name} exceeds the per-file archive limit`);
    }
    if (!Number.isFinite(entry.modifiedAt)) throw new Error(`${entry.name} has an invalid timestamp`);
    total += entry.size;
    if (total > MAX_ZIP_TOTAL_BYTES) throw new Error("archive exceeds the total-byte limit");
  }
}

async function* generateStoredZip(entries: OpenZipEntry[]): AsyncGenerator<Uint8Array> {
  const encoder = new TextEncoder();
  const central: CentralEntry[] = [];
  const openHandles = new Set(entries.map((entry) => entry.handle));
  let archiveOffset = 0;

  try {
    validateEntries(entries);
    for (const entry of entries) {
      const name = encoder.encode(entry.name);
      if (name.byteLength === 0 || name.byteLength > 255) {
        throw new Error("archive filename is too long");
      }
      const stamp = zipDateParts(entry.modifiedAt);
      const header = localHeader(name, stamp.time, stamp.date);
      const entryOffset = archiveOffset;
      archiveOffset += header.byteLength;
      yield header;

      let crc = 0xffff_ffff;
      let position = 0;
      while (position < entry.size) {
        const requested = Math.min(ZIP_CHUNK_BYTES, entry.size - position);
        const buffer = new Uint8Array(requested);
        const { bytesRead } = await entry.handle.read(buffer, 0, requested, position);
        if (bytesRead <= 0) throw new Error(`${entry.name} changed while it was downloaded`);
        const chunk = bytesRead === buffer.byteLength ? buffer : buffer.slice(0, bytesRead);
        crc = updateCrc32(crc, chunk);
        position += bytesRead;
        archiveOffset += bytesRead;
        yield chunk;
      }
      const finalCrc = (crc ^ 0xffff_ffff) >>> 0;
      const descriptor = dataDescriptor(finalCrc, entry.size);
      archiveOffset += descriptor.byteLength;
      yield descriptor;
      central.push({
        name,
        crc: finalCrc,
        size: entry.size,
        offset: entryOffset,
        time: stamp.time,
        date: stamp.date
      });
      await entry.handle.close();
      openHandles.delete(entry.handle);
    }

    const centralOffset = archiveOffset;
    let centralBytes = 0;
    for (const entry of central) {
      const header = centralHeader(entry);
      centralBytes += header.byteLength;
      yield header;
    }
    yield endOfCentralDirectory(central.length, centralBytes, centralOffset);
  } finally {
    await Promise.allSettled([...openHandles].map((handle) => handle.close()));
  }
}

export function createStoredZipStream(entries: OpenZipEntry[]): ReadableStream<Uint8Array> {
  const iterator = generateStoredZip(entries);
  let started = false;
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      try {
        started = true;
        const next = await iterator.next();
        if (next.done) controller.close();
        else controller.enqueue(next.value);
      } catch (error) {
        controller.error(error);
      }
    },
    async cancel() {
      if (started) {
        await iterator.return(undefined);
      } else {
        await Promise.allSettled(entries.map((entry) => entry.handle.close()));
      }
    }
  });
}
