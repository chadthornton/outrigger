#include <Arduino.h>
// Outrigger Synth, P4 proposal steps 0 and 1: a throwaway USB probe for the
// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5. Not Outrigger's app.
//
// Wiring: the board's UART USB-C to the Mac (flash + logs, CH343P bridge); a
// powered hub on the USB OTG USB-C, with a MIDI controller and a C-Media
// CM108/CM119 adapter behind it. The HS port runs forced full speed so the
// hub's full-speed devices need no transaction translator.
//
// Build at DebugLevel=info, never verbose: EspUsbHost found verbose logging
// crashes the console on the P4 while USB host runs.
//
// Serial commands (115200): t = start 440 Hz tone (2 ch, 16 bit, 44.1 kHz),
// s = stop tone, r = print the topology report now, p = one software replug,
// c = ten software replugs. A software replug stops and restarts the USB host
// (usb.end() / usb.begin()), so the hub and everything behind it disconnect and
// re-enumerate and forced full speed is set again, then checks that the
// settled topology matches the one before the first cycle. Toggling root port
// power alone does not work: the port refuses to power back on
// (ESP_ERR_INVALID_STATE) while the hub's handle is still held.
//
// Log lines are prefixed so docs/p4-usb-probe.md can quote them: CONNECTED,
// DISCONNECTED, DEVICE, AUDIO_STREAM, CHANNELS, NOTE, PROBE, TONE.

#include <math.h>
#include "EspUsbHost.h"
#include "soc/usb_dwc_struct.h"

EspUsbHost usb;
static EspUsbHostConfig hostConfig;

static constexpr uint32_t REPORT_INTERVAL_MS = 3000;
static constexpr uint32_t TONE_STATS_INTERVAL_MS = 10000;
static constexpr uint32_t TONE_RATE = 44100;
static constexpr float TONE_HZ = 440.0f;
static constexpr int16_t TONE_AMPLITUDE = 6000;  // about -15 dBFS, kind to ears

// MIDI and connect events arrive on the USB client task; they go through a
// queue so printing never blocks USB.
struct ProbeEvent
{
  enum Kind : uint8_t { Connected, Disconnected, Midi } kind;
  EspUsbHostDeviceInfo device;  // Connected / Disconnected
  uint8_t address, status, data1, data2;  // Midi
};
static QueueHandle_t events;

static volatile bool topologyDirty = true;
static uint32_t hubConnects = 0;

// 440 Hz phase accumulator, advanced only on the USB task.
static float tonePhase = 0.0f;
static volatile uint32_t toneFramesWritten = 0;
static bool toneRunning = false;
static uint32_t toneStartMs = 0;
static uint32_t toneLastUnderruns = 0;
static void stopTone();

static const char *speedName(usb_speed_t speed)
{
  switch (speed)
  {
  case USB_SPEED_LOW:
    return "low";
  case USB_SPEED_FULL:
    return "full";
  case USB_SPEED_HIGH:
    return "high";
  default:
    return "unknown";
  }
}

static void printDevice(const char *prefix, const EspUsbHostDeviceInfo &d)
{
  Serial.printf("%s addr=%u parent=%u port=0x%02x speed=%s hub=%u class=0x%02x protocol=0x%02x "
                "vid:pid=%04x:%04x usb=%x.%02x product=\"%s\" maker=\"%s\"\n",
                prefix, d.address, d.parentAddress, d.portId, speedName(d.speed), d.isHub ? 1 : 0,
                d.deviceClass, d.deviceProtocol, d.vid, d.pid, d.usbVersion >> 8, d.usbVersion & 0xff,
                d.product, d.manufacturer);
}

