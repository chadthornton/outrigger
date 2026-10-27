#include "posix_storage.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace fm1_loader {

namespace {

// Each component: 1..kMaxComponent bytes of [A-Za-z0-9._-], not "." or "..",
// not starting with '~' (temp files).
bool valid_component(const std::string &c) {
  if (c.empty() || c.size() > PosixStorage::kMaxComponent || c == "." || c == ".." || c[0] == '~')
    return false;
  for (char ch : c)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
          ch == '.' || ch == '_' || ch == '-'))
      return false;
  return true;
}

bool make_dirs(const std::string &dir) {
  struct stat st;
  if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  size_t slash = dir.rfind('/');
  if (slash != std::string::npos && slash > 0 && !make_dirs(dir.substr(0, slash))) return false;
  return mkdir(dir.c_str(), 0755) == 0 || (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

}  // namespace

bool PosixStorage::path_of(const std::string &key, std::string &path) const {
  size_t start = 0;
  while (true) {
    size_t slash = key.find('/', start);
    std::string c = key.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!valid_component(c)) return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  path = root_ + "/" + key;
  return true;
}

bool PosixStorage::read(const std::string &key, std::vector<uint8_t> &out) {
  std::string path;
  if (!path_of(key, path)) return false;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  bool ok = !ferror(f);
  fclose(f);
  return ok;
}

bool PosixStorage::write(const std::string &key, const uint8_t *data, size_t len) {
  std::string path;
  if (!path_of(key, path)) return false;
  size_t slash = path.rfind('/');
  const std::string dir = path.substr(0, slash), name = path.substr(slash + 1);
  if (!make_dirs(dir)) return false;
  // "~" plus the name, cut to fit the component limit: one temp per target.
  const std::string tmp = dir + "/~" + name.substr(0, kMaxComponent - 1);
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = (len == 0 || fwrite(data, 1, len, f) == len) && fflush(f) == 0 && fsync(fileno(f)) == 0;
  ok = fclose(f) == 0 && ok;
  if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) ::remove(tmp.c_str());
  return ok;
}

bool PosixStorage::remove(const std::string &key) {
  std::string path;
  return path_of(key, path) && ::remove(path.c_str()) == 0;
}

bool PosixStorage::exists(const std::string &key) {
  std::string path;
  struct stat st;
  return path_of(key, path) && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void PosixStorage::walk(const std::string &rel, const std::string &prefix,
                        std::vector<std::string> &out) {
  const std::string dir = rel.empty() ? root_ : root_ + "/" + rel;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (struct dirent *e = readdir(d)) names.push_back(e->d_name);
  closedir(d);
  for (const std::string &n : names) {
    if (!valid_component(n)) continue;  // ".", "..", temp files
    const std::string key = rel.empty() ? n : rel + "/" + n;
    // Only descend where keys could still start with prefix.
    const size_t m = std::min(key.size(), prefix.size());
    if (key.compare(0, m, prefix, 0, m) != 0) continue;
    struct stat st;
    if (stat((root_ + "/" + key).c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) walk(key, prefix, out);
    else if (S_ISREG(st.st_mode) && key.compare(0, prefix.size(), prefix) == 0) out.push_back(key);
  }
}

std::vector<std::string> PosixStorage::list(const std::string &prefix) {
  std::vector<std::string> out;
  walk("", prefix, out);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace fm1_loader
