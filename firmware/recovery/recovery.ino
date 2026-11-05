// The recovery app (docs/ota-updates.md). Its code is in recovery_app.cpp: the
// Arduino IDE rewrites .ino files (it inserts prototypes where ctags guesses the
// first function starts), and on this file that guess once landed inside
// setup(), as calls to setup() and loop(). It recursed until the stack ran out
// on every boot (Small-V1, 2026-11-05). .cpp files are compiled as written.
