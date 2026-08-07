/*
  ABTRSMIDIUSBC_Adaptor.ino by woz.lol
  RP2040 USB-C MIDI <-> DIN/TRS MIDI bridge

  Note: Status LEDs fade in over 1 sec and fade out over 1/2 sec so that the blinking is not distracting while playing

  Purpose:
  - Basic USB-C MIDI device <-> DIN/TRS MIDI adapter + Advanced router features via sysex config from a web app
  - Activity LEDs for DIN/TRS OUT, USB, and DIN/TRS IN
  - Panic button on GP5, configurable by sysex

  Wiring:
  - GP1 / board pin 1 = DIN MIDI IN to this RP2040 RX
  - GP0 / board pin 0 = DIN MIDI OUT from this RP2040 TX
  - GP2 = OUT activity LED
  - GP3 = USB activity LED
  - GP4 = IN activity LED
  - GP5 = front button, active LOW with INPUT_PULLUP

  Arduino setup:
  - Board: Waveshare RP2040-Zero
  - Tools -> USB Stack = Adafruit TinyUSB

*/

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// LED BRIGHTNESS
constexpr uint8_t LED_OUT_MAX = 128; // Red
constexpr uint8_t LED_USB_MAX = 96; // Green
constexpr uint8_t LED_IN_MAX = 223;  // Yellow
//

constexpr uint8_t PIN_DIN_MIDI_TX = 0;
constexpr uint8_t PIN_DIN_MIDI_RX = 1;
constexpr uint8_t PIN_LED_OUT = 2;
constexpr uint8_t PIN_LED_USB = 3;
constexpr uint8_t PIN_LED_IN = 4;
constexpr uint8_t PIN_BUTTON = 5;
constexpr uint32_t DIN_BAUD = 31250;
constexpr uint32_t LED_FADE_IN_MS = 1000;
constexpr uint32_t LED_FADE_OUT_MS = 500;
constexpr uint32_t ACTIVITY_HOLD_MS = 12;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 25;
constexpr uint32_t STARTUP_BEAT_MS = 125; // Startup chase is twice the 240 BPM beat speed.
constexpr uint32_t STARTUP_FADE_MS = 250;
constexpr uint32_t PANIC_FLASH_MS = 35;

Adafruit_USBD_MIDI usb_midi;

enum LedPhase {
  LED_OFF,
  LED_RISING,
  LED_ON,
  LED_FALLING
};

struct ActivityLed {
  uint8_t pin;
  uint8_t maxLevel;
  LedPhase phase = LED_OFF;
  uint32_t phaseStartMs = 0;
  uint32_t lastActivityMs = 0;
  uint8_t currentLevel = 0;
  uint8_t fadeStartLevel = 0;
  bool activitySeen = false;
};

struct DinParser {
  uint8_t runningStatus = 0;
  uint8_t data[2] = {0, 0};
  uint8_t needed = 0;
  uint8_t have = 0;
};

ActivityLed ledOut = {PIN_LED_OUT, LED_OUT_MAX};
ActivityLed ledUsb = {PIN_LED_USB, LED_USB_MAX};
ActivityLed ledIn = {PIN_LED_IN, LED_IN_MAX};
DinParser dinParser;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t lastButtonChangeMs = 0;

uint8_t scaleByte(uint32_t elapsed, uint32_t duration) {
  if (elapsed >= duration) return 255;
  return static_cast<uint8_t>((elapsed * 255UL) / duration);
}

uint8_t maxLevelForPin(uint8_t pin) {
  switch (pin) {
    case PIN_LED_OUT: return LED_OUT_MAX;
    case PIN_LED_USB: return LED_USB_MAX;
    case PIN_LED_IN: return LED_IN_MAX;
    default: return 255;
  }
}

uint8_t clampLedLevel(uint8_t pin, uint8_t value) {
  const uint8_t maxLevel = maxLevelForPin(pin);
  return (value > maxLevel) ? maxLevel : value;
}

void writeLed(uint8_t pin, uint8_t value) {
  analogWrite(pin, clampLedLevel(pin, value));
}

void writeActivityLed(ActivityLed &led, uint8_t value) {
  led.currentLevel = (value > led.maxLevel) ? led.maxLevel : value;
  analogWrite(led.pin, led.currentLevel);
}

