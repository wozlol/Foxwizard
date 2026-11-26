#pragma once

// USB MIDI host on the ESP32-S3's own USB port, on top of ESP-IDF's USB Host Library, which the Arduino core ships with hub support on
// (CONFIG_USB_HOST_HUBS_SUPPORTED and CONFIG_USB_HOST_HUB_MULTI_LEVEL), so a device on a hub works as far as the chip's 8 host
// channels go. Each endpoint of each device takes one channel and a hub takes some, so the module takes 2 MIDI devices (USB_HOSTS_MAX in
// board_config.h), through a hub or one straight in.
//
// The USB port is either this host or a USB MIDI device (TinyUSB, started by the sketch), never both. The sketch decides, at start,
// which one the port is.
//
// Every USB Host Library call is made from this file's client task. The rest of the sketch only sets flags and passes bytes through
// two rings, so nothing here is called from two tasks at once.
//
// Not run on hardware yet. Written from the IDF 5.5 headers in the core.

#include <Arduino.h>
#include <string.h>

#include "usb/usb_host.h"

#include "board_config.h"
#include "foxwizard_link.h"

namespace usbmidi {

constexpr uint8_t kMaxDevices = USB_HOSTS_MAX;
constexpr uint16_t kOutRing = 256; // bytes of USB MIDI packets waiting to go to a device

enum State : uint8_t {
  kFree = 0,  // nothing here
  kOpened,    // a MIDI device is plugged in and described, not in use
  kActive,    // its interface is claimed and the transfers run
  kStopping   // being let go, waiting for its transfers to end
};

typedef void (*RxFn)(uint8_t index, const uint8_t *data, size_t length);

struct Device {
  volatile uint8_t state = kFree;
  volatile bool ignore = false;         // sent away or unusable, left alone until it is unplugged
  volatile bool activateWanted = false; // set by the sketch, acted on by the client task
  volatile bool releaseWanted = false;
  bool gone = false;
  bool claimed = false;
  usb_device_handle_t dev = nullptr;
  uint8_t ifnum = 0;
  uint8_t epIn = 0, epOut = 0;
  uint16_t mpsIn = 0, mpsOut = 0;
  usb_transfer_t *in = nullptr;
  usb_transfer_t *out = nullptr;
  volatile bool inPending = false;
  volatile bool outPending = false;
  uint8_t errors = 0;
  uint16_t vid = 0, pid = 0, serialHash = 0;
  char name[NAME_CHARS + 1] = {0};
  uint8_t outRing[kOutRing];
  volatile uint16_t outHead = 0;
  volatile uint16_t outTail = 0;
};

static Device devs[kMaxDevices];
static usb_host_client_handle_t client = nullptr;
static RxFn rxSink = nullptr;
static volatile bool started = false;

// ---------------------------------------------------------------------------------------------------------------------------
// Used by the sketch
// ---------------------------------------------------------------------------------------------------------------------------
inline uint8_t state(uint8_t i) { return devs[i].state; }
inline bool pending(uint8_t i) { return devs[i].activateWanted; }
inline bool ignored(uint8_t i) { return devs[i].ignore; }
inline const Device &device(uint8_t i) { return devs[i]; }

inline void wake() { if (client) usb_host_client_unblock(client); }

// Starts using a device the sketch has room for. It is active when state() says so.
inline void activate(uint8_t i) { devs[i].activateWanted = true; wake(); }

// Stops using it and leaves it alone until it is unplugged.
inline void release(uint8_t i) { devs[i].ignore = true; devs[i].releaseWanted = true; wake(); }

// USB MIDI 1.0 packets, 4 bytes each, for the device. False when its ring is full.
inline bool send(uint8_t i, const uint8_t *packets, uint16_t length) {
  Device &d = devs[i];
  if (d.state != kActive || !d.epOut) return false;
  const uint16_t used = (uint16_t)((d.outHead + kOutRing - d.outTail) % kOutRing);
  if (kOutRing - 1 - used < length) return false;
  uint16_t head = d.outHead;
  for (uint16_t k = 0; k < length; ++k) { d.outRing[head] = packets[k]; head = (uint16_t)((head + 1) % kOutRing); }
  __sync_synchronize();
  d.outHead = head;
  wake();
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The client task
// ---------------------------------------------------------------------------------------------------------------------------
static uint8_t indexOf(const Device *d) { return (uint8_t)(d - devs); }

static void stopDevice(Device &d, bool gone) {
  d.gone = d.gone || gone;
  if (d.state == kStopping) return;
  d.state = kStopping;
  d.activateWanted = false;
  if (d.inPending) { usb_host_endpoint_halt(d.dev, d.epIn); usb_host_endpoint_flush(d.dev, d.epIn); }
  if (d.outPending) { usb_host_endpoint_halt(d.dev, d.epOut); usb_host_endpoint_flush(d.dev, d.epOut); }
}

// Called when a transfer ends and every pass of the task: once nothing is in flight the device's resources go back.
static void finishStopping(Device &d) {
  if (d.state != kStopping || d.inPending || d.outPending) return;
  if (d.in) { usb_host_transfer_free(d.in); d.in = nullptr; }
  if (d.out) { usb_host_transfer_free(d.out); d.out = nullptr; }
  if (d.claimed) { usb_host_interface_release(client, d.dev, d.ifnum); d.claimed = false; }
  d.outHead = d.outTail = 0;
  if (d.gone) {
    usb_host_device_close(client, d.dev);
    d.dev = nullptr;
    d.ignore = false;
    d.gone = false;
    __sync_synchronize();
    d.state = kFree;
  } else {
    d.ignore = true;
    __sync_synchronize();
    d.state = kOpened;
  }
}

static void inDone(usb_transfer_t *t) {
  Device &d = *static_cast<Device *>(t->context);
  d.inPending = false;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    d.errors = 0;
    if (t->actual_num_bytes > 0 && rxSink) rxSink(indexOf(&d), t->data_buffer, (size_t)t->actual_num_bytes);
  } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) {
    finishStopping(d);
    return;
  } else {
    if (t->status == USB_TRANSFER_STATUS_STALL) usb_host_endpoint_clear(d.dev, d.epIn);
    if (++d.errors > 8) { stopDevice(d, false); finishStopping(d); return; } // it keeps failing, let it go
  }
  if (d.state != kActive) { finishStopping(d); return; }
  t->num_bytes = d.mpsIn;
  if (usb_host_transfer_submit(t) == ESP_OK) d.inPending = true;
  else stopDevice(d, false);
}

