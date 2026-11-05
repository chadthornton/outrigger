// A pinned deploy's installer check (tools/deploy_site.py, docs/publishing.md):
// the staged site's own installer code (installer/install.js fetchBuild) loads
// every build under firmware/, from files instead of the network, so the pages
// going up can read the firmware they go up with. Also: every manifest a page's
// data-builds names exists, and every board folder is named by a page.
//
//     node tools/check_pinned.mjs <staged site dir>    prints the problems; exit 1 if any
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const ENTITIES = { "&quot;": '"', "&#39;": "'", "&lt;": "<", "&gt;": ">", "&amp;": "&" };

// What fetchBuild needs of a fetch Response, from a file.
async function fileFetch(path) {
  if (!existsSync(path) || !statSync(path).isFile()) return { ok: false };
  const data = readFileSync(path);
  return { ok: true, json: async () => JSON.parse(data), arrayBuffer: async () => data };
}

function pages(dir) {
  return readdirSync(dir, { recursive: true }).filter((f) => f.endsWith(".html")).map((f) => join(dir, f));
}

// Manifest paths, relative to the site, named by each page's data-builds.
function namedManifests(site) {
  const named = [];
  for (const page of pages(site)) {
    for (const [, , attr] of readFileSync(page, "utf8").matchAll(/data-builds=(["'])(.*?)\1/gs)) {
      const builds = JSON.parse(attr.replace(/&(quot|#39|lt|gt|amp);/g, (e) => ENTITIES[e]));
      for (const b of builds) {
        if (b.manifest) named.push({ page: relative(site, page), manifest: relative(site, resolve(dirname(page), b.manifest)) });
      }
    }
  }
  return named;
}

export async function pinnedProblems(site) {
  const { fetchBuild } = await import(pathToFileURL(join(site, "installer", "install.js")));
  const firmware = join(site, "firmware");
  const builds = existsSync(firmware)
    ? readdirSync(firmware, { recursive: true }).filter((f) => f.split(/[\\/]/).length === 3 && f.endsWith("manifest.json"))
    : [];
  if (!builds.length) return ["no firmware/<board>/<app set>/manifest.json"];
  const problems = [];
  for (const build of builds.sort()) {
    try {
      await fetchBuild(join(firmware, build), fileFetch);
    } catch (e) {
      problems.push(`firmware/${dirname(build)}: the installer can't load it: ${e.message}`);
    }
  }
  const named = namedManifests(site);
  for (const { page, manifest } of named) {
    if (!existsSync(join(site, manifest))) problems.push(`${page} names ${manifest}, which the firmware lacks`);
  }
  const boards = new Set(named.map((n) => n.manifest.split(/[\\/]/)[1]));
  for (const board of readdirSync(firmware).filter((b) => statSync(join(firmware, b)).isDirectory()).sort()) {
    if (!boards.has(board)) problems.push(`firmware/${board} is named by no page`);
  }
  return problems;
}

if (import.meta.url === pathToFileURL(process.argv[1] || "").href) {
  const problems = await pinnedProblems(resolve(process.argv[2]));
  for (const p of problems) console.error(p);
  process.exit(problems.length ? 1 : 0);
}