void serviceActivityLed(ActivityLed &led) {
  const uint32_t now = millis();

  if (led.activitySeen) {
    led.activitySeen = false;
    led.lastActivityMs = now;
    if (led.phase != LED_RISING && led.phase != LED_ON) {
      led.phase = LED_RISING;
      led.phaseStartMs = now;
      led.fadeStartLevel = led.currentLevel;
    }
  }

  switch (led.phase) {
    case LED_OFF:
      writeActivityLed(led, 0);
      break;

    case LED_RISING: {
      const uint32_t elapsed = now - led.phaseStartMs;
      const uint8_t progress = scaleByte(elapsed, LED_FADE_IN_MS);
      const uint8_t level = led.fadeStartLevel + (((led.maxLevel - led.fadeStartLevel) * progress) / 255);
      writeActivityLed(led, level);
      if (elapsed >= LED_FADE_IN_MS) {
        led.phase = LED_ON;
        writeActivityLed(led, led.maxLevel);
      }
      break;
    }

    case LED_ON:
      writeActivityLed(led, led.maxLevel);
      if (now - led.lastActivityMs > ACTIVITY_HOLD_MS) {
        led.phase = LED_FALLING;
        led.phaseStartMs = now;
        led.fadeStartLevel = led.currentLevel;
      }
      break;

    case LED_FALLING: {
      if (now - led.lastActivityMs <= ACTIVITY_HOLD_MS) {
        led.phase = LED_RISING;
        led.phaseStartMs = now;
        led.fadeStartLevel = led.currentLevel;
        break;
      }

      const uint32_t elapsed = now - led.phaseStartMs;
      if (elapsed >= LED_FADE_OUT_MS) {
        led.phase = LED_OFF;
        writeActivityLed(led, 0);
      } else {
        const uint8_t progress = scaleByte(elapsed, LED_FADE_OUT_MS);
        const uint8_t level = led.fadeStartLevel - ((led.fadeStartLevel * progress) / 255);
        writeActivityLed(led, level);
      }
      break;
    }
  }
}

void noteActivity(ActivityLed &led) {
  led.activitySeen = true;
}

void setAllLedLevels(uint8_t outLevel, uint8_t usbLevel, uint8_t inLevel) {
  writeLed(PIN_LED_OUT, outLevel);
  writeLed(PIN_LED_USB, usbLevel);
  writeLed(PIN_LED_IN, inLevel);
}

void fadeAllLeds(uint8_t fromLevel, uint8_t toLevel, uint32_t durationMs) {
  const uint32_t startMs = millis();

  while (true) {
#ifdef TINYUSB_NEED_POLLING_TASK
    TinyUSBDevice.task();
#endif
    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= durationMs) break;

    const int16_t delta = static_cast<int16_t>(toLevel) - static_cast<int16_t>(fromLevel);
    const uint8_t level = static_cast<uint8_t>(fromLevel + ((delta * static_cast<int32_t>(elapsed)) / static_cast<int32_t>(durationMs)));
    setAllLedLevels(level, level, level);
    delay(2);
  }

  setAllLedLevels(toLevel, toLevel, toLevel);
}

void allLedPanicFlash() {
  fadeAllLeds(0, 255, 500);
  fadeAllLeds(255, 0, 500);

  ledOut.currentLevel = 0;
  ledUsb.currentLevel = 0;
  ledIn.currentLevel = 0;
  ledOut.phase = LED_OFF;
  ledUsb.phase = LED_OFF;
  ledIn.phase = LED_OFF;
}

void allLedConfirmationFlashes() {
  for (uint8_t i = 0; i < 3; ++i) {
    setAllLedLevels(255, 255, 255);
    delay(PANIC_FLASH_MS);
    setAllLedLevels(0, 0, 0);
    delay(PANIC_FLASH_MS);
  }

  ledOut.currentLevel = 0;
  ledUsb.currentLevel = 0;
  ledIn.currentLevel = 0;
  ledOut.phase = LED_OFF;
  ledUsb.phase = LED_OFF;
  ledIn.phase = LED_OFF;
}

void startupSequence() {
  setAllLedLevels(LED_OUT_MAX, 0, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, LED_USB_MAX, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, 0, LED_IN_MAX);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, LED_USB_MAX, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(LED_OUT_MAX, 0, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, 0, 0);
  delay(STARTUP_BEAT_MS);
  fadeAllLeds(0, 255, STARTUP_FADE_MS);
  fadeAllLeds(255, 0, STARTUP_FADE_MS);
}

void resetRoutingToFactoryDefault() {
  // Routing storage will be added with the SysEx-programmable router.
}

uint8_t midiDataLength(uint8_t status) {
  if (status >= 0xF8) return 0;

  if (status >= 0xF0) {
    switch (status) {
      case 0xF1:
      case 0xF3:
        return 1;
      case 0xF2:
        return 2;
      default:
        return 0;
    }
  }

  switch (status & 0xF0) {
    case 0x80:
    case 0x90:
    case 0xA0:
    case 0xB0:
    case 0xE0:
      return 2;
    case 0xC0:
    case 0xD0:
      return 1;
    default:
      return 0;
  }
}

uint8_t cinFromStatus(uint8_t status, uint8_t len) {
  if (status >= 0xF8) return 0x0F;

  if (status >= 0xF0) {
    switch (status) {
      case 0xF1: return 0x02;
      case 0xF2: return 0x03;
      case 0xF3: return 0x02;
      case 0xF6: return 0x05;
      default:   return (len == 1) ? 0x05 : ((len == 2) ? 0x06 : 0x04);
    }
  }

  switch (status & 0xF0) {
    case 0x80: return 0x08;
    case 0x90: return 0x09;
    case 0xA0: return 0x0A;
    case 0xB0: return 0x0B;
    case 0xC0: return 0x0C;
    case 0xD0: return 0x0D;
    case 0xE0: return 0x0E;
    default:   return 0x00;
  }
}

