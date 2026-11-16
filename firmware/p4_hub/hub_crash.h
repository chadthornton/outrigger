// The crash record behind GET /crash.
//
// On the S3 boards the breadcrumbs live in RTC memory, which keeps them across
// every reset but a power-on. On Big-P4 every UART-C open and every flash is a
// power-on-class reset, so RTC alone would lose a crash the moment anyone looks.
// So: the panic handler and the log tail write RTC memory (fast, safe in a
// panic), and the first boot after an abnormal reset copies that record into
// NVS (namespace `crash`), where it stays until the next crash replaces it.
// ESP-IDF's own core dump (flash, ELF) is summarised too; GET /coredump has it whole.
#pragma once
#include <stddef.h>
#include <stdint.h>

void hub_crash_begin();  // call first in setup(): saves a pending record to NVS
// From hub_log: the newest lines, kept in RTC memory.
void hub_crash_tail(char source, uint32_t ms, const char *text);
size_t hub_crash_json(char *out, size_t cap);
const char *hub_reset_name();
uint32_t hub_boot_count();
