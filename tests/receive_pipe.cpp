// fm1_loader's Receiver on stdin and stdout, over a store in a directory: the
// board for tests/installer/add.test.mjs, so the page's side is tested against
// the same code the board runs.
//
// usage: receive_pipe store_dir free_bytes [junk]
// junk: write a log line before every reply, as the board's console may.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "library.h"
#include "posix_storage.h"
#include "receive.h"

using namespace fm1_loader;

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: receive_pipe store_dir free_bytes [junk]\n");
    return 2;
  }
  PosixStorage storage(argv[1]);
  Library lib(storage);
  const uint64_t free_bytes = strtoull(argv[2], nullptr, 10);
  const bool junk = argc > 3 && strcmp(argv[3], "junk") == 0;
  Receiver rx(lib, [&] { return free_bytes; }, "test 1234567");
  uint8_t in[4096];
  std::vector<uint8_t> out;
  for (;;) {
    const ssize_t n = read(0, in, sizeof in);
    if (n <= 0) return 0;
    out.clear();
    rx.feed(in, size_t(n), 0, out);
    if (out.empty()) continue;
    if (junk) {
      static const char line[] = "I (1234) littlefs: OR\n";
      if (write(1, line, sizeof line - 1) < 0) return 1;
    }
    if (write(1, out.data(), out.size()) < 0) return 1;
  }
}
