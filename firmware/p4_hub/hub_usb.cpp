#include "hub_usb.h"

#include <Arduino.h>
#include <string.h>

#include "EspUsbHost.h"
#include "hub_log.h"
#include "json_out.h"

namespace {

EspUsbHost usb;
EspUsbHostConfig config;

constexpr int kMaxDev = 12;
constexpr int kCables = 4;      // counted per cable; robots use 2
constexpr int kMaxKinds = 8;    // per-identity history (vid:pid:serial)

struct Ident {
  uint16_t vid, pid;
  char serial[24];
  char product[32];
};

struct Dev {
  bool used;
  uint8_t address, parent, port;
  bool hub;
  Ident id;
  uint8_t in_cables, out_cables, channels;
  uint32_t connect_ms;
  uint32_t rx[kCables], tx_echo, echo_fail;
  uint32_t rx_other_cable;
};

// Connects/disconnects per identity, across replugs (an address changes each time).
struct Kind {
  bool used;
  Ident id;
  uint32_t connects, disconnects;
  uint32_t last_connect_ms, last_disconnect_ms;
  uint32_t last_remount_ms;   // disconnect -> next connect, P4 side
  uint32_t remount_min, remount_max, remount_sum, remount_n;
};

Dev devs[kMaxDev];
Kind kinds[kMaxKinds];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

struct Event {
  enum : uint8_t { Connected, Disconnected } kind;
  uint8_t address, parent, port;
  bool hub;
  Ident id;
  uint32_t ms;
};
QueueHandle_t events;

struct Echo {
  uint8_t address;
  uint8_t pkt[4];
};
QueueHandle_t echoes;
volatile bool echo_on = false;
volatile HubMidiIn midi_in = nullptr;
volatile bool replug_wanted = false;
uint32_t echo_dropped = 0, midi_unknown_dev = 0, hub_connects = 0, replugs = 0;
uint32_t topology_dirty_ms = 0;
uint8_t channels_used = 0, channels_max = 16;

void copy_ident(Ident &id, const EspUsbHostDeviceInfo &d) {
  id.vid = d.vid;
  id.pid = d.pid;
  strlcpy(id.serial, d.serial ? d.serial : "", sizeof id.serial);
  strlcpy(id.product, d.product ? d.product : "", sizeof id.product);
}

bool same(const Ident &a, const Ident &b) {
  return a.vid == b.vid && a.pid == b.pid && strcmp(a.serial, b.serial) == 0 && strcmp(a.product, b.product) == 0;
}

Kind *kind_for(const Ident &id) {
  for (Kind &k : kinds)
    if (k.used && same(k.id, id)) return &k;
  for (Kind &k : kinds)
    if (!k.used) {
      memset(&k, 0, sizeof k);
      k.used = true;
      k.id = id;
      k.remount_min = UINT32_MAX;
      return &k;
    }
  return nullptr;
}

void on_connected(const EspUsbHostDeviceInfo &d) {
  Event e{};
  e.kind = Event::Connected;
  e.address = d.address;
  e.parent = d.parentAddress;
  e.port = d.portId;
  e.hub = d.isHub;
  copy_ident(e.id, d);
  e.ms = millis();
  xQueueSend(events, &e, 0);
}

void on_disconnected(const EspUsbHostDeviceInfo &d) {
  Event e{};
  e.kind = Event::Disconnected;
  e.address = d.address;
  e.parent = d.parentAddress;
  e.port = d.portId;
  e.hub = d.isHub;
  copy_ident(e.id, d);
  e.ms = millis();
  xQueueSend(events, &e, 0);
}

void on_midi(const EspUsbHostMidiMessage &m) {
  bool found = false;
  portENTER_CRITICAL_SAFE(&mux);
  for (Dev &d : devs) {
    if (!d.used || d.address != m.address) continue;
    found = true;
    if (m.cable < kCables) ++d.rx[m.cable];
    else ++d.rx_other_cable;
    break;
  }
  if (!found) ++midi_unknown_dev;
  portEXIT_CRITICAL_SAFE(&mux);
  if (!m.raw || m.length != 4) return;
  const HubMidiIn fn = midi_in;
  if (fn) fn(m.address, m.raw, millis());
  if (echo_on && !hub_usb_send(m.address, m.raw)) ++echo_dropped;
}

// Sends echoes off the USB client task.
void echo_task(void *) {
  Echo e;
  for (;;) {
    if (xQueueReceive(echoes, &e, portMAX_DELAY) != pdTRUE) continue;
    const bool ok = usb.midiSend(e.pkt, 4, e.address);
    portENTER_CRITICAL_SAFE(&mux);
    for (Dev &d : devs) {
      if (!d.used || d.address != e.address) continue;
      if (ok) ++d.tx_echo; else ++d.echo_fail;
      break;
    }
    portEXIT_CRITICAL_SAFE(&mux);
  }
}

uint8_t hub_address() {
  for (const Dev &d : devs)
    if (d.used && d.hub) return d.address;
  return 0;
}

void handle(const Event &e) {
  if (e.kind == Event::Connected) {
    EspUsbHostMidiPortInfo mp;
    const bool midi = usb.getMidiPortInfo(mp, e.address);
    const uint8_t ch = (uint8_t)usb.estimatedHcdChannelCount(e.address);
    portENTER_CRITICAL_SAFE(&mux);
    Dev *slot = nullptr;
    for (Dev &d : devs)
      if (d.used && d.address == e.address) slot = &d;
    if (!slot)
      for (Dev &d : devs)
        if (!d.used) { slot = &d; break; }
    if (slot) {
      memset(slot, 0, sizeof *slot);
      slot->used = true;
      slot->address = e.address;
      slot->parent = e.parent;
      slot->port = e.port;
      slot->hub = e.hub;
      slot->id = e.id;
      slot->in_cables = midi ? mp.inCableCount : 0;
      slot->out_cables = midi ? mp.outCableCount : 0;
      slot->channels = ch;
      slot->connect_ms = e.ms;
    }
    Kind *k = kind_for(e.id);
    if (k) {
      ++k->connects;
      if (k->last_disconnect_ms && k->disconnects == k->connects - 1) {
        const uint32_t r = e.ms - k->last_disconnect_ms;
        k->last_remount_ms = r;
        if (r < k->remount_min) k->remount_min = r;
        if (r > k->remount_max) k->remount_max = r;
        k->remount_sum += r;
        ++k->remount_n;
      }
      k->last_connect_ms = e.ms;
    }
    if (e.hub) ++hub_connects;
    portEXIT_CRITICAL_SAFE(&mux);
    HLOG_USB("CONNECTED addr=%u parent=%u port=%u %04x:%04x \"%s\" hub=%u midi_in=%u midi_out=%u ch=%u",
             e.address, e.parent, e.port, e.id.vid, e.id.pid, e.id.product, e.hub,
             midi ? mp.inCableCount : 0, midi ? mp.outCableCount : 0, ch);
  } else {
    // The identity stored at connect: a gone device's own strings may be empty.
    Ident id = e.id;
    portENTER_CRITICAL_SAFE(&mux);
    for (Dev &d : devs)
      if (d.used && d.address == e.address) {
        id = d.id;
        d.used = false;
      }
    Kind *k = kind_for(id);
    if (k) {
      ++k->disconnects;
      k->last_disconnect_ms = e.ms;
    }
    portEXIT_CRITICAL_SAFE(&mux);
    HLOG_USB("DISCONNECTED addr=%u %04x:%04x \"%s\" %s", e.address, id.vid, id.pid, id.product, id.serial);
  }
  topology_dirty_ms = millis();
}

void report_topology() {
  channels_used = (uint8_t)usb.estimatedHcdChannelCount();
  channels_max = (uint8_t)usb.maxEndpointChannelCount();
  const HubLogCounters c = hub_log_counters();
  HLOG_USB("TOPOLOGY devices=%u channels=%u/%u short_desc_failed=%lu port_disabled=%lu", hub_usb_device_count(),
           channels_used, channels_max, (unsigned long)c.short_desc_failed, (unsigned long)c.port_disabled);
}

}  // namespace

