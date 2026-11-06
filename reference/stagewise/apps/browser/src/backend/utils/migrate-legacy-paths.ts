import path from 'node:path';
import fs from 'node:fs';
import { app } from 'electron';
import type { Logger } from '../services/logger';

const SENTINEL_NAME = '.migrated-v1';

type MoveResult = 'moved' | 'skipped' | 'failed';

interface MigrationStats {
  moved: number;
  skipped: number;
  failed: number;
}

function moveIfExists(from: string, to: string, logger: Logger): MoveResult {
  if (!fs.existsSync(from)) return 'skipped';
  if (fs.existsSync(to)) return 'skipped';
  try {
    fs.renameSync(from, to);
    return 'moved';
  } catch (err) {
    logger.warn(`[Migration] Failed to move ${from} -> ${to}: ${err}`);
    return 'failed';
  }
}

function countResult(result: MoveResult, stats: MigrationStats): void {
  stats[result]++;
}

/**
 * Also move SQLite WAL and SHM companion files if they exist.
 * These only exist when the DB has uncommitted transactions.
 */
function moveSqlite(
  fromBase: string,
  toBase: string,
  logger: Logger,
  stats: MigrationStats,
): void {
  countResult(moveIfExists(fromBase, toBase, logger), stats);
  for (const suffix of ['-wal', '-shm']) {
    const r = moveIfExists(fromBase + suffix, toBase + suffix, logger);
    if (r === 'failed') stats.failed++;
  }
}

function migrateIdentityJson(
  userData: string,
  stagewiseRoot: string,
  logger: Logger,
  stats: MigrationStats,
): void {
  const oldPath = path.join(userData, 'identity.json');
  const newPath = path.join(stagewiseRoot, 'identity.json');

  if (!fs.existsSync(oldPath)) {
    stats.skipped++;
    return;
  }
  if (fs.existsSync(newPath)) {
    stats.skipped++;
    return;
  }

  try {
    const raw = fs.readFileSync(oldPath, 'utf-8').trim();

    // Old format: bare UUID string. New format: { machineId: "<uuid>" }
    const isAlreadyJson = raw.startsWith('{') && raw.includes('machineId');
    const json = isAlreadyJson
      ? raw
      : JSON.stringify({ machineId: raw }, null, 2);

    fs.writeFileSync(newPath, json, 'utf-8');
    fs.unlinkSync(oldPath);
    stats.moved++;
  } catch (err) {
    logger.warn(`[Migration] Failed to content-migrate identity.json: ${err}`);
    stats.failed++;
  }
}

function migrateAttachmentBlobs(
  userData: string,
  stagewiseRoot: string,
  logger: Logger,
  stats: MigrationStats,
): void {
  const oldRoot = path.join(userData, 'attachment-blobs');
  if (!fs.existsSync(oldRoot)) return;

  try {
    const entries = fs.readdirSync(oldRoot, { withFileTypes: true });
    for (const entry of entries) {
      if (!entry.isDirectory()) continue;
      const agentId = entry.name;
      const agentDir = path.join(stagewiseRoot, 'agents', agentId);
      fs.mkdirSync(agentDir, { recursive: true });

      const from = path.join(oldRoot, agentId);
      const to = path.join(agentDir, 'data-attachments');
      countResult(moveIfExists(from, to, logger), stats);
    }

    // Remove now-empty root (safe: only if empty)
    try {
      fs.rmdirSync(oldRoot);
    } catch {
      // Not empty or already gone -- ignore
    }
  } catch (err) {
    logger.warn(`[Migration] Failed to migrate attachment-blobs: ${err}`);
    stats.failed++;
  }
}

function cleanupCustomApps(userData: string, logger: Logger): void {
  const dir = path.join(userData, 'custom-apps');
  if (!fs.existsSync(dir)) return;

  try {
    fs.rmSync(dir, { recursive: true, force: true });
    logger.debug('[Migration] Removed legacy custom-apps directory');
  } catch (err) {
    logger.warn(`[Migration] Failed to remove custom-apps: ${err}`);
  }
}

/**
 * One-shot, synchronous migration from the legacy flat `<userData>/` layout
 * to the new `<userData>/stagewise/` structure.
 *
 * Safe to call on every launch -- a sentinel file makes subsequent calls
 * a no-op, and every individual move is guarded by existence checks.
 */
