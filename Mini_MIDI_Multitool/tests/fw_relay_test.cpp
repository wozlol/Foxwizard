// The relayed module update: the block format in ../foxwizard_link.h and the main brain's end in ../src/fw_relay.h, run on a computer,
// not the device, against a module model on a link that drops and damages frames:
//   clang++ -std=c++17 -I .. -I ../src fw_relay_test.cpp -o fw_relay_test && ./fw_relay_test
#include "fw_relay.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace fw::relay;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; if (fails < 12) { printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// The module's side, as the sidecars do it: a block reader, the next chunk it wants, the bytes it has kept.
struct Module {
  UpdBlockReader reader;
  bool begun = false;
  uint16_t nextChunk = 0;
  uint32_t size = 0;
  std::vector<uint8_t> image;
  bool ended = false;
  std::vector<std::vector<uint8_t>> replies; // control packets waiting to go to the main brain

  void reply(uint8_t cmd, uint8_t status, uint8_t extra) {
    uint8_t pkt[4];
    updControl(UPD_REPLY, cmd, status, extra, pkt);
    replies.push_back(std::vector<uint8_t>(pkt, pkt + 4));
  }

  void feed(const LinkFrame &f) {
    uint8_t pkt[4];
    if (decodeLinkFrame(f, pkt) != LINK_PORT_CONTROL || (pkt[0] & 0x0F) != CONTROL_UPDATE) return;
    const UpdBlockReader::Result r = reader.feed(pkt);
    if (r == UpdBlockReader::NONE) return;
    const UpdBlock &b = reader.block;
    if (r == UpdBlockReader::BAD) { reply(b.cmd, UPD_BAD_BLOCK, 0); return; }
    if (b.cmd == UPD_CMD_BEGIN) {
      size = ((uint32_t)b.data[0] << 16) | ((uint32_t)b.data[1] << 8) | b.data[2];
      begun = true; nextChunk = 0; image.clear(); ended = false;
      reply(b.cmd, UPD_OK, 0);
    } else if (b.cmd == UPD_CMD_DATA) {
      if (!begun) { reply(b.cmd, UPD_NOT_READY, 0); return; }
      const uint16_t idx = (uint16_t)(((uint16_t)b.data[0] << 8) | b.data[1]);
      if (nextChunk > 0 && idx == nextChunk - 1) { reply(b.cmd, UPD_OK, (uint8_t)idx); return; } // its reply was lost
      if (idx != nextChunk) { reply(b.cmd, UPD_BAD_INDEX, (uint8_t)nextChunk); return; }
      image.insert(image.end(), b.data + 2, b.data + b.len);
      ++nextChunk;
      reply(b.cmd, UPD_OK, (uint8_t)idx);
    } else if (b.cmd == UPD_CMD_END) {
      if (!begun) { reply(b.cmd, UPD_NOT_READY, 0); return; }
      if (image.size() != size) { reply(b.cmd, UPD_ERROR, 5); return; }
      ended = true;
      reply(b.cmd, UPD_OK, 0);
    }
  }
};

// The main brain's queue and a link that treats frames badly. lossPm drops a frame and damagePm changes a data bit in one, per thousand.
struct World {
  MidiQueue<LinkFrame, 64, 0> queue;
  Module module;
  Relay relay;
  uint32_t now = 1000;
  int lossPm = 0, damagePm = 0, replyLossPm = 0; // per thousand
  uint32_t seed = 12345;
  int sent = 0;

  int rnd(int n) { seed = seed * 1664525u + 1013904223u; return (int)((seed >> 16) % (uint32_t)n); }

  void carry() { // everything queued crosses the link
    while (LinkFrame *f = queue.peek()) {
      LinkFrame copy = *f;
      queue.pop();
      ++sent;
      if (rnd(1000) < lossPm) continue;
      if (rnd(1000) < damagePm) copy.b[2 + rnd(4)] ^= (uint8_t)(1 << rnd(7));
      module.feed(copy);
    }
  }