bool hub_usb_begin() {
  events = xQueueCreate(32, sizeof(Event));
  echoes = xQueueCreate(256, sizeof(Echo));
  xTaskCreatePinnedToCore(echo_task, "echo", 4096, nullptr, 4, nullptr, 0);
  usb.onDeviceConnected(on_connected);
  usb.onDeviceDisconnected(on_disconnected);
  usb.onMidiMessage(on_midi);
  config.port = ESP_USB_HOST_PORT_HIGH_SPEED;
  config.experimentalForceFullSpeed = true;
  config.fifo = ESP_USB_HOST_FIFO_LARGE_PERIODIC_OUT;
  if (!usb.begin(config)) {
    HLOG_USB("HOST_BEGIN_FAILED error=%s", usb.lastErrorName());
    return false;
  }
  channels_max = (uint8_t)usb.maxEndpointChannelCount();
  HLOG_USB("HOST_READY port=hs bus_mode=full_speed_only channels_max=%u", channels_max);
  return true;
}

void hub_usb_loop(uint32_t now_ms) {
  Event e;
  while (xQueueReceive(events, &e, 0) == pdTRUE) handle(e);
  if (topology_dirty_ms && now_ms - topology_dirty_ms >= 3000) {
    topology_dirty_ms = 0;
    report_topology();
  }
  if (replug_wanted) {
    replug_wanted = false;
    ++replugs;
    HLOG_USB("REPLUG %lu end", (unsigned long)replugs);
    usb.end();
    delay(1000);
    const bool ok = usb.begin(config);
    HLOG_USB("REPLUG %lu begin=%s", (unsigned long)replugs, ok ? "ok" : usb.lastErrorName());
  }
}

