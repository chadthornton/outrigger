#include "apps.h"
#if APP_LOADER && APP_WEB_UI

#include "store_json.h"

#include <stdio.h>

#include "json_field.h"

using namespace fm1_loader;

namespace {

void add_string(std::string &out, const char *key, const std::string &value) {
  std::string escaped(value.size() * 6 + 1, '\0');
  escaped.resize(web_core::json_escape(value.c_str(), &escaped[0], escaped.size()));
  out += '"';
  out += key;
  out += "\":\"";
  out += escaped;
  out += '"';
}

void add_number(std::string &out, const char *key, unsigned long long value) {
  char buf[48];
  snprintf(buf, sizeof buf, "\"%s\":%llu", key, value);
  out += buf;
}

}  // namespace

std::string store_json(Library &library, Storage &storage, uint64_t fs_total, uint64_t fs_used) {
  std::string out = "{";
  add_number(out, "fs_total", fs_total);
  out += ',';
  add_number(out, "fs_used", fs_used);
  out += ',';
  add_string(out, "last_unit", library.last_unit());

  out += ",\"units\":[";
  bool first = true;
  for (const std::string &id : library.unit_ids()) {
    Unit u;
    if (!library.unit(id, u)) continue;
    out += first ? "{" : ",{";
    first = false;
    add_string(out, "id", u.id);
    out += ',';
    add_string(out, "name", u.name);
    out += ',';
    add_string(out, "baseline", u.baseline_backup);
    out += ',';
    add_string(out, "latest", u.latest_backup);
    out += ',';
    add_string(out, "installed", u.installed_package);
    out += ',';
    add_string(out, "pending", u.has_pending ? u.pending.package_id : std::string());
    out += ",\"saves\":[";
    bool first_save = true;
    for (const SaveState &s : library.saves_of(u.id)) {
      out += first_save ? "{" : ",{";
      first_save = false;
      add_string(out, "id", s.id);
      out += ',';
      add_string(out, "label", s.label);
      out += ',';
      add_string(out, "identity", s.identity);
      out += ',';
      add_number(out, "seq", s.seq);
      out += ',';
      add_number(out, "bytes", s.sectors.size() * uint64_t(kSector));
      out += '}';
    }
    out += "]}";
  }

  out += "],\"backups\":[";
  first = true;
  for (const std::string &key : storage.list("m/backup/")) {
    out += first ? "" : ",";
    first = false;
    std::string escaped(key.size() * 6 + 1, '\0');
    escaped.resize(web_core::json_escape(key.c_str() + 9, &escaped[0], escaped.size()));
    out += '"' + escaped + '"';
  }

  out += "],\"packages\":[";
  first = true;
  for (const std::string &id : library.package_ids()) {
    PackageInfo p;
    if (!library.package(id, p)) continue;
    out += first ? "{" : ",{";
    first = false;
    add_string(out, "id", p.id);
    out += ',';
    add_string(out, "name", p.name);
    out += ',';
    add_string(out, "version", p.version);
    out += ',';
    add_string(out, "identity", p.identity);
    out += p.pinned ? ",\"pinned\":true," : ",\"pinned\":false,";
    add_number(out, "bytes", p.app.size() * uint64_t(kSector));
    out += '}';
  }
  out += "]}";
  return out;
}

#endif  // APP_LOADER && APP_WEB_UI