static void outDone(usb_transfer_t *t) {
  Device &d = *static_cast<Device *>(t->context);
  d.outPending = false;
  finishStopping(d);
}

static void doActivate(Device &d) {
  if (d.state != kOpened || d.ignore) return;
  if (usb_host_interface_claim(client, d.dev, d.ifnum, 0) != ESP_OK) { d.ignore = true; return; }
  d.claimed = true;
  if (usb_host_transfer_alloc(d.mpsIn, 0, &d.in) != ESP_OK) { d.in = nullptr; d.ignore = true; usb_host_interface_release(client, d.dev, d.ifnum); d.claimed = false; return; }
  d.in->device_handle = d.dev;
  d.in->bEndpointAddress = d.epIn;
  d.in->callback = inDone;
  d.in->context = &d;
  d.in->num_bytes = d.mpsIn;
  if (d.epOut && usb_host_transfer_alloc(d.mpsOut, 0, &d.out) == ESP_OK) {
    d.out->device_handle = d.dev;
    d.out->bEndpointAddress = d.epOut;
    d.out->callback = outDone;
    d.out->context = &d;
  } else {
    d.out = nullptr;
    d.epOut = 0;
  }
  d.errors = 0;
  d.outHead = d.outTail = 0;
  d.state = kActive;
  if (usb_host_transfer_submit(d.in) == ESP_OK) d.inPending = true;
  else stopDevice(d, false);
}

static void serviceOut(Device &d) {
  if (d.state != kActive || !d.out || d.outPending || d.outHead == d.outTail) return;
  uint16_t n = 0;
  const uint16_t cap = (uint16_t)((d.mpsOut / 4) * 4);
  uint16_t tail = d.outTail;
  while (tail != d.outHead && n < cap) { d.out->data_buffer[n++] = d.outRing[tail]; tail = (uint16_t)((tail + 1) % kOutRing); }
  d.outTail = tail;
  d.out->num_bytes = n;
  d.out->flags = 0;
  if (usb_host_transfer_submit(d.out) == ESP_OK) d.outPending = true;
}

