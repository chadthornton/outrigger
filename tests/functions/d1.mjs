// A stand-in for Cloudflare D1 on the Mac: the D1 calls functions/ makes
// (prepare, bind, first, run, all), on node:sqlite in memory, with the
// schema from migrations/. Real SQL, so a query typo fails here too.
import { DatabaseSync } from "node:sqlite";
import { readFileSync, readdirSync } from "node:fs";

const MIGRATIONS = new URL("../../migrations/", import.meta.url);

class Statement {
  constructor(db, sql, args = []) {
    Object.assign(this, { db, sql, args });
  }

  bind(...args) {
    return new Statement(this.db, this.sql, args);
  }

  async first() {
    return this.db.prepare(this.sql).get(...this.args) ?? null;
  }

  async run() {
    const r = this.db.prepare(this.sql).run(...this.args);
    return { success: true, meta: { last_row_id: Number(r.lastInsertRowid), changes: Number(r.changes) } };
  }

  async all() {
    return { success: true, results: this.db.prepare(this.sql).all(...this.args) };
  }
}

export function fakeD1() {
  const db = new DatabaseSync(":memory:");
  for (const name of readdirSync(MIGRATIONS).filter((n) => n.endsWith(".sql")).sort()) {
    db.exec(readFileSync(new URL(name, MIGRATIONS), "utf8"));
  }
  return { prepare: (sql) => new Statement(db, sql), sqlite: db };
}
