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
// Outrigger Synth (Makefile DEV_APP_SETS): Felucca out of Big-S3's speaker.
// A development build, never on the installer site.
#ifndef APP_SYNTH
#define APP_SYNTH 0
#endif

// Short label for the settings page and the build tag (build_tag.h), next to
// the firmware commit. A string literal, so the tag can hold it as an array.
#if APP_SYNTH
#define APP_SET_LABEL "synth"
#elif APP_LOADER && APP_RECORDER && APP_WEB_UI
#define APP_SET_LABEL "all"
#elif APP_LOADER && !APP_RECORDER && !APP_WEB_UI
#define APP_SET_LABEL "hub+L"
#elif APP_LOADER && !APP_RECORDER && APP_WEB_UI
#define APP_SET_LABEL "hub+LW"
#elif !APP_LOADER && !APP_RECORDER && APP_WEB_UI
#define APP_SET_LABEL "hub+W"
#elif !APP_LOADER && !APP_RECORDER && !APP_WEB_UI
#define APP_SET_LABEL "hub"
#else
#define APP_SET_LABEL "custom"
#endif

namespace platform {

constexpr bool kHasLoader = APP_LOADER;
constexpr bool kHasRecorder = APP_RECORDER;
constexpr bool kHasWebUi = APP_WEB_UI;
constexpr bool kHasSynth = APP_SYNTH;

constexpr const char *kAppSetLabel = APP_SET_LABEL;

}  // namespace platform
