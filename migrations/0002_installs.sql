-- The anonymous install count (functions/api/install.js). Apply with
-- tools/reports.py migrate, on both databases (wrangler.toml); tools/installs.py reads it.

-- One row per counted install. Nothing in it identifies anyone: the day (no time), and four fixed-list values.
CREATE TABLE installs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  day TEXT NOT NULL,
  board TEXT NOT NULL,
  edition TEXT NOT NULL,
  installer TEXT NOT NULL,
  result TEXT NOT NULL,
  failure TEXT
);

-- Its rate limit, apart from the report form's (hits), with the same keys: SHA-256(today's salt | IP), kept one day.
CREATE TABLE install_hits (key TEXT NOT NULL, at INTEGER NOT NULL);
CREATE INDEX install_hits_key_at ON install_hits (key, at);
CREATE INDEX install_hits_at ON install_hits (at);