  // Runs until the relay settles, returns its result.
  Result run(int maxMs = 60000) {
    for (int t = 0; t < maxMs; ++t) {
      now += 1;
      Result r = relay.pump(now, queue);
      if (r.outcome != PENDING) return r;
      carry();
      std::vector<std::vector<uint8_t>> got;
      got.swap(module.replies);
      for (auto &p : got) {
        if (rnd(1000) < replyLossPm) continue;
        r = relay.reply(p[1], p[2], p[3]);
        if (r.outcome != PENDING) return r;
      }
    }
    Result timeout;
    timeout.outcome = FAILED; timeout.extra = 0xFF;
    return timeout;
  }

  // The whole update, as Patchling drives it: BEGIN, every chunk, END, each waiting for its answer.
  bool update(const std::vector<uint8_t> &img, Result *last = nullptr) {
    uint8_t md5[16] = {1, 2, 3};
    relay.begin(TARGET_BLE, (uint32_t)img.size(), md5);
    Result r = run();
    if (last) *last = r;
    if (r.outcome != DONE) return false;
    const size_t chunks = (img.size() + UPD_CHUNK_BYTES - 1) / UPD_CHUNK_BYTES;
    for (size_t i = 0; i < chunks; ++i) {
      const size_t at = i * UPD_CHUNK_BYTES;
      const uint8_t n = (uint8_t)std::min<size_t>(UPD_CHUNK_BYTES, img.size() - at);
      relay.chunk = (uint16_t)i;
      relay.data(TARGET_BLE, (uint16_t)i, img.data() + at, n);
      r = run();
      if (last) *last = r;
      if (r.outcome != DONE) return false;
    }
    relay.end(TARGET_BLE);
    r = run();
    if (last) *last = r;
    return r.outcome == DONE;
  }
};

static std::vector<uint8_t> image(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i * 31 + (i >> 8) * 7 + 1);
  return v;
}

