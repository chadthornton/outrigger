// A PosixStorage in a fresh temp directory, removed afterwards.
#pragma once

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "posix_storage.h"

inline std::string make_temp_dir() {
  char tmpl[] = "/tmp/fm1_store_XXXXXX";
  const char *d = mkdtemp(tmpl);
  return d ? d : "/tmp/fm1_store_failed";
}

struct TempPosixStorage : fm1_loader::PosixStorage {
  std::string dir;
  TempPosixStorage() : TempPosixStorage(make_temp_dir()) {}
  explicit TempPosixStorage(const std::string &d) : PosixStorage(d), dir(d) {}
  ~TempPosixStorage() override { system(("rm -rf '" + dir + "'").c_str()); }
};
