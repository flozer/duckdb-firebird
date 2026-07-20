#!/usr/bin/env node

import fs from 'node:fs/promises';
import path from 'node:path';

const EXTENSION = 'firebird';
const START_DATE = new Date('2026-01-04T00:00:00Z');
const OUT_FILE = path.join('.github', 'badges', 'downloads.json');
const WEEKLY_URL =
  'https://community-extensions.duckdb.org/download-stats-weekly';
const METRICS_PAGE = 'https://duckdb.org/community_extensions/download_metrics';

function isoWeekParts(date) {
  const d = new Date(Date.UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate()));
  const day = d.getUTCDay() || 7;
  d.setUTCDate(d.getUTCDate() + 4 - day);
  const yearStart = new Date(Date.UTC(d.getUTCFullYear(), 0, 1));
  const week = Math.ceil((((d - yearStart) / 86400000) + 1) / 7);
  return { year: d.getUTCFullYear(), week };
}

function addDays(date, days) {
  const next = new Date(date);
  next.setUTCDate(next.getUTCDate() + days);
  return next;
}

function formatCount(value) {
  return new Intl.NumberFormat('en-US', {
    notation: value >= 1000 ? 'compact' : 'standard',
    maximumFractionDigits: value >= 10000 ? 0 : 1,
  }).format(value);
}

async function readExisting() {
  try {
    return JSON.parse(await fs.readFile(OUT_FILE, 'utf8'));
  } catch {
    return null;
  }
}

async function fetchWeek(year, week) {
  const url = `${WEEKLY_URL}/${year}/${week}.json`;
  const response = await fetch(url);
  if (response.status === 404) {
    return null;
  }
  if (!response.ok) {
    throw new Error(`Failed to fetch ${url}: HTTP ${response.status}`);
  }
  const payload = await response.json();
  const downloads = Number(payload[EXTENSION] ?? 0);
  return {
    url,
    downloads: Number.isFinite(downloads) ? downloads : 0,
    lastUpdate: payload._last_update ?? null,
  };
}

async function main() {
  const today = new Date();
  const weeks = [];
  for (let cursor = new Date(START_DATE); cursor <= today; cursor = addDays(cursor, 7)) {
    const { year, week } = isoWeekParts(cursor);
    if (week === 53) {
      continue;
    }
    weeks.push({ year, week });
  }

  let total = 0;
  let weeksWithData = 0;
  let lastUpdate = null;
  for (const { year, week } of weeks) {
    const data = await fetchWeek(year, week);
    if (!data) {
      continue;
    }
    total += data.downloads;
    weeksWithData += 1;
    lastUpdate = data.lastUpdate ?? lastUpdate;
  }

  const existing = await readExisting();
  if (existing?.total === total) {
    console.log(`No badge update needed; ${EXTENSION} total downloads still ${total}.`);
    return;
  }

  const badge = {
    schemaVersion: 1,
    label: 'downloads',
    message: `${formatCount(total)} total`,
    color: 'blue',
    total,
    extension: EXTENSION,
    source: METRICS_PAGE,
    source_data: WEEKLY_URL,
    weeks_with_data: weeksWithData,
    last_update: lastUpdate,
    generated_at: new Date().toISOString(),
  };

  await fs.mkdir(path.dirname(OUT_FILE), { recursive: true });
  await fs.writeFile(OUT_FILE, `${JSON.stringify(badge, null, 2)}\n`);
  console.log(`Updated ${OUT_FILE}: ${total} total downloads across ${weeksWithData} weekly files.`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