// A new device: open it, look for a MIDI Streaming interface (class 1, subclass 3) with bulk endpoints, and keep it if there is one.
// A hub, or anything else, is closed again. The library handles hubs itself and tells us about each device behind one.
static void onNewDevice(uint8_t address) {
  usb_device_handle_t dev;
  if (usb_host_device_open(client, address, &dev) != ESP_OK) return;
  const usb_config_desc_t *cfg = nullptr;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || !cfg) { usb_host_device_close(client, dev); return; }

  int chosen = -1;
  bool inChosen = false;
  uint8_t epIn = 0, epOut = 0;
  uint16_t mpsIn = 0, mpsOut = 0;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(cfg);
  const uint8_t *end = p + cfg->wTotalLength;
  while (p + 2 <= end && p[0] >= 2) {
    const uint8_t length = p[0], type = p[1];
    if (type == 0x04 && length >= 9) { // interface: number, alternate setting, ..., class, subclass
      const uint8_t number = p[2], alt = p[3], cls = p[5], sub = p[6];
      if (chosen < 0 && cls == 0x01 && sub == 0x03 && alt == 0) { chosen = number; inChosen = true; }
      else inChosen = false; // anything after the chosen interface's own endpoints ends it
    } else if (type == 0x05 && length >= 7 && inChosen) { // endpoint: address, attributes, max packet size
      const uint8_t address2 = p[2], attributes = p[3] & 0x03;
      const uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
      if (attributes == 0x02) { // bulk
        if (address2 & 0x80) { if (!epIn) { epIn = address2; mpsIn = mps; } }
        else if (!epOut) { epOut = address2; mpsOut = mps; }
      }
    }
    p += length;
  }
  if (chosen < 0 || !epIn || mpsIn == 0 || mpsIn > 512) { usb_host_device_close(client, dev); return; }

  Device *slot = nullptr;
  for (Device &d : devs) if (d.state == kFree) { slot = &d; break; }
  if (!slot) { usb_host_device_close(client, dev); return; } // more MIDI devices than the module keeps track of

  Device &d = *slot;
  d.dev = dev;
  d.ifnum = (uint8_t)chosen;
  d.epIn = epIn; d.epOut = epOut; d.mpsIn = mpsIn; d.mpsOut = mpsOut;
  d.in = d.out = nullptr;
  d.inPending = d.outPending = false;
  d.claimed = false;
  d.gone = false;
  d.ignore = false;
  d.activateWanted = d.releaseWanted = false;
  d.errors = 0;
  d.outHead = d.outTail = 0;
  const usb_device_desc_t *dd = nullptr;
  d.vid = d.pid = 0;
  if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && dd) { d.vid = dd->idVendor; d.pid = dd->idProduct; }
  memset(d.name, 0, sizeof(d.name));
  d.serialHash = 0;
  usb_device_info_t info;
  if (usb_host_device_info(dev, &info) == ESP_OK) {
    if (info.str_desc_product && info.str_desc_product->bLength > 2) {
      const uint8_t chars = (uint8_t)((info.str_desc_product->bLength - 2) / 2);
      for (uint8_t i = 0; i < chars && i < NAME_CHARS; ++i) {
        const uint16_t c = info.str_desc_product->wData[i];
        d.name[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
      }
    }
    if (info.str_desc_serial_num && info.str_desc_serial_num->bLength > 2) { // for two of the same device, and for a device to find its slot again
      uint16_t h = 0x811C;
      const uint8_t chars = (uint8_t)((info.str_desc_serial_num->bLength - 2) / 2);
      for (uint8_t i = 0; i < chars; ++i) h = (uint16_t)((h ^ (info.str_desc_serial_num->wData[i] & 0xFF)) * 31 + 7);
      d.serialHash = h;
    }
  }
  if (d.name[0] == 0) snprintf(d.name, sizeof(d.name), "USB %04X:%04X", d.vid, d.pid);
  __sync_synchronize();
  d.state = kOpened;
}

static void onGone(usb_device_handle_t dev) {
  for (Device &d : devs) {
    if (d.state == kFree || d.dev != dev) continue;
    if (d.state == kOpened) { // nothing in flight
      usb_host_device_close(client, d.dev);
      d.dev = nullptr;
      d.ignore = false;
      d.state = kFree;
    } else {
      stopDevice(d, true);
      finishStopping(d);
    }
  }
}

static void clientEvent(const usb_host_client_event_msg_t *msg, void *) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) onNewDevice(msg->new_dev.address);
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) onGone(msg->dev_gone.dev_hdl);
}

static void daemonTask(void *) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

static void clientTask(void *) {
  for (;;) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(20));
    for (Device &d : devs) {
      if (d.releaseWanted) {
        d.releaseWanted = false;
        if (d.state == kActive) stopDevice(d, false);
      }
      if (d.activateWanted && d.state == kOpened) {
        d.activateWanted = false;
        doActivate(d);
      } else if (d.activateWanted && d.state != kActive && d.state != kStopping) {
        d.activateWanted = false;
      }
      finishStopping(d);
      serviceOut(d);
    }
  }
}

// Starts the host. False if the library would not install, and the port is then left as it was.
inline bool begin(RxFn sink) {
  if (started) return true;
  rxSink = sink;
  usb_host_config_t config = {};
  config.skip_phy_setup = false;
  config.root_port_unpowered = false;
  config.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&config) != ESP_OK) return false;
  xTaskCreatePinnedToCore(daemonTask, "usbhost", 4096, nullptr, 20, nullptr, 0);
  usb_host_client_config_t clientConfig = {};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 8;
  clientConfig.async.client_event_callback = clientEvent;
  clientConfig.async.callback_arg = nullptr;
  if (usb_host_client_register(&clientConfig, &client) != ESP_OK) return false;
  xTaskCreatePinnedToCore(clientTask, "usbclient", 6144, nullptr, 19, nullptr, 0);
  started = true;
  return true;
}

}  // namespace usbmidi
