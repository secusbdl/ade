import type { z } from 'zod';
import fs from 'node:fs/promises';
import fsSync from 'node:fs';
import { safeStorage } from 'electron';
import { type JsonName, getJsonPath } from './paths';

export type { JsonName };

interface PersistedDataOptions {
  encrypt?: boolean;
}

// ---------------------------------------------------------------------------
// Async variants
// ---------------------------------------------------------------------------

export async function readPersistedData<T extends z.ZodTypeAny>(
  name: JsonName,
  schema: T,
  defaultValue: z.infer<T>,
  options?: PersistedDataOptions,
): Promise<z.infer<T>> {
  const filePath = getJsonPath(name);
  try {
    let content: string;

    if (options?.encrypt) {
      const buffer = await fs.readFile(filePath);
      try {
        if (safeStorage.isEncryptionAvailable())
          content = safeStorage.decryptString(buffer);
        else content = buffer.toString('utf-8');
      } catch {
        content = buffer.toString('utf-8');
      }
    } else content = await fs.readFile(filePath, 'utf-8');

    return schema.parse(JSON.parse(content));
  } catch {
    return defaultValue;
  }
}

export async function writePersistedData<T extends z.ZodTypeAny>(
  name: JsonName,
  schema: T,
  data: z.infer<T>,
  options?: PersistedDataOptions,
): Promise<void> {
  const filePath = getJsonPath(name);
  schema.parse(data);

  const json = JSON.stringify(data, null, 2);

  if (options?.encrypt) {
    try {
      if (safeStorage.isEncryptionAvailable()) {
        const encrypted = safeStorage.encryptString(json);
        await fs.writeFile(filePath, encrypted);
        return;
      }
    } catch {
      // fall through to plaintext write
    }
  }

  await fs.writeFile(filePath, json, 'utf-8');
}

// ---------------------------------------------------------------------------
// Sync variants (used by WindowLayoutService for startup window-state)
// ---------------------------------------------------------------------------

export function readPersistedDataSync<T extends z.ZodTypeAny>(
  name: JsonName,
  schema: T,
  defaultValue: z.infer<T>,
  options?: PersistedDataOptions,
): z.infer<T> {
  const filePath = getJsonPath(name);
  try {
    let content: string;

    if (options?.encrypt) {
      const buffer = fsSync.readFileSync(filePath);
      try {
        if (safeStorage.isEncryptionAvailable())
          content = safeStorage.decryptString(buffer);
        else content = buffer.toString('utf-8');
      } catch {
        content = buffer.toString('utf-8');
      }
    } else content = fsSync.readFileSync(filePath, 'utf-8');

    return schema.parse(JSON.parse(content));
  } catch {
    return defaultValue;
  }
}

export function writePersistedDataSync<T extends z.ZodTypeAny>(
  name: JsonName,
  schema: T,
  data: z.infer<T>,
  options?: PersistedDataOptions,
): void {
  const filePath = getJsonPath(name);
  schema.parse(data);

  const json = JSON.stringify(data, null, 2);

  if (options?.encrypt) {
    try {
      if (safeStorage.isEncryptionAvailable()) {
        const encrypted = safeStorage.encryptString(json);
        fsSync.writeFileSync(filePath, encrypted);
        return;
      }
    } catch {
      // fall through to plaintext write
    }
  }

  fsSync.writeFileSync(filePath, json, 'utf-8');
}