static void printAudioStreams(uint8_t address, bool &out44kStereo)
{
  EspUsbHostAudioStreamInfo streams[ESP_USB_HOST_MAX_AUDIO_STREAMS];
  const size_t count = usb.getAudioStreams(address, streams, ESP_USB_HOST_MAX_AUDIO_STREAMS);
  for (size_t i = 0; i < count; i++)
  {
    const EspUsbHostAudioStreamInfo &s = streams[i];
    Serial.printf("AUDIO_STREAM addr=%u if=%u alt=%u ep=0x%02x dir=%s uac=%u ch=%u bits=%u mps=%u startable=%u rates=",
                  address, s.interfaceNumber, s.alternate, s.endpointAddress, s.output ? "out" : "in",
                  s.protocol == ESP_USB_HOST_AUDIO_PROTOCOL_UAC2 ? 2 : 1, s.channels, s.bitsPerSample,
                  s.maxPacketSize, s.startable ? 1 : 0);
    bool has44k = false;
    if (s.sampleRateCount > 0)
    {
      for (uint8_t r = 0; r < s.sampleRateCount; r++)
      {
        Serial.printf("%s%lu", r ? "," : "", static_cast<unsigned long>(s.sampleRates[r]));
        has44k |= s.sampleRates[r] == TONE_RATE;
      }
    }
    else
    {
      Serial.printf("%lu-%lu", static_cast<unsigned long>(s.sampleRateMin),
                    static_cast<unsigned long>(s.sampleRateMax));
      has44k = s.sampleRateMin <= TONE_RATE && TONE_RATE <= s.sampleRateMax;
    }
    Serial.println();
    if (s.output && s.startable && s.channels == 2 && s.bitsPerSample == 16 && has44k)
    {
      out44kStereo = true;
    }
  }
}

// The last settled topology in one comparable line, for the replug cycles.
static char topologySig[160] = "";

static void reportTopology()
{
  EspUsbHostDeviceInfo devices[ESP_USB_HOST_MAX_DEVICES];
  const size_t count = usb.getDevices(devices, ESP_USB_HOST_MAX_DEVICES);
  bool hubFull = false, hubHigh = false, downstream = false, midi = false, out44k = false;

  Serial.printf("TOPOLOGY devices=%u hub_connects=%lu\n", static_cast<unsigned>(count),
                static_cast<unsigned long>(hubConnects));
  for (size_t i = 0; i < count; i++)
  {
    const EspUsbHostDeviceInfo &d = devices[i];
    printDevice("DEVICE", d);
    Serial.printf("CHANNELS addr=%u ep0=%u endpoints=%u hub_endpoints=%u total=%u\n", d.address,
                  static_cast<unsigned>(usb.ep0ChannelCount(d.address)),
                  static_cast<unsigned>(usb.endpointChannelCount(d.address)),
                  static_cast<unsigned>(usb.hubEndpointChannelCount(d.address)),
                  static_cast<unsigned>(usb.estimatedHcdChannelCount(d.address)));
    if (d.isHub)
    {
      hubFull |= d.speed == USB_SPEED_FULL;
      hubHigh |= d.speed == USB_SPEED_HIGH;
    }
    else if (d.parentAddress != 0 && d.speed != USB_SPEED_HIGH)
    {
      downstream = true;
    }
    midi |= usb.midiReady(d.address);
    printAudioStreams(d.address, out44k);
  }
  Serial.printf("CHANNELS used=%u of=%u\n", static_cast<unsigned>(usb.estimatedHcdChannelCount()),
                static_cast<unsigned>(usb.maxEndpointChannelCount()));
  Serial.printf("PROBE hub_full_speed=%u hub_high_speed=%u downstream_fs=%u midi_ready=%u audio_out_44k1_stereo=%u\n",
                hubFull, hubHigh, downstream, midi, out44k);
  snprintf(topologySig, sizeof(topologySig), "devices=%u channels=%u hub_fs=%u down=%u midi=%u out44k=%u",
           static_cast<unsigned>(count), static_cast<unsigned>(usb.estimatedHcdChannelCount()), hubFull,
           downstream, midi, out44k);
}

// Software replug cycles: end() -> (1 s) begin() -> (8 s settle) compare -> next.
static constexpr uint32_t CYCLE_OFF_MS = 1000;
static constexpr uint32_t CYCLE_SETTLE_MS = 8000;
static enum { CycleIdle, CycleOff, CycleOn } cycleState = CycleIdle;
static uint32_t cycleAtMs = 0, cycleIndex = 0, cycleTotal = 0, cycleSame = 0;
static char cycleBaseline[160] = "";
static bool cycleScoring = false;

static void startCycles(uint32_t total)
{
  if (cycleState != CycleIdle)
  {
    return;
  }
  reportTopology();
  strncpy(cycleBaseline, topologySig, sizeof(cycleBaseline));
  Serial.printf("CYCLE baseline %s\n", cycleBaseline);
  cycleIndex = cycleSame = 0;
  cycleTotal = total;
  cycleScoring = false;
  cycleState = CycleOn;
  cycleAtMs = millis() - CYCLE_SETTLE_MS;  // start the first power-off now
}

