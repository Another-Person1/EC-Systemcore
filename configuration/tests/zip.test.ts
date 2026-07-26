import { afterEach, describe, expect, test } from "bun:test";
import { mkdtemp, open, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  MAX_ZIP_ENTRY_BYTES,
  createStoredZipStream,
  type OpenZipEntry
} from "../zip";

const temporaryRoots: string[] = [];

async function temporaryFile(name: string, contents: string): Promise<OpenZipEntry> {
  const root = await mkdtemp(join(tmpdir(), "ec-systemcore-zip-"));
  temporaryRoots.push(root);
  const path = join(root, name);
  await writeFile(path, contents);
  return {
    name,
    size: Buffer.byteLength(contents),
    modifiedAt: Date.UTC(2026, 0, 2, 3, 4, 6),
    handle: await open(path, "r")
  };
}

async function expectClosed(entry: OpenZipEntry): Promise<void> {
  await expect(entry.handle.stat()).rejects.toThrow();
}

function containsSignature(bytes: Uint8Array, signature: number[]): boolean {
  return bytes.some((_, offset) =>
    signature.every((value, index) => bytes[offset + index] === value)
  );
}

afterEach(async () => {
  await Promise.allSettled(
    temporaryRoots.splice(0).map((root) => rm(root, { recursive: true, force: true }))
  );
});

describe("bounded streaming ZIP writer", () => {
  test("streams stored entries with data descriptors and closes every handle", async () => {
    const first = await temporaryFile("first.log", "alpha\n");
    const second = await temporaryFile("second.wpilog", "binary-ish");
    const bytes = new Uint8Array(
      await new Response(createStoredZipStream([first, second])).arrayBuffer()
    );

    expect([...bytes.slice(0, 4)]).toEqual([0x50, 0x4b, 0x03, 0x04]);
    expect(containsSignature(bytes, [0x50, 0x4b, 0x07, 0x08])).toBe(true);
    expect(containsSignature(bytes, [0x50, 0x4b, 0x01, 0x02])).toBe(true);
    expect([...bytes.slice(-22, -18)]).toEqual([0x50, 0x4b, 0x05, 0x06]);
    expect(new TextDecoder().decode(bytes)).toContain("alpha");
    await expectClosed(first);
    await expectClosed(second);
  });

  test("cancellation closes handles before and after streaming starts", async () => {
    const beforeRead = await temporaryFile("cancel-before.log", "before");
    const beforeStream = createStoredZipStream([beforeRead]);
    await beforeStream.cancel("test cancellation");
    await expectClosed(beforeRead);

    const afterRead = await temporaryFile(
      "cancel-after.log",
      "x".repeat(128 * 1024)
    );
    const reader = createStoredZipStream([afterRead]).getReader();
    expect((await reader.read()).done).toBe(false);
    await reader.cancel("test cancellation");
    await expectClosed(afterRead);
  });

  test("rejects duplicate names, unsafe names, declared limits, and file shrink races", async () => {
    const duplicateOne = await temporaryFile("duplicate.log", "one");
    const duplicateTwo = await temporaryFile("duplicate.log", "two");
    await expect(
      new Response(createStoredZipStream([duplicateOne, duplicateTwo])).arrayBuffer()
    ).rejects.toThrow("unique basenames");
    await expectClosed(duplicateOne);
    await expectClosed(duplicateTwo);

    const unsafe = await temporaryFile("safe.log", "data");
    unsafe.name = "..\\escape.log";
    await expect(
      new Response(createStoredZipStream([unsafe])).arrayBuffer()
    ).rejects.toThrow("unique basenames");
    await expectClosed(unsafe);

    const oversized = await temporaryFile("oversized.log", "tiny");
    oversized.size = MAX_ZIP_ENTRY_BYTES + 1;
    await expect(
      new Response(createStoredZipStream([oversized])).arrayBuffer()
    ).rejects.toThrow("per-file");
    await expectClosed(oversized);

    const shrunk = await temporaryFile("shrunk.log", "short");
    shrunk.size += 10;
    await expect(
      new Response(createStoredZipStream([shrunk])).arrayBuffer()
    ).rejects.toThrow("changed");
    await expectClosed(shrunk);
  });
});