uint8_t midiPacketDataLength(uint8_t cin) {
  switch (cin & 0x0F) {
    case 0x2:
    case 0x6:
    case 0xC:
    case 0xD:
      return 2;
    case 0x3:
    case 0x4:
    case 0x7:
    case 0x8:
    case 0x9:
    case 0xA:
    case 0xB:
    case 0xE:
      return 3;
    case 0x5:
    case 0xF:
      return 1;
    default:
      return 0;
  }
}

void sendUsbMidi(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
  uint8_t packet[4] = {
    cinFromStatus(status, len),
    status,
    data1,
    data2
  };
  usb_midi.writePacket(packet);
  noteActivity(ledUsb);
}

void processDinByte(uint8_t b) {
  noteActivity(ledIn);

  if (b >= 0xF8) {
    sendUsbMidi(b, 0, 0, 1);
    return;
  }

  if (b & 0x80) {
    dinParser.runningStatus = b;
    dinParser.needed = midiDataLength(b);
    dinParser.have = 0;

    if (dinParser.needed == 0) {
      sendUsbMidi(b, 0, 0, 1);
      if (b >= 0xF0) dinParser.runningStatus = 0;
    }
    return;
  }

  if (!dinParser.runningStatus || !dinParser.needed) return;

  dinParser.data[dinParser.have++] = b;
  if (dinParser.have < dinParser.needed) return;

  const uint8_t status = dinParser.runningStatus;
  const uint8_t data1 = dinParser.data[0];
  const uint8_t data2 = (dinParser.needed > 1) ? dinParser.data[1] : 0;
  sendUsbMidi(status, data1, data2, dinParser.needed + 1);
  dinParser.have = 0;

  if (status >= 0xF0) dinParser.runningStatus = 0;
}

void pumpDinToUsb() {
  while (Serial1.available() > 0) {
    processDinByte(static_cast<uint8_t>(Serial1.read()));
  }
}

void pumpUsbToDin() {
  uint8_t packet[4];
  while (usb_midi.readPacket(packet)) {
    const uint8_t len = midiPacketDataLength(packet[0]);
    for (uint8_t i = 0; i < len; ++i) {
      Serial1.write(packet[i + 1]);
      noteActivity(ledOut);
    }
    if (len > 0) noteActivity(ledUsb);
  }
}

void sendDinMidi(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
  Serial1.write(status);
  if (len > 1) Serial1.write(data1);
  if (len > 2) Serial1.write(data2);
  noteActivity(ledOut);
}

void sendMidiBoth(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
  sendDinMidi(status, data1, data2, len);
  sendUsbMidi(status, data1, data2, len);
  noteActivity(ledIn);
  serviceLeds();
}

void sendPanic() {
  for (uint8_t channel = 0; channel < 16; ++channel) {
    const uint8_t controlChangeStatus = 0xB0 | channel;
    sendMidiBoth(controlChangeStatus, 121, 0, 3);
    sendMidiBoth(controlChangeStatus, 123, 0, 3);
    sendMidiBoth(controlChangeStatus, 120, 0, 3);
  }

  for (uint8_t channel = 0; channel < 16; ++channel) {
    const uint8_t noteOffStatus = 0x80 | channel;
    for (uint8_t note = 0; note < 128; ++note) {
      sendMidiBoth(noteOffStatus, note, 0, 3);
    }
  }

  Serial1.flush();
  allLedConfirmationFlashes();
}

void serviceButton() {
  const uint32_t now = millis();
  const bool reading = digitalRead(PIN_BUTTON);

  if (reading != lastButtonReading) {
    lastButtonChangeMs = now;
    lastButtonReading = reading;
  }

  if (now - lastButtonChangeMs < BUTTON_DEBOUNCE_MS) return;
  if (reading == stableButtonState) return;

  stableButtonState = reading;
  if (stableButtonState == LOW) {
    sendPanic();
  }
}

void serviceLeds() {
  serviceActivityLed(ledOut);
  serviceActivityLed(ledUsb);
  serviceActivityLed(ledIn);
}

void setup() {
  pinMode(PIN_LED_OUT, OUTPUT);
  pinMode(PIN_LED_USB, OUTPUT);
  pinMode(PIN_LED_IN, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  setAllLedLevels(0, 0, 0);

  const bool factoryResetRequested = digitalRead(PIN_BUTTON) == LOW;

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  usb_midi.setStringDescriptor("AB TRS MIDI USBC Adaptor 1");
  usb_midi.begin();

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  Serial1.setTX(PIN_DIN_MIDI_TX);
  Serial1.setRX(PIN_DIN_MIDI_RX);
  Serial1.begin(DIN_BAUD);

  if (factoryResetRequested) {
    resetRoutingToFactoryDefault();
  }

  startupSequence();
}

void loop() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif

  pumpUsbToDin();
  pumpDinToUsb();
  serviceButton();
  serviceLeds();
}