export function migrateLegacyPaths(logger: Logger): MigrationStats {
  const userData = app.getPath('userData');
  const stagewiseRoot = path.join(userData, 'stagewise');
  const sentinelPath = path.join(stagewiseRoot, SENTINEL_NAME);

  if (fs.existsSync(sentinelPath)) {
    return { moved: 0, skipped: 0, failed: 0 };
  }

  logger.debug('[Migration] Starting legacy path migration...');

  // Ensure target directories exist
  for (const dir of [
    stagewiseRoot,
    path.join(stagewiseRoot, 'agents'),
    path.join(stagewiseRoot, 'diff-history'),
    path.join(stagewiseRoot, 'bin'),
  ]) {
    fs.mkdirSync(dir, { recursive: true });
  }

  const stats: MigrationStats = { moved: 0, skipped: 0, failed: 0 };

  // -----------------------------------------------------------------------
  // SQLite databases (+ WAL/SHM companions)
  // -----------------------------------------------------------------------
  const sqliteMoves: [string, string][] = [
    ['Favicons', 'favicon.sqlite'],
    ['Web Data', 'web-data.sqlite'],
    ['History', 'history.sqlite'],
    ['Thumbnails', 'thumbnails.sqlite'],
    ['Agents', path.join('agents', 'instances.sqlite')],
    ['DiffHistory', path.join('diff-history', 'data.sqlite')],
  ];

  for (const [oldName, newName] of sqliteMoves) {
    moveSqlite(
      path.join(userData, oldName),
      path.join(stagewiseRoot, newName),
      logger,
      stats,
    );
  }

  // -----------------------------------------------------------------------
  // JSON files (straight rename, no content change)
  // -----------------------------------------------------------------------
  const jsonMoves: [string, string][] = [
    ['credentials.json', 'auth-session.json'],
    ['AgentCredentials.json', 'credentials.json'],
    ['Preferences.json', 'preferences.json'],
    ['config.json', 'config.json'],
    ['recently-opened-workspaces.json', 'recently-opened-workspaces.json'],
    ['onboarding-state.json', 'onboarding-state.json'],
    ['downloads-state.json', 'downloads-state.json'],
    ['window-state.json', 'window-state.json'],
  ];

  for (const [oldName, newName] of jsonMoves) {
    countResult(
      moveIfExists(
        path.join(userData, oldName),
        path.join(stagewiseRoot, newName),
        logger,
      ),
      stats,
    );
  }

  // -----------------------------------------------------------------------
  // Directories (straight rename)
  // -----------------------------------------------------------------------
  countResult(
    moveIfExists(
      path.join(userData, 'diff-history-blobs'),
      path.join(stagewiseRoot, 'diff-history', 'data-blobs'),
      logger,
    ),
    stats,
  );

  // bin/ (ripgrep) -- only move contents, target dir already created above
  const oldBin = path.join(userData, 'bin');
  if (fs.existsSync(oldBin)) {
    try {
      const entries = fs.readdirSync(oldBin, { withFileTypes: true });
      for (const entry of entries) {
        const from = path.join(oldBin, entry.name);
        const to = path.join(stagewiseRoot, 'bin', entry.name);
        countResult(moveIfExists(from, to, logger), stats);
      }
      try {
        fs.rmdirSync(oldBin);
      } catch {
        // Not empty or already gone
      }
    } catch (err) {
      logger.warn(`[Migration] Failed to migrate bin/: ${err}`);
      stats.failed++;
    }
  }

  // -----------------------------------------------------------------------
  // Special: identity.json (content migration: raw UUID -> JSON object)
  // -----------------------------------------------------------------------
  migrateIdentityJson(userData, stagewiseRoot, logger, stats);

  // -----------------------------------------------------------------------
  // Special: attachment-blobs/<agentId>/ -> agents/<agentId>/data-attachments/
  // -----------------------------------------------------------------------
  migrateAttachmentBlobs(userData, stagewiseRoot, logger, stats);

  // -----------------------------------------------------------------------
  // Cleanup: delete legacy custom-apps/ (incompatible with new per-agent layout)
  // -----------------------------------------------------------------------
  cleanupCustomApps(userData, logger);

  // -----------------------------------------------------------------------
  // Write sentinel only if zero failures
  // -----------------------------------------------------------------------
  if (stats.failed === 0) {
    try {
      fs.writeFileSync(sentinelPath, new Date().toISOString(), 'utf-8');
    } catch (err) {
      logger.warn(`[Migration] Failed to write sentinel: ${err}`);
    }
  }

  logger.debug(
    `[Migration] Complete: ${stats.moved} moved, ${stats.skipped} skipped, ${stats.failed} failed`,
  );

  return stats;
}
