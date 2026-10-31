// GET /store's body: what the Loader's store holds. Built on the loader task
// (Library isn't thread-safe), then handed to the web server as a string.
#pragma once

#include <stdint.h>

#include <string>

#include "library.h"
#include "store.h"

// Units with their backups and save states, every stored backup, and the
// packages, plus the file system's size (bytes, from LittleFS on the board).
std::string store_json(fm1_loader::Library &library, fm1_loader::Storage &storage, uint64_t fs_total,
                       uint64_t fs_used);
