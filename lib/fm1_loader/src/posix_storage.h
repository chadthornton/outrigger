// Storage on a file system through POSIX calls: on the Waveshare, LittleFS
// mounted at /store through ESP-IDF's VFS; on the Mac, a temp directory, so
// the same code is tested there.
//
// A key is a relative path ("m/unit/0123…", "s/ab/cdef…"). A write goes to a
// temp file named "~…" in the same directory, is fsync'd, then renamed over the
// old file, so a reader sees the old value or the new one, never a torn one.
// Temp files are never listed. Each write is durable when it returns, so
// sync() has nothing left to do.
//
// LittleFS names are at most 64 bytes per path component
// (CONFIG_LITTLEFS_OBJ_NAME_LEN); keys longer than that are refused.
#pragma once

#include <string>

#include "store.h"

namespace fm1_loader {

class PosixStorage : public Storage {
 public:
  explicit PosixStorage(const std::string &root) : root_(root) {}
  bool read(const std::string &key, std::vector<uint8_t> &out) override;
  bool write(const std::string &key, const uint8_t *data, size_t len) override;
  bool remove(const std::string &key) override;
  bool exists(const std::string &key) override;
  std::vector<std::string> list(const std::string &prefix) override;
  bool sync() override { return true; }

  static constexpr size_t kMaxComponent = 63;

 private:
  bool path_of(const std::string &key, std::string &path) const;
  void walk(const std::string &rel, const std::string &prefix, std::vector<std::string> &out);
  std::string root_;
};

}  // namespace fm1_loader
