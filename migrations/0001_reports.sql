-- "Report this board" (functions/api/report.js). Apply with
-- tools/reports.py migrate, on both databases (wrangler.toml).

-- One row per report. email is the only personal field: deleted on request (tools/reports.py forget).
CREATE TABLE reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_at TEXT NOT NULL,
  chip TEXT,
  flash_mb INTEGER,
  psram_mb INTEGER,
  usb_vid INTEGER,
  picked TEXT,
  verdict TEXT,
  installer TEXT,
  board TEXT NOT NULL,
  note TEXT,
  email TEXT
);

-- The rate limit: one row per accepted report, key = SHA-256(today's salt | IP). Kept one day.
CREATE TABLE hits (key TEXT NOT NULL, at INTEGER NOT NULL);
CREATE INDEX hits_key_at ON hits (key, at);
CREATE INDEX hits_at ON hits (at);

-- Today's random salt for those keys; yesterday's is deleted, so old keys can't be tied to anyone.
CREATE TABLE salts (day TEXT PRIMARY KEY, salt TEXT NOT NULL);