int main() {
  // The CRC, from the standard check string.
  { const uint8_t s[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'}; CHECK(updCrc16(s, 9) == 0x29B1, "crc16 of 123456789 is %04X", updCrc16(s, 9)); }

  // A block as frames and back, every length.
  for (int len = 0; len <= UPD_BLOCK_MAX; ++len) {
    UpdBlock b; b.cmd = UPD_CMD_DATA; b.len = (uint8_t)len;
    for (int i = 0; i < len; ++i) b.data[i] = (uint8_t)(i * 5 + len);
    UpdBlockReader rd; UpdBlockReader::Result res = UpdBlockReader::NONE; int done = 0;
    for (uint8_t i = 0; i < b.frames(); ++i) {
      LinkFrame f; b.frame(i, f);
      uint8_t pkt[4]; CHECK(decodeLinkFrame(f, pkt) == LINK_PORT_CONTROL && (pkt[0] & 0x0F) == CONTROL_UPDATE, "frame %d of a %d byte block is a control frame", i, len);
      res = rd.feed(pkt);
      if (res == UpdBlockReader::DONE) ++done;
    }
    CHECK(done == 1 && rd.block.len == len && memcmp(rd.block.data, b.data, len) == 0, "a %d byte block comes back whole", len);
  }

  // A clean link: a 9 KB image, every byte kept, END taken.
  { World w; auto img = image(9000); Result r; CHECK(w.update(img, &r), "clean update outcome %d extra %02X", r.outcome, r.extra);
    CHECK(w.module.image == img, "clean: image kept whole"); CHECK(w.module.ended, "clean: END taken"); }

  // A 650 KB image like the Bluetooth module's, clean, as a time and frame count.
  { World w; auto img = image(657000); CHECK(w.update(img), "large update"); CHECK(w.module.image == img, "large: image kept whole");
    printf("  657000 byte image: %d frames, %d bytes on the link\n", w.sent, w.sent * LINK_FRAME_BYTES); }

  // Frames dropped and damaged at every rate that still works: the image still arrives whole, or the update fails, never wrong.
  for (int loss : {1, 5, 30, 100}) for (int dmg : {0, 2, 10, 50}) {
    World w; w.lossPm = loss; w.damagePm = dmg; auto img = image(6000); Result r;
    const bool ok = w.update(img, &r);
    CHECK(!ok || w.module.image == img, "loss %d/1000 damage %d/1000: an update that says done has the whole image", loss, dmg);
    if (loss <= 5 && dmg <= 10) CHECK(ok, "loss %d/1000 damage %d/1000 outcome %d extra %02X", loss, dmg, r.outcome, r.extra);
    CHECK(!ok || w.module.ended, "loss %d/1000 damage %d/1000: END reached the module", loss, dmg);
  }

  // Replies lost: a chunk the module already has is confirmed again, never written twice.
  { World w; w.replyLossPm = 100; auto img = image(8000); Result r; const bool ok = w.update(img, &r);
    CHECK(ok, "lost replies: outcome %d extra %02X", r.outcome, r.extra); CHECK(w.module.image == img, "lost replies: nothing written twice or skipped"); }

  // A module that never answers: the relay gives up with the silent code and does not hang.
  { World w; w.lossPm = 1000; uint8_t md5[16] = {0}; w.relay.begin(TARGET_HOST, 1000, md5); Result r = w.run(); CHECK(r.outcome == FAILED && r.extra == ERR_SILENT, "silent module: outcome %d extra %02X", r.outcome, r.extra); CHECK(!w.relay.busy, "silent module: relay free again"); }

  // Blocks that always arrive damaged: gives up with the damaged code after the tries it has.
  { World w; w.damagePm = 1000; uint8_t md5[16] = {0}; w.relay.begin(TARGET_HOST, 1000, md5); Result r = w.run(); CHECK(r.outcome == FAILED && (r.extra == ERR_DAMAGED || r.extra == ERR_SILENT), "damaged link: outcome %d extra %02X", r.outcome, r.extra); }

  // A module that has no update open says so, and the relay reports it.
  { World w; const uint8_t raw[4] = {1, 2, 3, 4}; w.relay.chunk = 0; w.relay.data(TARGET_HOST, 0, raw, 4); Result r = w.run(); CHECK(r.outcome == FAILED && r.extra == ERR_NOT_READY, "no update open: outcome %d extra %02X", r.outcome, r.extra); }

  // A chunk out of order is refused with the chunk the module wants.
  { World w; uint8_t md5[16] = {0}; w.relay.begin(TARGET_HOST, 1000, md5); w.run(); const uint8_t raw[4] = {1, 2, 3, 4}; w.relay.chunk = 5; w.relay.data(TARGET_HOST, 5, raw, 4); Result r = w.run();
    CHECK(r.outcome == FAILED && r.status == UPD_BAD_INDEX && r.extra == 0, "out of order: outcome %d status %d extra %d", r.outcome, r.status, r.extra); }

  // Replies to another command or another chunk are ignored while waiting.
  { Relay rl; const uint8_t raw[4] = {1, 2, 3, 4}; rl.chunk = 7; rl.data(TARGET_BLE, 7, raw, 4);
    CHECK(rl.reply(UPD_CMD_BEGIN, UPD_OK, 0).outcome == PENDING, "a BEGIN reply does not answer a DATA block");
    CHECK(rl.reply(UPD_CMD_DATA, UPD_OK, 6).outcome == PENDING, "a reply for chunk 6 does not answer chunk 7");
    CHECK(rl.reply(UPD_CMD_DATA, UPD_OK, 7).outcome == DONE, "the reply for chunk 7 does"); }

  // END is never sent twice.
  { World w; w.lossPm = 1000; w.relay.end(TARGET_HOST); Result r = w.run(); CHECK(r.outcome == FAILED && r.extra == ERR_SILENT, "END silent"); CHECK(w.sent == 2, "END sent once, %d frames", w.sent); }

  printf(fails ? "FAILED, %d\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