uint8_t hub_usb_device_count() {
  uint8_t n = 0;
  portENTER_CRITICAL_SAFE(&mux);
  for (const Dev &d : devs) n += d.used;
  portEXIT_CRITICAL_SAFE(&mux);
  return n;
}
uint8_t hub_usb_channels_used() { return channels_used; }
uint8_t hub_usb_channels_max() { return channels_max; }
void hub_usb_set_echo(bool on) { echo_on = on; }
void hub_usb_on_midi(HubMidiIn fn) { midi_in = fn; }
bool hub_usb_send(uint8_t address, const uint8_t pkt[4]) {
  Echo e;
  e.address = address;
  memcpy(e.pkt, pkt, 4);
  return xQueueSend(echoes, &e, 0) == pdTRUE;
}
bool hub_usb_echo() { return echo_on; }
void hub_usb_request_replug() { replug_wanted = true; }

int hub_usb_port_status(uint8_t port, uint16_t &status, uint16_t &change) {
  const uint8_t h = hub_address();
  if (!h) return -1;
  return usb.getHubPortStatus(h, port, status, change, 1000) ? 1 : 0;
}

int hub_usb_port_power(uint8_t port, bool on) {
  const uint8_t h = hub_address();
  if (!h) return -1;
  const bool ok = usb.setHubPortPower(h, port, on);
  HLOG_USB("HUBPORT hub=%u port=%u power=%u ok=%u", h, port, on, ok);
  return ok ? 1 : 0;
}

void hub_usb_reset_counters() {
  portENTER_CRITICAL_SAFE(&mux);
  for (Dev &d : devs) {
    memset(d.rx, 0, sizeof d.rx);
    d.tx_echo = d.echo_fail = d.rx_other_cable = 0;
  }
  for (Kind &k : kinds) {
    if (!k.used) continue;
    k.connects = k.disconnects = 0;
    k.remount_min = UINT32_MAX;
    k.remount_max = k.remount_sum = k.remount_n = k.last_remount_ms = 0;
  }
  echo_dropped = midi_unknown_dev = 0;
  portEXIT_CRITICAL_SAFE(&mux);
}

