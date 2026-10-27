// Which apps this build carries. The Makefile's APPS= picks an app set and
// passes its flags (docs/architecture.md, "App sets and builds"); the Router is
// always built.
#pragma once

#ifndef APP_LOADER
#define APP_LOADER 0
#endif
#ifndef APP_RECORDER
#define APP_RECORDER 0
#endif
#ifndef APP_WEB_UI
#define APP_WEB_UI 0
#endif

namespace platform {

constexpr bool kHasLoader = APP_LOADER;
constexpr bool kHasRecorder = APP_RECORDER;
constexpr bool kHasWebUi = APP_WEB_UI;

// Short label for the settings page, next to the firmware commit.
constexpr const char *kAppSetLabel = kHasLoader && kHasRecorder && kHasWebUi ? "all"
                                     : kHasLoader && !kHasRecorder && !kHasWebUi ? "hub+L"
                                     : kHasLoader && !kHasRecorder && kHasWebUi ? "hub+LW"
                                     : !kHasLoader && !kHasRecorder && !kHasWebUi ? "hub"
                                                                                  : "custom";

}  // namespace platform
