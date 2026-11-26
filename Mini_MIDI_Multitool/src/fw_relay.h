#pragma once

// The main brain's end of a relayed module update, see foxwizard_link.h. One block at a time goes to the module, and the answer to
// the SysEx that carried it waits for the module's reply. Nothing here touches hardware, tests/fw_relay_test.cpp runs it against a
// module model on a link that drops and damages frames.

#include <stdint.h>
#include <string.h>

#include "../foxwizard_link.h"

namespace fw::relay {

constexpr uint8_t TARGET_MAIN = 0;
constexpr uint8_t TARGET_HOST = 1; // the USB host module
constexpr uint8_t TARGET_BLE = 2;  // the Bluetooth module

// Codes in the ACK's extra field when the failure is the relay's, not the module's updater (those are small numbers).
constexpr uint8_t ERR_ABSENT = 0x70;   // the module is not there
constexpr uint8_t ERR_SILENT = 0x71;   // it did not answer
constexpr uint8_t ERR_OLD = 0x72;      // its firmware cannot take an update
constexpr uint8_t ERR_BUSY = 0x73;     // another block is still on its way
constexpr uint8_t ERR_DAMAGED = 0x74;  // blocks kept arriving damaged
constexpr uint8_t ERR_NOT_READY = 0x75; // the module has no update open

constexpr uint32_t BEGIN_WAIT_MS = 5000;  // the module may have to set up its update space
constexpr uint32_t DATA_WAIT_MS = 700;    // a flash write is well under this
constexpr uint32_t END_WAIT_MS = 8000;    // the module checks the whole image's MD5
constexpr uint8_t TRIES = 6;              // sends of one block when it keeps arriving damaged, they cost a few milliseconds each
constexpr uint8_t QUIET_TRIES = 3;        // sends of one block when the module stays silent, the wait is the cost (3 DATA waits fit inside Patchling's 3 s)
constexpr uint8_t FRAME_ROOM = 8;         // queue slots left free for other control frames

enum Outcome : uint8_t { PENDING, DONE, FAILED };

struct Result {
  Outcome outcome = PENDING;
  uint8_t status = 0; // FAILED: the module's UPD_* status, or 0 for the relay's own code
  uint8_t extra = 0;  // FAILED: the module's byte 3, or an ERR_* code
};

struct Relay {
  bool busy = false;
  uint8_t target = TARGET_MAIN;
  UpdBlock block;
  uint8_t next = 0;      // next frame of the block to queue
  uint8_t tries = 0;
  uint8_t quiet = 0;
  uint32_t deadlineMs = 0;
  bool deadlineSet = false;

  // The sender's own record of what the block is for.
  uint8_t sourcePort = 0;
  uint16_t chunk = 0;
  uint8_t checksum = 0;

  uint16_t chunkNumber() const { return (uint16_t)(((uint16_t)block.data[0] << 8) | block.data[1]); }

  void start(uint8_t forTarget) {
    busy = true;
    target = forTarget;
    next = 0;
    tries = 1;
    quiet = 0;
    deadlineSet = false;
  }

  void begin(uint8_t forTarget, uint32_t size, const uint8_t md5[16]) {
    block.cmd = UPD_CMD_BEGIN;
    block.len = 19;
    block.data[0] = (uint8_t)(size >> 16); block.data[1] = (uint8_t)(size >> 8); block.data[2] = (uint8_t)size;
    memcpy(block.data + 3, md5, 16);
    start(forTarget);
  }

  void data(uint8_t forTarget, uint16_t index, const uint8_t *raw, uint8_t n) {
    block.cmd = UPD_CMD_DATA;
    block.len = (uint8_t)(2 + n);
    block.data[0] = (uint8_t)(index >> 8); block.data[1] = (uint8_t)index;
    memcpy(block.data + 2, raw, n);
    start(forTarget);
  }

  void end(uint8_t forTarget) {
    block.cmd = UPD_CMD_END;
    block.len = 0;
    start(forTarget);
  }

  uint32_t waitMs() const {
    return block.cmd == UPD_CMD_BEGIN ? BEGIN_WAIT_MS : (block.cmd == UPD_CMD_END ? END_WAIT_MS : DATA_WAIT_MS);
  }

  void giveUp() { busy = false; }

  // Queues the block's frames as the queue has room, and sends the block again when no reply comes. Q has freeSlots(bool) and
  // push(const LinkFrame &, bool). Returns FAILED when the module stayed silent.
  template <typename Q>
  Result pump(uint32_t nowMs, Q &queue) {
    Result r;
    if (!busy) return r;
    while (next < block.frames() && queue.freeSlots(true) > FRAME_ROOM) {
      LinkFrame f;
      block.frame(next++, f);
      queue.push(f, true);
    }
    if (next < block.frames()) return r;
    if (!deadlineSet) { deadlineMs = nowMs + waitMs(); deadlineSet = true; }
    if ((int32_t)(nowMs - deadlineMs) < 0) return r;
    // No reply. Sending an END again is not safe to assume: if the first got through the module is already restarting.
    if (block.cmd == UPD_CMD_END || ++quiet >= QUIET_TRIES) {
      busy = false;
      r.outcome = FAILED;
      r.extra = ERR_SILENT;
      return r;
    }
    ++tries;
    next = 0;
    deadlineSet = false;
    return r;
  }

  // A reply frame from the module. Replies to a block that is not the one waiting are ignored, so a late answer to an earlier send
  // of the same block cannot be mistaken for the answer to the next.
  Result reply(uint8_t cmd, uint8_t status, uint8_t extra) {
    Result r;
    if (!busy || cmd != block.cmd) return r;
    if (status == UPD_OK) {
      if (cmd == UPD_CMD_DATA && extra != (uint8_t)chunk) return r; // an answer to some other chunk
      busy = false;
      r.outcome = DONE;
      return r;
    }
    if (status == UPD_BAD_BLOCK) {
      if (tries >= TRIES) {
        busy = false;
        r.outcome = FAILED;
        r.extra = ERR_DAMAGED;
        return r;
      }
      ++tries;
      next = 0;
      deadlineSet = false;
      return r;
    }
    busy = false;
    r.outcome = FAILED;
    r.status = status;
    r.extra = status == UPD_NOT_READY ? ERR_NOT_READY : extra;
    return r;
  }
};

}  // namespace fw::relay