static void runCycles()
{
  if (cycleState == CycleIdle || millis() - cycleAtMs < (cycleState == CycleOff ? CYCLE_OFF_MS : CYCLE_SETTLE_MS))
  {
    return;
  }
  cycleAtMs = millis();
  if (cycleState == CycleOff)
  {
    const bool ok = usb.begin(hostConfig);
    Serial.printf("CYCLE %lu/%lu begin=%s HCFG=0x%08lx FSLSSUPP=%u\n",
                  static_cast<unsigned long>(cycleIndex + 1), static_cast<unsigned long>(cycleTotal),
                  ok ? "ok" : usb.lastErrorName(), static_cast<unsigned long>(USB_DWC_HS.hcfg_reg.val),
                  static_cast<unsigned>(USB_DWC_HS.hcfg_reg.fslssupp));
    cycleState = CycleOn;
    return;
  }
  // CycleOn and settled: score the cycle that just finished (none before the first).
  if (cycleScoring)
  {
    reportTopology();
    const bool same = strcmp(topologySig, cycleBaseline) == 0;
    cycleSame += same;
    Serial.printf("CYCLE %lu/%lu result=%s %s\n", static_cast<unsigned long>(cycleIndex + 1),
                  static_cast<unsigned long>(cycleTotal), same ? "same" : "DIFF", topologySig);
    cycleIndex++;
    if (cycleIndex >= cycleTotal)
    {
      Serial.printf("CYCLE done same=%lu of=%lu\n", static_cast<unsigned long>(cycleSame),
                    static_cast<unsigned long>(cycleTotal));
      cycleState = CycleIdle;
      return;
    }
  }
  cycleScoring = true;
  if (toneRunning)
  {
    stopTone();
  }
  usb.end();
  Serial.printf("CYCLE %lu/%lu end\n", static_cast<unsigned long>(cycleIndex + 1),
                static_cast<unsigned long>(cycleTotal));
  cycleState = CycleOff;
}

static void fillTone(EspUsbHostAudioOutputRequest &req)
{
  if (req.bytesPerSample != 2 || req.channels == 0)
  {
    return;  // only the 16-bit format this probe starts
  }
  int16_t *pcm = reinterpret_cast<int16_t *>(req.data);
  const float step = 2.0f * static_cast<float>(M_PI) * TONE_HZ / static_cast<float>(req.sampleRate);
  for (size_t f = 0; f < req.frameCount; f++)
  {
    const int16_t v = static_cast<int16_t>(sinf(tonePhase) * TONE_AMPLITUDE);
    for (uint8_t c = 0; c < req.channels; c++)
    {
      *pcm++ = v;
    }
    tonePhase += step;
    if (tonePhase >= 2.0f * static_cast<float>(M_PI))
    {
      tonePhase -= 2.0f * static_cast<float>(M_PI);
    }
  }
  req.writtenFrames = req.frameCount;
  toneFramesWritten += req.frameCount;
}

static void startTone()
{
  if (!usb.audioOutputReady())
  {
    Serial.println("TONE start=fail reason=no_audio_output");
    return;
  }
  tonePhase = 0.0f;
  toneFramesWritten = 0;
  if (!usb.audioOutputStart(2, 16, TONE_RATE))
  {
    Serial.printf("TONE start=fail error=%s\n", usb.lastErrorName());
    return;
  }
  usb.audioConfigureVolume(-12.0f);
  toneRunning = true;
  toneStartMs = millis();
  toneLastUnderruns = usb.audioOutputUnderruns();
  Serial.printf("TONE start=ok hz=440 rate=%lu feedback=%u\n", static_cast<unsigned long>(TONE_RATE),
                usb.audioOutputHasFeedback() ? 1 : 0);
}

static void stopTone()
{
  usb.audioOutputStop();
  toneRunning = false;
  Serial.println("TONE stopped");
}

