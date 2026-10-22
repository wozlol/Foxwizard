#pragma once
// FoxWizard addition. The patched RP2040 USB host driver calls this where stock TinyUSB calls
// panic(), which halts the chip. Defined in Foxwizard_Host_Edition_Sidecar.ino, which restarts the
// sidecar at once with the fault as the reason. Never returns. See VENDORED.md.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  FOXWIZARD_FAULT_DATA_SEQ = 1, // data sequence (DATA0/DATA1) mismatch
  FOXWIZARD_FAULT_STALL = 2,    // the stack asked to clear a device's stalled endpoint, unsupported here
  FOXWIZARD_FAULT_BUF_CTRL = 3  // a buffer was armed while already armed
};

void foxwizard_usb_fault(uint8_t fault);

// Defined in host/hub.c: how many downstream ports the hub at this address has, 0 if it isn't one.
uint8_t foxwizard_hub_port_count(uint8_t daddr);

// Things the host stack does that nothing else reports, for the host report's event list. Defined in
// Foxwizard_Host_Edition_Sidecar.ino, called from the USB task, never an interrupt.
enum {
  FOXWIZARD_TRACE_ENUM_FAILED = 1,   // a device never finished enumerating. a = its hub's address, b = its hub port
  FOXWIZARD_TRACE_EDPT_POOL_FULL = 2 // no free endpoint slot for a device. a = slots in use, b = the device's address
};
void foxwizard_usb_trace(uint8_t code, uint8_t a, uint8_t b);

#ifdef __cplusplus
}
#endif
