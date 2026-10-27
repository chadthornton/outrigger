#include "describe.h"

namespace platform {

namespace {
constexpr uint8_t kInterfaceDescriptor = 4;
}

bool describe_device(uint8_t address, uint16_t vid, uint16_t pid, bool is_hub, const char tag[4],
                     const uint8_t *config, size_t length, DeviceDesc &out) {
  out = DeviceDesc{};
  out.address = address;
  out.vid = vid;
  out.pid = pid;
  out.is_hub = is_hub;
  for (size_t i = 0; i < sizeof out.tag - 1 && tag && tag[i]; ++i) out.tag[i] = tag[i];
  if (!config) return false;

  for (size_t pos = 0; pos < length;) {
    const uint8_t len = config[pos];
    if (len < 2 || pos + len > length) return false;
    if (config[pos + 1] == kInterfaceDescriptor && len >= 9) {
      const uint8_t number = config[pos + 2], endpoints = config[pos + 4];
      InterfaceDesc *found = nullptr;
      for (uint8_t k = 0; k < out.interface_count; ++k)
        if (out.interfaces[k].number == number) found = &out.interfaces[k];
      if (found) {
        if (endpoints > found->endpoints) found->endpoints = endpoints;
      } else if (out.interface_count < kMaxInterfaces) {
        InterfaceDesc &d = out.interfaces[out.interface_count++];
        d.number = number;
        d.endpoints = endpoints;
        d.cls = config[pos + 5];
        d.subclass = config[pos + 6];
        d.protocol = config[pos + 7];
      }
    }
    pos += len;
  }
  return true;
}

}  // namespace platform
