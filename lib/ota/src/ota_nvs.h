// The update record in NVS, for the board only (Arduino's Preferences): one
// blob, "ota"/"rec", read the same way by Outrigger (update_app.cpp) and the
// recovery app (firmware/recovery). The recovery app's log is "ota"/"log".
#pragma once

#include <Preferences.h>

#include "ota.h"

namespace ota {

constexpr const char *kNvsNamespace = "ota";
constexpr const char *kNvsRecord = "rec";
constexpr const char *kNvsLog = "log";
constexpr size_t kLogBytes = 640;  // the recovery app's newest lines

// prefs: begun on kNvsNamespace.
inline bool nvs_load(Preferences &prefs, Record &out) {
  uint8_t blob[kRecordBytes];
  return prefs.getBytesLength(kNvsRecord) == sizeof blob && prefs.getBytes(kNvsRecord, blob, sizeof blob) == sizeof blob &&
         decode(blob, sizeof blob, out);
}

inline bool nvs_save(Preferences &prefs, const Record &r) {
  uint8_t blob[kRecordBytes];
  encode(r, blob);
  return prefs.putBytes(kNvsRecord, blob, sizeof blob) == sizeof blob;
}

}  // namespace ota