int hub_usb_lines(char lines[][48], int max) {
  int n = 0;
  portENTER_CRITICAL_SAFE(&mux);
  for (const Dev &d : devs) {
    if (!d.used || n >= max) continue;
    snprintf(lines[n++], 48, "%u %04x:%04x %.14s ch%u%s", d.address, d.id.vid, d.id.pid, d.id.product, d.channels,
             d.hub ? " hub" : "");
  }
  portEXIT_CRITICAL_SAFE(&mux);
  return n;
}

size_t hub_usb_json(char *out, size_t cap) {
  static Dev snap[kMaxDev];
  static Kind ksnap[kMaxKinds];
  uint32_t dropped, unknown, hubc, rp;
  portENTER_CRITICAL_SAFE(&mux);
  memcpy(snap, devs, sizeof snap);
  memcpy(ksnap, kinds, sizeof ksnap);
  dropped = echo_dropped;
  unknown = midi_unknown_dev;
  hubc = hub_connects;
  rp = replugs;
  portEXIT_CRITICAL_SAFE(&mux);
  const HubLogCounters c = hub_log_counters();
  JsonOut o{out, cap};
  o.raw("{\"channels_used\":%u,\"channels_max\":%u,\"echo\":%s,\"echo_dropped\":%lu,\"midi_unknown_dev\":%lu,"
        "\"hub_connects\":%lu,\"replugs\":%lu,\"short_desc_failed\":%lu,\"port_disabled\":%lu,"
        "\"enum_failed_other\":%lu,\"devices\":[",
        (unsigned)channels_used, (unsigned)channels_max, echo_on ? "true" : "false",
        (unsigned long)dropped, (unsigned long)unknown, (unsigned long)hubc, (unsigned long)rp,
        (unsigned long)c.short_desc_failed, (unsigned long)c.port_disabled, (unsigned long)c.enum_failed_other);
  bool first = true;
  const uint32_t now = millis();
  for (const Dev &d : snap) {
    if (!d.used) continue;
    o.raw(first ? "{" : ",{");
    first = false;
    o.raw("\"address\":%u,\"parent\":%u,\"port\":%u,\"hub\":%s,\"vid\":\"%04x\",\"pid\":\"%04x\",\"product\":", d.address,
          d.parent, d.port, d.hub ? "true" : "false", d.id.vid, d.id.pid);
    o.str(d.id.product);
    o.raw(",\"serial\":");
    o.str(d.id.serial);
    o.raw(",\"midi_in_cables\":%u,\"midi_out_cables\":%u,\"channels\":%u,\"up_ms\":%lu,\"rx\":[", d.in_cables,
          d.out_cables, d.channels, (unsigned long)(now - d.connect_ms));
    for (int i = 0; i < kCables; ++i) o.raw("%s%lu", i ? "," : "", (unsigned long)d.rx[i]);
    o.raw("],\"rx_other_cable\":%lu,\"echoed\":%lu,\"echo_fail\":%lu}", (unsigned long)d.rx_other_cable,
          (unsigned long)d.tx_echo, (unsigned long)d.echo_fail);
  }
  o.raw("],\"identities\":[");
  first = true;
  for (const Kind &k : ksnap) {
    if (!k.used) continue;
    o.raw(first ? "{" : ",{");
    first = false;
    o.raw("\"vid\":\"%04x\",\"pid\":\"%04x\",\"product\":", k.id.vid, k.id.pid);
    o.str(k.id.product);
    o.raw(",\"serial\":");
    o.str(k.id.serial);
    o.raw(",\"connects\":%lu,\"disconnects\":%lu,\"remount_n\":%lu,\"remount_ms_last\":%lu,\"remount_ms_min\":%lu,"
          "\"remount_ms_max\":%lu,\"remount_ms_mean\":%lu}",
          (unsigned long)k.connects, (unsigned long)k.disconnects, (unsigned long)k.remount_n,
          (unsigned long)k.last_remount_ms, (unsigned long)(k.remount_n ? k.remount_min : 0),
          (unsigned long)k.remount_max, (unsigned long)(k.remount_n ? k.remount_sum / k.remount_n : 0));
  }
  o.raw("]}");
  return o.len;
}