static void reportTone()
{
  const uint32_t underruns = usb.audioOutputUnderruns();
  const uint32_t elapsed = (millis() - toneStartMs) / 1000;
  Serial.printf("TONE t=%lus running=%u frames=%lu underruns=%lu (+%lu) pace_rate=%lu fb_updates=%lu fb_rejects=%lu\n",
                static_cast<unsigned long>(elapsed), usb.audioOutputRunning() ? 1 : 0,
                static_cast<unsigned long>(toneFramesWritten), static_cast<unsigned long>(underruns),
                static_cast<unsigned long>(underruns - toneLastUnderruns),
                static_cast<unsigned long>(usb.audioOutputRate()),
                static_cast<unsigned long>(usb.audioOutputFeedbackUpdates()),
                static_cast<unsigned long>(usb.audioOutputFeedbackRejects()));
  toneLastUnderruns = underruns;
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("TEST_BEGIN p4_usb_probe");
  events = xQueueCreate(64, sizeof(ProbeEvent));

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &d) {
    ProbeEvent e = {ProbeEvent::Connected, d, 0, 0, 0, 0};
    xQueueSend(events, &e, 0);
  });
  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &d) {
    ProbeEvent e = {ProbeEvent::Disconnected, d, 0, 0, 0, 0};
    xQueueSend(events, &e, 0);
  });
  usb.onMidiMessage([](const EspUsbHostMidiMessage &m) {
    const uint8_t type = m.status & 0xf0;
    if (type != 0x80 && type != 0x90)
    {
      return;  // notes only; knobs and clock would flood the log
    }
    ProbeEvent e = {ProbeEvent::Midi, {}, m.address, m.status, m.data1, m.data2};
    xQueueSend(events, &e, 0);
  });
  usb.onAudioOutputRequest(fillTone);

  EspUsbHostConfig &config = hostConfig;
  config.port = ESP_USB_HOST_PORT_HIGH_SPEED;
  config.experimentalForceFullSpeed = true;
  // The default periodic-OUT FIFO caps an isochronous OUT endpoint at 512
  // bytes; the iFi USB Audio SE's is 776 and failed to claim without this.
  config.fifo = ESP_USB_HOST_FIFO_LARGE_PERIODIC_OUT;
  if (!usb.begin(config))
  {
    Serial.printf("HOST_BEGIN_FAILED error=%s\n", usb.lastErrorName());
    return;
  }
  Serial.println("HOST_READY port=hs bus_mode=full_speed_only");
}

void loop()
{
  ProbeEvent e;
  while (xQueueReceive(events, &e, 0) == pdTRUE)
  {
    switch (e.kind)
    {
    case ProbeEvent::Connected:
      printDevice("CONNECTED", e.device);
      if (e.device.isHub)
      {
        hubConnects++;
      }
      topologyDirty = true;
      break;
    case ProbeEvent::Disconnected:
      printDevice("DISCONNECTED", e.device);
      if (toneRunning && !usb.audioOutputReady())
      {
        toneRunning = false;
        Serial.println("TONE stopped reason=device_gone");
      }
      topologyDirty = true;
      break;
    case ProbeEvent::Midi:
    {
      const bool on = (e.status & 0xf0) == 0x90 && e.data2 > 0;
      Serial.printf("NOTE addr=%u ch=%u %s note=%u vel=%u\n", e.address, (e.status & 0x0f) + 1,
                    on ? "on" : "off", e.data1, e.data2);
      break;
    }
    }
  }

  // Report a settled topology: wait one interval after the last change so a
  // hub's downstream devices have enumerated.
  static uint32_t lastChangeMs = 0;
  if (topologyDirty)
  {
    topologyDirty = false;
    lastChangeMs = millis();
  }
  if (lastChangeMs && millis() - lastChangeMs >= REPORT_INTERVAL_MS)
  {
    lastChangeMs = 0;
    reportTopology();
  }

  runCycles();

  static uint32_t lastToneMs = 0;
  if (toneRunning && millis() - lastToneMs >= TONE_STATS_INTERVAL_MS)
  {
    lastToneMs = millis();
    reportTone();
  }

  while (Serial.available())
  {
    switch (Serial.read())
    {
    case 't':
      startTone();
      break;
    case 's':
      stopTone();
      break;
    case 'r':
      reportTopology();
      break;
    case 'p':
      startCycles(1);
      break;
    case 'c':
      startCycles(10);
      break;
    }
  }
  delay(5);
}
