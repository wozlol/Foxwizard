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

#ifdef __cplusplus
}
#endif
