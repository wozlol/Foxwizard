// FoxWizard MIDI clock check. Runs on a Mac, talks to the FoxWizard over its USB MIDI port.
//
//   swiftc -O tools/clock_check.swift -o /tmp/clock_check
//   /tmp/clock_check list
//   /tmp/clock_check all                 the whole check, about 1 minute
//   /tmp/clock_check out [seconds]       the clock the FoxWizard sends, and how accurate and steady it is
//   /tmp/clock_check in [bpm] [seconds]  send it a clock and read back what it made of it
//   /tmp/clock_check selftest            the same checks against a simulated FoxWizard, no hardware
//
// Before the real thing: a preset with an arp (something has to use the clock), Clock set to Auto, and Send Clock to every
// port switched on. Options: --port NAME (part of the MIDI port's name, default FoxWizard), --expect BPM.

import Foundation
import CoreMIDI
import Darwin

// ---- time ------------------------------------------------------------------------------------------------------------

var timebase = mach_timebase_info_data_t()
mach_timebase_info(&timebase)
func ticksToSec(_ t: UInt64) -> Double { Double(t) * Double(timebase.numer) / Double(timebase.denom) / 1e9 }
func secToTicks(_ s: Double) -> UInt64 { UInt64(s * 1e9 * Double(timebase.denom) / Double(timebase.numer)) }

// ---- CoreMIDI plumbing -----------------------------------------------------------------------------------------------

struct Received { let time: Double; let bytes: [UInt8] }
final class Inbox {
  private let lock = NSLock()
  private var items: [Received] = []
  func add(_ r: Received) { lock.lock(); items.append(r); lock.unlock() }
  func take() -> [Received] { lock.lock(); let out = items; items = []; lock.unlock(); return out }
}

var client = MIDIClientRef()
MIDIClientCreate("clock_check" as CFString, nil, nil, &client)

func endpointName(_ e: MIDIEndpointRef) -> String {
  var name: Unmanaged<CFString>?
  MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name)
  return (name?.takeRetainedValue() as String?) ?? "?"
}

func readPackets(_ list: UnsafePointer<MIDIPacketList>, _ into: Inbox) {
  var packet = list.pointee.packet
  for _ in 0..<list.pointee.numPackets {
    let n = Int(packet.length)
    let bytes = withUnsafePointer(to: &packet.data) { p in
      p.withMemoryRebound(to: UInt8.self, capacity: n) { Array(UnsafeBufferPointer(start: $0, count: n)) }
    }
    // every real time byte is its own event, a packet can hold several
    var rest: [UInt8] = []
    for b in bytes {
      if b >= 0xF8 { into.add(Received(time: ticksToSec(packet.timeStamp != 0 ? packet.timeStamp : mach_absolute_time()), bytes: [b])) } else { rest.append(b) }
    }
    if !rest.isEmpty { into.add(Received(time: ticksToSec(packet.timeStamp != 0 ? packet.timeStamp : mach_absolute_time()), bytes: rest)) }
    packet = MIDIPacketNext(&packet).pointee
  }
}

final class Link {
  let inbox = Inbox()
  var inPort = MIDIPortRef()
  var outPort = MIDIPortRef()
  var dest = MIDIEndpointRef()
  var sysexTail: [UInt8] = []

  init?(source: MIDIEndpointRef, destination: MIDIEndpointRef) {
    dest = destination
    let box = inbox
    MIDIInputPortCreateWithBlock(client, "in" as CFString, &inPort) { list, _ in readPackets(list, box) }
    MIDIOutputPortCreate(client, "out" as CFString, &outPort)
    if MIDIPortConnectSource(inPort, source, nil) != noErr { return nil }
  }

  // at is a host time in ticks, 0 for now. CoreMIDI delivers it then, which is steadier than a thread waking up on time.
  func send(_ bytes: [UInt8], at: UInt64 = 0) {
    var buffer = [UInt8](repeating: 0, count: 1024)
    buffer.withUnsafeMutableBytes { raw in
      let list = raw.baseAddress!.assumingMemoryBound(to: MIDIPacketList.self)
      let p = MIDIPacketListInit(list)
      _ = MIDIPacketListAdd(list, 1024, p, at, bytes.count, bytes)
      MIDISend(outPort, dest, list)
    }
  }

  // The messages that came in since the last call, one each: a complete SysEx, a channel message, a real time byte. A real
  // parser of the byte stream, since one packet can hold the end of a SysEx and the start of a note.
  var inSysex = false
  var pending: [UInt8] = []
  var runningStatus: UInt8 = 0

  func dataBytes(_ status: UInt8) -> Int {
    switch status & 0xF0 {
    case 0x80, 0x90, 0xA0, 0xB0, 0xE0: return 2
    case 0xC0, 0xD0: return 1
    default: return status == 0xF2 ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0
    }
  }

  func drain() -> [Received] {
    var out: [Received] = []
    for r in inbox.take() {
      for b in r.bytes {
        if b >= 0xF8 { out.append(Received(time: r.time, bytes: [b])); continue }
        if inSysex {
          if b < 0x80 { sysexTail.append(b); continue }
          if b == 0xF7 { sysexTail.append(b); out.append(Received(time: r.time, bytes: sysexTail)); sysexTail = []; inSysex = false; continue }
          sysexTail = []; inSysex = false // a status byte ends it early, and is read as the start of the next message
        }
        if b == 0xF0 { inSysex = true; sysexTail = [b]; pending = []; runningStatus = 0; continue }
        if b >= 0x80 {
          pending = [b]
          if b < 0xF0 { runningStatus = b } else { runningStatus = 0 }
          if dataBytes(b) == 0 { out.append(Received(time: r.time, bytes: [b])); pending = [] }
          continue
        }
        if pending.isEmpty && runningStatus != 0 { pending = [runningStatus] }
        if pending.isEmpty { continue }
        pending.append(b)
        if pending.count == 1 + dataBytes(pending[0]) { out.append(Received(time: r.time, bytes: pending)); pending = [] }
      }
    }
    return out
  }

  // The memory readout in GET_STATUS (1.108): the lowest the free heap has been in bytes, and the deepest the main stack has gone.
  func memory(timeout: Double = 0.6) -> (heapLowest: Int, stackDeepest: Int, crash: Int, faultPc: Int)? {
    _ = drain()
    send([0xF0, 0x7D, 0x4D, 0x13, 0xF7])
    let until = Date().addingTimeInterval(timeout)
    while Date() < until {
      for r in drain() where r.bytes.count > 34 && r.bytes[0] == 0xF0 && r.bytes[3] == 0x14 {
        // the stack depth: from 1.122 two bytes at 41 and 42 in 16 byte units, before that byte 29, which stops at 2,032
        let deep = r.bytes.count > 4 + 42 ? (Int(r.bytes[4 + 41]) | (Int(r.bytes[4 + 42]) << 7)) * 16 : Int(r.bytes[4 + 29]) * 16
        return (Int(r.bytes[4 + 28]) * 512, deep, r.bytes.count > 4 + 31 ? Int(r.bytes[4 + 30]) | (Int(r.bytes[4 + 31]) << 7) : 0, r.bytes.count > 4 + 36 && r.bytes[4 + 36] & 0x40 != 0 ? Int(r.bytes[4 + 32]) | (Int(r.bytes[4 + 33]) << 7) | (Int(r.bytes[4 + 34]) << 14) | (Int(r.bytes[4 + 35]) << 21) | (Int(r.bytes[4 + 36] & 0x0F) << 28) : 0)
      }
      Thread.sleep(forTimeInterval: 0.01)
    }
    return nil
  }

  // GET_STATUS, and the tempo and clock state out of its reply: 0 not in use, 1 master, 2 following, 3 following with no clock,
  // 4 Auto leading with its own clock.
  func status(timeout: Double = 0.6) -> (bpm: Int, clock: Int, host: Bool, fw: Int)? {
    _ = drain()
    send([0xF0, 0x7D, 0x4D, 0x13, 0xF7])
    let until = Date().addingTimeInterval(timeout)
    var kept: [Received] = []
    while Date() < until {
      for r in drain() {
        if r.bytes.count > 12 && r.bytes[0] == 0xF0 && r.bytes[1] == 0x7D && r.bytes[3] == 0x14 {
          let p = Array(r.bytes[4..<(r.bytes.count - 1)])
          return (Int(p[6]) | (Int(p[7]) << 7), Int(p[8]), p[0] == 1, p.count > 14 ? Int(p[14]) : 0)
        }
        kept.append(r)
      }
      Thread.sleep(forTimeInterval: 0.01)
    }
    return nil
  }
}

extension Link {
  // IDENTITY_REQUEST: protocol, firmware major and minor.
  func identity(timeout: Double = 0.8) -> (protocolVersion: Int, major: Int, minor: Int)? {
    _ = drain()
    send([0xF0, 0x7D, 0x4D, 0x01, 0xF7])
    let until = Date().addingTimeInterval(timeout)
    while Date() < until {
      for r in drain() where r.bytes.count > 8 && r.bytes[0] == 0xF0 && r.bytes[3] == 0x02 { return (Int(r.bytes[4]), Int(r.bytes[5]), Int(r.bytes[6])) }
      Thread.sleep(forTimeInterval: 0.01)
    }
    return nil
  }

  // The Send Clock switch from GET_GLOBAL, nil when there is no reply.
  func sendClockOn(timeout: Double = 0.6) -> Bool? {
    _ = drain()
    send([0xF0, 0x7D, 0x4D, 0x10, 0xF7])
    let until = Date().addingTimeInterval(timeout)
    while Date() < until {
      for r in drain() where r.bytes.count > 16 && r.bytes[0] == 0xF0 && r.bytes[3] == 0x11 { return r.bytes[4 + 11] != 0 }
      Thread.sleep(forTimeInterval: 0.01)
    }
    return nil
  }
}

func findEndpoints(matching name: String) -> (MIDIEndpointRef, MIDIEndpointRef)? {
  var source: MIDIEndpointRef = 0, destination: MIDIEndpointRef = 0
  for i in 0..<MIDIGetNumberOfSources() { let e = MIDIGetSource(i); if endpointName(e).localizedCaseInsensitiveContains(name) { source = e; break } }
  for i in 0..<MIDIGetNumberOfDestinations() { let e = MIDIGetDestination(i); if endpointName(e).localizedCaseInsensitiveContains(name) { destination = e; break } }
  return source != 0 && destination != 0 ? (source, destination) : nil
}

// ---- measuring -------------------------------------------------------------------------------------------------------

struct ClockStats {
  var count = 0
  var bpm = 0.0
  var jitterStdMs = 0.0
  var jitterMaxMs = 0.0
  var gaps = 0        // intervals over 1.5 of the usual
  var doubles = 0     // under 0.5 of the usual
}

// Tempo from the slope of the clock byte times against their count, which USB wobble doesn't move, and the wobble from
// how far each byte is from that line.
func analyze(_ times: [Double]) -> ClockStats {
  var s = ClockStats()
  s.count = times.count
  guard times.count > 24 else { return s }
  let n = Double(times.count)
  let ks = (0..<times.count).map { Double($0) }
  let mk = ks.reduce(0, +) / n, mt = times.reduce(0, +) / n
  var num = 0.0, den = 0.0
  for i in 0..<times.count { num += (ks[i] - mk) * (times[i] - mt); den += (ks[i] - mk) * (ks[i] - mk) }
  let slope = num / den
  let intercept = mt - slope * mk
  s.bpm = 60.0 / (24.0 * slope)
  var sum2 = 0.0
  for i in 0..<times.count {
    let r = (times[i] - (intercept + slope * ks[i])) * 1000
    sum2 += r * r
    s.jitterMaxMs = max(s.jitterMaxMs, abs(r))
    if i > 0 { let d = times[i] - times[i - 1]; if d > 1.5 * slope { s.gaps += 1 } else if d < 0.5 * slope { s.doubles += 1 } }
  }
  s.jitterStdMs = (sum2 / n).squareRoot()
  return s
}

func measureOut(_ link: Link, seconds: Double) -> ClockStats {
  _ = link.drain()
  var times: [Double] = []
  let end = Date().addingTimeInterval(seconds)
  while Date() < end {
    for r in link.drain() where r.bytes == [0xF8] { times.append(r.time) }
    Thread.sleep(forTimeInterval: 0.02)
  }
  for r in link.drain() where r.bytes == [0xF8] { times.append(r.time) }
  return analyze(times)
}

// A clock of our own into the FoxWizard, one byte every 1/24 of a beat on a steady schedule.
final class ClockSender {
  var running = false
  func run(link: Link, bpm: Double, seconds: Double, start: Bool) {
    let interval = 60.0 / (bpm * 24.0)
    if start { link.send([0xFA]) }
    let t0 = mach_absolute_time()
    var k = 0
    let lead = secToTicks(0.004) // handed over 4 ms early, with the time it is due
    while ticksToSec(mach_absolute_time() - t0) < seconds {
      let target = t0 + secToTicks(Double(k) * interval)
      mach_wait_until(target > lead ? target - lead : target)
      link.send([0xF8], at: target)
      k += 1
    }
  }
}

// ---- the checks ------------------------------------------------------------------------------------------------------

var results: [(String, Bool, String)] = []
func record(_ name: String, _ ok: Bool, _ detail: String) {
  results.append((name, ok, detail))
  print("  \(ok ? "PASS" : "FAIL")  \(name): \(detail)")
}

func clockWord(_ s: Int) -> String {
  switch s { case 0: return "not in use"; case 1: return "master"; case 2: return "following"; case 3: return "following, no clock"; case 4: return "Auto, leading"; default: return "?" }
}

func checkOut(_ link: Link, label: String, expect: Double, seconds: Double) {
  print("Clock out, \(label): listening for \(Int(seconds)) s")
  let s = measureOut(link, seconds: seconds)
  if s.count < 24 {
    record("\(label): clock bytes arrive", false, "\(s.count) seen. Is Send Clock to every port on, and is something using the clock?")
    return
  }
  let errPpm = (s.bpm / expect - 1) * 1e6
  record("\(label): tempo", abs(errPpm) <= 1000, String(format: "%.3f BPM, %+.0f ppm from %.2f (the limit is 1000 ppm)", s.bpm, errPpm, expect))
  record("\(label): steadiness", s.jitterStdMs < 1.5 && s.jitterMaxMs < 6, String(format: "wobble %.2f ms (std) and %.2f ms (worst), USB adds up to about 1 ms", s.jitterStdMs, s.jitterMaxMs))
  record("\(label): no missing or doubled bytes", s.gaps == 0 && s.doubles == 0, "\(s.count) bytes, \(s.gaps) gaps, \(s.doubles) doubles")
}

func pollStatus(_ link: Link, times n: Int) -> [(bpm: Int, clock: Int)] {
  var out: [(Int, Int)] = []
  for _ in 0..<n { if let st = link.status() { out.append((st.bpm, st.clock)) }; Thread.sleep(forTimeInterval: 0.4) }
  return out.map { (bpm: $0.0, clock: $0.1) }
}

func checkIn(_ link: Link, bpm: Double, seconds: Double, start: Bool, label: String) {
  print("Clock in, \(label): sending \(bpm) BPM for \(Int(seconds)) s")
  var polled: [(bpm: Int, clock: Int)] = []
  let sender = ClockSender()
  let t = Thread { sender.run(link: link, bpm: bpm, seconds: seconds, start: start) }
  t.qualityOfService = .userInteractive
  t.start()
  Thread.sleep(forTimeInterval: 2.0)
  while !t.isFinished { polled += pollStatus(link, times: 1) }
  let last = polled.suffix(4)
  let sorted = last.map { Double($0.bpm) }.sorted()
  let median = sorted.isEmpty ? 0 : (sorted[(sorted.count - 1) / 2] + sorted[sorted.count / 2]) / 2
  // The tempo it reports is a whole number from a short average, so a BPM or two of wobble is normal.
  let ok = last.count >= 3 && last.allSatisfy { $0.clock == 2 && abs(Double($0.bpm) - bpm) <= 3 } && abs(median - bpm) <= 1
  record("\(label): it follows", ok, last.isEmpty ? "no status reply" : last.map { "\(clockWord($0.clock)) \($0.bpm)" }.joined(separator: ", "))
}

func runAll(_ link: Link, expectArg: Double?) {
  guard let st = link.status() else { print("No reply to GET_STATUS. Is this the FoxWizard's port? (try: list)"); exit(1) }
  print("FoxWizard answers: tempo \(st.bpm) BPM, clock \(clockWord(st.clock)), sidecar \(st.host ? "present, firmware \(st.fw)" : "not present")")
  if st.clock == 0 {
    print("The clock isn't in use. Load a preset with an arp, set Clock to Auto, and run again."); exit(1)
  }
  let nominal = expectArg ?? Double(st.bpm)
  print("")
  if st.clock == 2 { print("It is following a clock right now. Stop the other clock sources first.") ; exit(1) }
  let sends = link.sendClockOn() ?? true
  func out(_ label: String, expect: Double, seconds: Double) {
    if sends { checkOut(link, label: label, expect: expect, seconds: seconds) }
    else { record("\(label): clock out", false, "skipped, Send Clock is off on the unit. Turn it on in Patchling (Clock, Send Clock) and run again") }
  }
  if !sends { print("Send Clock is OFF on the unit, so it sends no clock bytes. The clock out checks are skipped and count as not passed.\n") }
  out("leading", expect: nominal, seconds: 15)
  print("")
  checkIn(link, bpm: 120, seconds: 10, start: true, label: "120 BPM")
  print("")
  print("Clock stopped: giving it 3 s to take over")
  Thread.sleep(forTimeInterval: 1.6)
  let quiet = pollStatus(link, times: 3)
  record("after the clock stops: it leads at the same tempo", quiet.count >= 2 && quiet.suffix(2).allSatisfy { $0.clock == 4 && abs($0.bpm - 120) <= 1 }, quiet.map { "\(clockWord($0.clock)) \($0.bpm)" }.joined(separator: ", "))
  out("carrying on at 120", expect: 120, seconds: 12)
  print("")
  checkIn(link, bpm: 130, seconds: 8, start: false, label: "a new clock at 130 BPM, no Start")
  print("")
  checkIn(link, bpm: 120, seconds: 8, start: true, label: "120 BPM again with Start")
}

func summary() -> Int32 {
  print("")
  let failed = results.filter { !$0.1 }
  print(failed.isEmpty ? "ALL \(results.count) CHECKS PASS" : "\(failed.count) OF \(results.count) CHECKS FAILED:")
  for f in failed { print("  - \(f.0): \(f.2)") }
  return failed.isEmpty ? 0 : 1
}

// ---- a simulated FoxWizard, to prove the checks can pass and fail ----------------------------------------------------

final class FakeFox {
  var source = MIDIEndpointRef()
  var destination = MIDIEndpointRef()
  let lock = NSLock()
  var errorPpm = 0.0       // how wrong its own clock runs
  var tempo = 120.0        // what it leads at
  var lastExternal = 0.0   // seconds, host time of the last F8 in
  var samples: [(Double, Int)] = []   // (time, clock bytes counted), one every 250 ms
  var count = 0
  var stopped = false
  var inbox = Inbox()
  var sendClock = true
  var running = true
  init() {
    MIDISourceCreate(client, "Fake FoxWizard" as CFString, &source)
    MIDIDestinationCreateWithBlock(client, "Fake FoxWizard" as CFString, &destination) { [self] list, _ in
      let box = Inbox(); readPackets(list, box)
      for r in box.take() { handle(r) }
    }
    let t = Thread { [self] in leadLoop() }
    t.qualityOfService = .userInteractive; t.start()
  }
  func emit(_ bytes: [UInt8], at: UInt64 = 0) {
    var buffer = [UInt8](repeating: 0, count: 1024)
    buffer.withUnsafeMutableBytes { raw in
      let list = raw.baseAddress!.assumingMemoryBound(to: MIDIPacketList.self)
      let p = MIDIPacketListInit(list)
      _ = MIDIPacketListAdd(list, 1024, p, at, bytes.count, bytes)
      MIDIReceived(source, list)
    }
  }
  func following(_ now: Double) -> Bool { !stopped && lastExternal > 0 && now - lastExternal < 0.5 }
  func handle(_ r: Received) {
    let now = r.time
    lock.lock(); defer { lock.unlock() }
    if r.bytes == [0xF8] {
      if lastExternal == 0 || now - lastExternal > 0.5 { samples = []; count = 0 }
      count += 1; lastExternal = now
      if samples.isEmpty || now - samples.last!.0 >= 0.25 { samples.append((now, count)); if samples.count > 16 { samples.removeFirst() } }
      if let a = samples.first, let b = samples.last, b.0 - a.0 >= 1.0 { tempo = 60.0 * Double(b.1 - a.1) / (24.0 * (b.0 - a.0)) }
    } else if r.bytes == [0xFA] { stopped = false } else if r.bytes == [0xFC] { stopped = true }
    else if r.bytes == [0xF0, 0x7D, 0x4D, 0x10, 0xF7] {
      var p = [UInt8](repeating: 0, count: 15); p[11] = sendClock ? 1 : 0
      emit([0xF0, 0x7D, 0x4D, 0x11] + p + [0xF7])
    }
    else if r.bytes == [0xF0, 0x7D, 0x4D, 0x13, 0xF7] {
      var p = [UInt8](repeating: 0, count: 15)
      let bpm = Int(tempo + 0.5); p[0] = 1; p[6] = UInt8(bpm & 0x7F); p[7] = UInt8((bpm >> 7) & 0x7F); p[8] = following(now) ? 2 : 4; p[14] = 8
      emit([0xF0, 0x7D, 0x4D, 0x14] + p + [0xF7])
    }
  }
  func leadLoop() {
    var k = 0, t0 = mach_absolute_time(), lastTempo = 0.0
    while running {
      lock.lock(); let now = ticksToSec(mach_absolute_time()); let lead = !following(now); let bpm = tempo; lock.unlock()
      if bpm != lastTempo { t0 = mach_absolute_time(); k = 0; lastTempo = bpm }
      let interval = 60.0 / (bpm * 24.0) * (1 + errorPpm * 1e-6)
      let due = t0 + secToTicks(Double(k + 1) * interval); k += 1
      mach_wait_until(due - secToTicks(0.004))
      // stamped with the time it is due, and up to 0.3 ms of wobble like a USB frame adds
      if lead && sendClock { emit([0xF8], at: due + secToTicks(Double.random(in: 0...0.0003))) }
    }
  }
}

// ---- a play session --------------------------------------------------------------------------------------------------
//
// Plays into the FoxWizard like a busy player for a while: chords held for different lengths, drum rolls, aftertouch floods like
// a pad controller leaning on its keys, CC and pitch bend sweeps, the requests Patchling makes when it loads, and with more than one
// preset the unit is switched while keys are held. It reads back every note the unit sends and says if any never ended, then
// reports the memory numbers. Nothing is saved on the unit, and it ends on the preset it started on.

struct Lcg {
  var state: UInt64
  mutating func next() -> UInt64 { state = state &* 6364136223846793005 &+ 1442695040888963407; return state >> 33 }
  mutating func int(_ lo: Int, _ hi: Int) -> Int { lo + Int(next() % UInt64(hi - lo + 1)) }
  mutating func double(_ lo: Double, _ hi: Double) -> Double { lo + Double(next() % 100000) / 100000.0 * (hi - lo) }
}

// The main stack is 8,192 bytes with only core 0 running (arduino-pico), the linker reserves 2,048 of them. The dive test in firmware 1.123
// used 6,192 and the unit carried on. Green up to half of it, Yellow up to 6,144 (the depth proven), Red beyond.
let stackSize = 8192
func stackLevel(_ used: Int) -> String { used <= 4096 ? "Green" : (used <= 6144 ? "Yellow" : "RED") }
func stackText(_ used: Int) -> String { "\(used) bytes of \(stackSize) (the linker reserves 2048), \(stackSize - used) free: \(stackLevel(used))" }

struct TimedEvent { let at: Double; let bytes: [UInt8]; let kind: String }

func buildSession(seconds: Double, channels: [UInt8], slots: Int, active: Int, routes: Int, seed: UInt64, live: Bool, liveKeys: [UInt8] = [120, 121, 122], skip: Set<String> = []) -> [TimedEvent] {
  var rng = Lcg(state: seed)
  var events: [TimedEvent] = []
  var busyUntil: [Int: Double] = [:] // channel * 128 + note, so a note is never started twice while it is down
  func note(_ at: Double, _ on: Bool, _ ch: UInt8, _ n: UInt8, _ v: UInt8) {
    events.append(TimedEvent(at: at, bytes: [(on ? 0x90 : 0x80) | ch, n, on ? v : 0], kind: on ? "noteOn" : "noteOff"))
  }
  func free(_ ch: UInt8, _ n: UInt8, _ at: Double) -> Bool { (busyUntil[Int(ch) * 128 + Int(n)] ?? -1) < at }
  var t = 0.5
  var presetAt = 10.0
  var presetStep = 0
  while t < seconds {
    let kind = rng.int(0, 99)
    let ch = channels[rng.int(0, channels.count - 1)]
    if live && rng.int(0, 99) < 30 { // a note trigger on channel 1: Live Ch Up, Down or a direct channel, with keys down
      let key = liveKeys[rng.int(0, liveKeys.count - 1)]
      let at = t + rng.double(0.1, 1.2)
      events.append(TimedEvent(at: at, bytes: [0x90, key, 100], kind: "liveOn"))
      events.append(TimedEvent(at: at + 0.06, bytes: [0x80, key, 0], kind: "liveOff"))
    }
    if (kind < 30 && skip.contains("chord")) || (kind >= 30 && kind < 50 && skip.contains("roll")) || (kind >= 50 && kind < 70 && skip.contains("at")) || (kind >= 70 && kind < 85 && skip.contains("sweep")) || (kind >= 85 && skip.contains("sysex")) {
      t += 0.5
    } else if kind < 30 { // a chord held for a while
      let root = rng.int(36, 72)
      let shape = [[0, 4, 7], [0, 3, 7, 10], [0, 5, 7], [0, 7]][rng.int(0, 3)]
      let hold = rng.double(0.3, 4.0)
      for (i, iv) in shape.enumerated() {
        let n = UInt8(min(127, root + iv)); let at = t + Double(i) * 0.012
        if !free(ch, n, at) { continue }
        busyUntil[Int(ch) * 128 + Int(n)] = at + hold + 0.05
        note(at, true, ch, n, UInt8(rng.int(60, 120))); note(at + hold, false, ch, n, 0)
      }
      t += rng.double(0.2, 1.2)
    } else if kind < 50 { // a drum roll
      let n = UInt8(rng.int(36, 46))
      var at = t
      for _ in 0..<rng.int(8, 24) {
        if free(ch, n, at) {
          busyUntil[Int(ch) * 128 + Int(n)] = at + 0.06
          note(at, true, ch, n, UInt8(rng.int(70, 127))); note(at + 0.05, false, ch, n, 0)
        }
        at += rng.double(0.1, 0.18)
      }
      t += (at - t) * 0.7
    } else if kind < 70 { // a chord with aftertouch flooding while it is held
      let root = rng.int(48, 72)
      let hold = rng.double(2.0, 5.0)
      let rate = Double(rng.int(100, 400))
      var held: [UInt8] = []
      for (i, iv) in [0, 4, 7].enumerated() {
        let n = UInt8(root + iv); let at = t + Double(i) * 0.01
        if !free(ch, n, at) { continue }
        busyUntil[Int(ch) * 128 + Int(n)] = at + hold + 0.05
        note(at, true, ch, n, 100); note(at + hold, false, ch, n, 0); held.append(n)
      }
      var at = t + 0.05
      var k = 0
      while at < t + hold - 0.05 {
        let v = UInt8(40 + 60 * (0.5 + 0.5 * sin(Double(k) * 0.07)))
        if !held.isEmpty {
          events.append(TimedEvent(at: at, bytes: [0xA0 | ch, held[k % held.count], v], kind: "polyAT"))
        }
        events.append(TimedEvent(at: at, bytes: [0xD0 | ch, v], kind: "chanAT"))
        at += 1.0 / rate; k += 1
      }
      t += rng.double(0.5, 2.0)
    } else if kind < 85 { // controller and pitch bend sweeps
      let cc = [1, 74, 11, 7][rng.int(0, 3)]
      let len = rng.double(1.0, 3.0)
      var at = t
      while at < t + len {
        let x = (at - t) / len
        let v = UInt8(127 * (x < 0.5 ? x * 2 : 2 - x * 2))
        events.append(TimedEvent(at: at, bytes: [0xB0 | ch, UInt8(cc), v], kind: "cc"))
        let pb = Int(8192 + 4000 * sin(x * 6.28))
        events.append(TimedEvent(at: at, bytes: [0xE0 | ch, UInt8(pb & 0x7F), UInt8((pb >> 7) & 0x7F)], kind: "bend"))
        at += 0.01
      }
      t += len * 0.5
    } else { // what Patchling asks for when it loads
      var at = t
      func ask(_ cmd: [UInt8]) { events.append(TimedEvent(at: at, bytes: [0xF0, 0x7D, 0x4D] + cmd + [0xF7], kind: "sysex")); at += 0.03 }
      ask([0x10]); ask([0x20])
      for i in 0..<min(routes, 8) { ask([0x22, UInt8(i & 0x7F), UInt8(i >> 7)]) }
      for ty in [11, 12, 13, 3, 4, 5, 6, 7, 8, 9, 10, 14, 15] { ask([0x17, UInt8(ty)]) }
      ask([0x37]); ask([0x13])
      t += 1.0
    }
    // the unit is switched with keys held, when there are presets to switch between
    if slots > 1 && t >= presetAt {
      presetStep += 1
      events.append(TimedEvent(at: t, bytes: [0xF0, 0x7D, 0x4D, 0x3A, UInt8((active + presetStep) % slots), 0xF7], kind: "preset"))
      presetAt += rng.double(9.0, 16.0)
    }
  }
  // back where it started, and a status request every two seconds like Patchling's
  if slots > 1 { events.append(TimedEvent(at: seconds + 1.0, bytes: [0xF0, 0x7D, 0x4D, 0x3A, UInt8(active), 0xF7], kind: "preset")) }
  var tick = 0.0
  while tick < seconds + 6 { events.append(TimedEvent(at: tick, bytes: [0xF0, 0x7D, 0x4D, 0x13, 0xF7], kind: "status")); tick += 2.0 }
  return events.sorted { $0.at < $1.at }
}

// A note's channel and number, when the message is a Note On or Off. A velocity 0 Note On is an Off.
func splitMessages(_ bytes: [UInt8]) -> [[UInt8]] {
  var out: [[UInt8]] = []
  var i = 0
  while i < bytes.count {
    let b = bytes[i]
    let len = b >= 0xF8 ? 1 : (b >= 0xC0 && b < 0xE0) ? 2 : (b >= 0x80 && b < 0xF0) ? 3 : 1
    if i + len <= bytes.count { out.append(Array(bytes[i..<(i + len)])) }
    i += len
  }
  return out
}


func pack7(_ raw: [UInt8]) -> [UInt8] {
  var out: [UInt8] = []
  var i = 0
  while i < raw.count {
    let group = Array(raw[i..<min(i + 7, raw.count)])
    var msbs: UInt8 = 0
    for (j, b) in group.enumerated() { msbs |= ((b >> 7) & 1) << UInt8(j) }
    out.append(msbs)
    for b in group { out.append(b & 0x7F) }
    i += 7
  }
  return out
}

func unpack7(_ packed: [UInt8]) -> [UInt8] {
  var raw: [UInt8] = []
  var k = 0
  while k < packed.count {
    let msbs = packed[k]
    for j in 0..<7 where k + 1 + j < packed.count { raw.append(packed[k + 1 + j] | (((msbs >> UInt8(j)) & 1) << 7)) }
    k += 8
  }
  return raw
}

// Sends a command and waits for the reply with this command number. The reply's payload, or nil.
func ask(_ link: Link, _ cmd: UInt8, _ payload: [UInt8] = [], reply: UInt8, timeout: Double = 0.8) -> [UInt8]? {
  _ = link.drain()
  link.send([0xF0, 0x7D, 0x4D, cmd] + payload + [0xF7])
  let until = Date().addingTimeInterval(timeout)
  while Date() < until {
    for r in link.drain() where r.bytes.count >= 5 && r.bytes[0] == 0xF0 && r.bytes[3] == reply { return Array(r.bytes[4..<(r.bytes.count - 1)]) }
    Thread.sleep(forTimeInterval: 0.005)
  }
  return nil
}

// A test setup for the session, loaded into the unit's memory only, never saved. Made from Patchling's own code (js/midi.js):
// 1 a Round Robin Priority route with a custom order on channels 1-4 with note triggers for Live Ch Up, Down and a direct channel,
// 2 an arp route on channel 10, 3 a Scale and Auto Chord route, 4 a plain Round Robin route, 5 Mono Retrig with Priority.
let testRoutes: [[UInt8]] = [
  [193,1,0,127,0,127,0,127,2,15,0,64,100,0,0,0,94,80,114,105,111,114,105,116,121,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,2,0,1,0,0,0,0,0,0,78,36,0],
  [193,0,2,127,0,127,0,127,2,0,2,64,100,0,0,0,28,65,114,112,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,2,0,0,0,1,0,0,0,0,78,36,0],
  [193,2,0,127,0,127,0,127,2,2,0,64,100,0,0,0,28,83,99,97,108,101,32,99,104,111,114,100,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,2,0,12,0,0,0,21,0,0,78,36,0],
  [193,4,0,127,0,127,0,127,2,112,0,64,100,0,0,0,30,67,121,99,108,101,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,2,0,0,0,0,0,0,0,0,78,36,0],
  [193,8,0,127,0,127,0,127,2,192,0,64,100,0,0,0,95,77,111,110,111,32,112,114,105,111,114,105,116,121,0,0,0,0,0,0,0,0,0,0,0,2,0,2,0,0,0,0,0,0,0,0,78,36,0]
]
let testBlocks: [(type: UInt8, bytes: [UInt8])] = [
  (4, [0,12,1,50,0,0,16,64,0,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]),
  (3, [0,0,0,0,0,0,0,0]),
  (11, [121,13,1,0,0,122,13,0,0,0,123,9,5,0,0,0] + [UInt8](repeating: 0, count: 64)),
  (15, [1,4,32,49])
]

// Loads the test setup, remembering the blocks it replaces. False when the unit already has routes.
func loadTestSetup(_ link: Link, originals: inout [UInt8: [UInt8]], subset: [Int] = [0, 1, 2, 3, 4]) -> Bool {
  for b in testBlocks {
    guard let r = ask(link, 0x17, [b.type], reply: 0x18) else { return false }
    originals[b.type] = unpack7(Array(r.dropFirst()))
  }
  _ = ask(link, 0x28, reply: 0x7E)
  for i in subset { _ = ask(link, 0x24, pack7(testRoutes[i]), reply: 0x7E) }
  for b in testBlocks { _ = ask(link, 0x19, [b.type] + pack7(b.bytes), reply: 0x7E) }
  return true
}

// Back to what the unit had: no routes and its own blocks.
func restoreAfterTestSetup(_ link: Link, originals: [UInt8: [UInt8]]) {
  _ = ask(link, 0x28, reply: 0x7E)
  for (type, bytes) in originals { _ = ask(link, 0x19, [type] + pack7(bytes), reply: 0x7E) }
}

// Records a loop through a route that transposes by +7 and measures where its notes play back, to check the Looper's Quantize Swing
// and that the route's transpose still comes out right, on both sides of Record After Routes. Loaded into memory only, put back after.
// The loop is one bar, recorded with the looper's trigger note on channel 16, five notes played off the grid, quantized to 1/16.
func runLoopSwing(_ link: Link) -> Bool {
  // A fixed 1 bar loop never changes the tempo (a Free take would), the test runs at 120 BPM and puts the unit's own tempo back after
  guard let globalBefore = ask(link, 0x10, reply: 0x11), globalBefore.count >= 15 else { print("No answer for the global settings"); return false }
  var globalTest = globalBefore
  globalTest[7] = 120; globalTest[8] = 0
  _ = ask(link, 0x12, globalTest, reply: 0x7E)
  defer { _ = ask(link, 0x12, globalBefore, reply: 0x7E) }
  let bpm = 120
  guard let saved = ask(link, 0x17, [10], reply: 0x18) else { print("No answer for the looper settings"); return false }
  var originals: [UInt8: [UInt8]] = [10: unpack7(Array(saved.dropFirst()))]
  guard loadTestSetup(link, originals: &originals, subset: []) else { print("could not load the test setup"); return false }
  var route = testRoutes[3]
  route[1] = 1; route[2] = 0 // in on channel 1
  route[9] = 4; route[10] = 0 // out on channel 3
  route[11] = 64 + 7 // transpose +7
  route[16] = 0 // no round robin
  _ = ask(link, 0x24, pack7(route), reply: 0x7E)
  let quantumMs = 60000.0 / Double(bpm) / 4 // 1/16
  let played: [(ms: Double, note: UInt8)] = [(0, 60), (130, 62), (250, 64), (370, 65), (490, 67)]
  let heard: [UInt8] = [67, 69, 71, 72, 74]
  func expected(_ ms: Double, _ pct: Int) -> Double { // the nearest line of the grid, lines at 0, the swung off-beat, and the pair's end
    let pair = 2 * quantumMs, base = (ms / pair).rounded(.down) * pair, x = ms - base
    let swung = Double(pct == 0 ? 50 : pct) * quantumMs / 50
    return base + [0, swung, pair].min { abs($0 - x) < abs($1 - x) }!
  }
  func wait(_ until: Date) { while Date() < until { Thread.sleep(forTimeInterval: 0.0005) } }
  let noteSeconds = (option("--note-ms").flatMap(Double.init) ?? 100) / 1000 // how long each played note is held
  var allGood = true
  print("Loop of 1 bar, quantize 1/16 (\(String(format: "%.1f", quantumMs)) ms at \(bpm) BPM), route in 1 to out 3 with transpose +7. Offsets are from the first note of each pass")
  for recordInputs: UInt8 in [0, 1] {
    for pct in [0, 66, 75] {
      let block: [UInt8] = [1, args.contains("--no-quantize") ? 0 : 12, 0, recordInputs, 0, 0, 1, 15, 120, 0, 0, UInt8(pct)] + [UInt8](repeating: 0, count: 36)
      _ = ask(link, 0x19, [10] + pack7(block), reply: 0x7E)
      _ = link.drain()
      link.send([0x9F, 120, 100]); Thread.sleep(forTimeInterval: 0.05); link.send([0x8F, 120, 0]) // arm
      Thread.sleep(forTimeInterval: 1.3)
      let start = Date()
      // Ons and Offs in time order, so notes that overlap are each played at their own time and length
      let sends = played.flatMap { n in [(n.ms / 1000, [UInt8(0x90), n.note, 100]), (n.ms / 1000 + noteSeconds, [UInt8(0x80), n.note, 0])] }.sorted { $0.0 < $1.0 }
      for (at, bytes) in sends { wait(start.addingTimeInterval(at)); link.send(bytes) }
      // the bar ends the take by itself, 2 s after the first note at 120 BPM, and the loop plays
      var events: [(time: Double, note: UInt8, on: Bool)] = []
      var until = Date().addingTimeInterval(10.0)
      while Date() < until { for r in link.drain() where r.bytes.count == 3 && (r.bytes[0] == 0x92 || r.bytes[0] == 0x82) { events.append((r.time, r.bytes[1], r.bytes[0] == 0x92 && r.bytes[2] > 0)) }; Thread.sleep(forTimeInterval: 0.002) }
      Thread.sleep(forTimeInterval: 0.3)
      link.send([0x9F, 120, 100]); Thread.sleep(forTimeInterval: 1.0); link.send([0x8F, 120, 0]) // hold: clears the loop
      until = Date().addingTimeInterval(1.5)
      while Date() < until { for r in link.drain() where r.bytes.count == 3 && (r.bytes[0] == 0x92 || r.bytes[0] == 0x82) { events.append((r.time, r.bytes[1], r.bytes[0] == 0x92 && r.bytes[2] > 0)) }; Thread.sleep(forTimeInterval: 0.002) }
      if args.contains("--verbose"), let t0 = events.first?.time {
        print("  \(recordInputs == 1 ? "inputs" : "after routes") \(pct)%: " + events.prefix(40).map { String(format: "%@%d@%.0f", $0.on ? "+" : "-", $0.note, ($0.time - t0) * 1000) }.joined(separator: " "))
      }
      let ons = events.filter { $0.on }, offs = events.filter { !$0.on }
      let loopOns = Array(ons.dropFirst(5)) // the first five are the notes as played, passing through
      var offsets = [[Double]](repeating: [], count: 5), notesRight = true, passes = 0, starts: [Double] = []
      var i = 0
      while i + 5 <= loopOns.count {
        let chunk = Array(loopOns[i..<(i + 5)])
        if chunk.map({ $0.note }) != heard { notesRight = false }
        for k in 0..<5 { offsets[k].append((chunk[k].time - chunk[0].time) * 1000) }
        starts.append(chunk[0].time); passes += 1; i += 5
      }
      let label = "\(recordInputs == 1 ? "records inputs" : "records after routes"), swing \(pct == 0 ? "off" : "\(pct)%")"
      guard passes >= 3 else { print("FAIL  \(label): only \(passes) full passes heard (\(ons.count) Note Ons)"); allGood = false; continue }
      let means = offsets.map { $0.reduce(0, +) / Double($0.count) }
      let want = played.map { expected($0.ms, pct) - expected(0, pct) }
      let worst = zip(means, want).map { abs($0 - $1) }.max()!
      let length = (starts.last! - starts.first!) / Double(starts.count - 1) * 1000
      // each note keeps the length it was played, Quantize moves only its start
      var lengths: [Double] = []
      for on in loopOns { if let off = offs.first(where: { $0.note == on.note && $0.time > on.time }) { lengths.append((off.time - on.time) * 1000) } }
      let meanLength = lengths.isEmpty ? 0 : lengths.reduce(0, +) / Double(lengths.count)
      let lengthOk = abs(meanLength - noteSeconds * 1000) < 4
      let ok = notesRight && worst < 3.0 && ons.count == offs.count && lengthOk
      if !ok { allGood = false }
      print("\(ok ? "PASS" : "FAIL")  \(label): \(passes) passes, notes \(notesRight ? "67 69 71 72 74 (transpose once)" : "WRONG: \(loopOns.prefix(5).map { String($0.note) }.joined(separator: " "))"), at " + means.map { String(format: "%.0f", $0) }.joined(separator: " ") + " ms, wanted " + want.map { String(format: "%.0f", $0) }.joined(separator: " ") + String(format: ", worst %.1f ms, pass %.0f ms, notes last %.0f ms%@, ", worst, length, meanLength, lengthOk ? "" : " (played " + String(format: "%.0f", noteSeconds * 1000) + ")") + "\(ons.count) on / \(offs.count) off")
    }
  }
  restoreAfterTestSetup(link, originals: originals)
  return allGood
}

// Round Robin Velocity (1.119, protocol 7): a route with three output channels, ranked 3, 6 and 10, sends each note to a channel by its velocity.
// Played softest to hardest at three Velocity % settings, and a key pressed again while down. Loaded into memory only, put back after.
func runRrVelocity(_ link: Link) -> Bool {
  var originals: [UInt8: [UInt8]] = [:]
  guard loadTestSetup(link, originals: &originals, subset: []) else { print("could not load the test setup"); return false }
  let order: [UInt8] = [2, 5, 9] // 0 based channels in rank order, 3, 6, 10
  func band(_ v: Int) -> Int { ((max(1, min(127, v)) - 1) * order.count) / 127 }
  var allGood = true
  func setUp(scale: Int) {
    _ = ask(link, 0x28, reply: 0x7E)
    var route = testRoutes[3]
    route[1] = 1; route[2] = 0 // in on channel 1
    route[9] = 36; route[10] = 2 // out on channels 3, 6 and 10
    route[11] = 64; route[12] = UInt8(scale)
    route[16] = 158 // Round Robin, Velocity, the usual pass through bits
    _ = ask(link, 0x24, pack7(route), reply: 0x7E)
    _ = ask(link, 0x19, [15] + pack7([1, 3, 0x25, 0x90]), reply: 0x7E) // the rank order 3, 6, 10
  }
  func play(_ bytes: [UInt8], wait: Double = 0.06) -> [(on: Bool, ch: UInt8, note: UInt8, vel: UInt8)] {
    _ = link.drain()
    link.send(bytes)
    Thread.sleep(forTimeInterval: wait)
    return link.drain().filter { $0.bytes.count == 3 && ($0.bytes[0] & 0xE0) == 0x80 }.map { (($0.bytes[0] & 0xF0) == 0x90 && $0.bytes[2] > 0, $0.bytes[0] & 0x0F, $0.bytes[1], $0.bytes[2]) }
  }
  for scale in [100, 200, 50] {
    setUp(scale: scale)
    var wrong: [String] = []
    var landed: [String] = []
    for vel in [1, 10, 43, 44, 64, 85, 86, 100, 127] {
      let on = play([0x90, 60, UInt8(vel)])
      let off = play([0x80, 60, 0])
      let scaled = max(1, min(127, vel * scale / 100))
      let want = order[band(scaled)]
      let ok = on.count == 1 && on[0].on && on[0].ch == want && Int(on[0].vel) == scaled && off.count == 1 && !off[0].on && off[0].ch == want
      landed.append("\(vel)>ch\(on.first.map { Int($0.ch) + 1 } ?? 0)")
      if !ok { wrong.append("vel \(vel) wanted ch\(want + 1) at \(scaled), got \(on.map { "\($0.on ? "on" : "off") ch\(Int($0.ch) + 1) v\($0.vel)" })") }
    }
    if !wrong.isEmpty { allGood = false }
    print("\(wrong.isEmpty ? "PASS" : "FAIL")  Velocity % \(scale): " + landed.joined(separator: " ") + (wrong.isEmpty ? "" : "   " + wrong.joined(separator: "; ")))
  }
  // a key pressed again while down: it lets go where it was, then sounds on its new channel, and its Off follows
  setUp(scale: 100)
  _ = link.drain()
  link.send([0x90, 60, 20]); Thread.sleep(forTimeInterval: 0.05)
  link.send([0x90, 60, 120]); Thread.sleep(forTimeInterval: 0.05)
  link.send([0x80, 60, 0]); Thread.sleep(forTimeInterval: 0.08)
  let seq = link.drain().filter { $0.bytes.count == 3 && ($0.bytes[0] & 0xE0) == 0x80 }.map { "\(($0.bytes[0] & 0xF0) == 0x90 && $0.bytes[2] > 0 ? "+" : "-")ch\(Int($0.bytes[0] & 0x0F) + 1)" }
  let again = seq == ["+ch3", "-ch3", "+ch10", "-ch10"]
  if !again { allGood = false }
  print("\(again ? "PASS" : "FAIL")  pressed again while down: \(seq.joined(separator: " ")) (wanted +ch3 -ch3 +ch10 -ch10)")
  restoreAfterTestSetup(link, originals: originals)
  return allGood
}

// Four arps at once. Each arp slot gets its own route, input channel, chord and output channel. Each is first played alone for a reference,
// then all four together, and every one must play the same pattern as alone, with its own steps the same distance apart. This is the check
// that the arps do not share anything they should not (scratch buffers, state). Loaded into memory only, put back after.
func runFourArps(_ link: Link) -> Bool {
  var originals: [UInt8: [UInt8]] = [:]
  guard loadTestSetup(link, originals: &originals, subset: []) else { print("could not load the test setup"); return false }
  struct Arp { let slot: UInt8, inCh: UInt8, outCh: UInt8, chord: [UInt8], mode: UInt8, division: UInt8, octaves: UInt8 }
  // modes 0 Up, 1 Down, 5 Random is left out, it has no pattern. Divisions are fw::MusicalDivision + 1: 12 a 16th, 9 an 8th, 14 a 16th triplet.
  let arps = [
    Arp(slot: 1, inCh: 0, outCh: 10, chord: [60, 64, 67], mode: 0, division: 12, octaves: 1),
    Arp(slot: 2, inCh: 1, outCh: 11, chord: [48, 52, 55, 59], mode: 1, division: 9, octaves: 2),
    Arp(slot: 3, inCh: 2, outCh: 12, chord: [72, 76, 79], mode: 0, division: 12, octaves: 3),
    Arp(slot: 4, inCh: 3, outCh: 13, chord: [36, 43, 40, 47, 41], mode: 1, division: 14, octaves: 1),
  ]
  func load(_ active: [Arp]) {
    _ = ask(link, 0x28, reply: 0x7E)
    for a in arps {
      var block = testBlocks[0].bytes
      block[0] = a.mode; block[1] = a.division; block[2] = a.octaves
      _ = ask(link, 0x19, [3 + a.slot] + pack7(block), reply: 0x7E)
    }
    for a in active {
      var route = testRoutes[1]
      let inMask = UInt16(1) << UInt16(a.inCh), outMask = UInt16(1) << UInt16(a.outCh)
      route[1] = UInt8(inMask & 0xFF); route[2] = UInt8(inMask >> 8)
      route[9] = UInt8(outMask & 0xFF); route[10] = UInt8(outMask >> 8)
      route[47] = a.slot
      _ = ask(link, 0x24, pack7(route), reply: 0x7E)
    }
  }
  // plays the chords of the given arps for a while and returns each output channel's Note Ons with their times
  func play(_ active: [Arp], seconds: Double) -> [UInt8: [(time: Double, note: UInt8)]] {
    load(active)
    _ = link.drain()
    for a in active { for n in a.chord { link.send([0x90 | a.inCh, n, 100]) } }
    var got: [UInt8: [(time: Double, note: UInt8)]] = [:]
    let until = Date().addingTimeInterval(seconds)
    while Date() < until { for r in link.drain() where r.bytes.count == 3 && (r.bytes[0] & 0xF0) == 0x90 && r.bytes[2] > 0 { got[r.bytes[0] & 0x0F, default: []].append((r.time, r.bytes[1])) }; Thread.sleep(forTimeInterval: 0.002) }
    for a in active { for n in a.chord { link.send([0x80 | a.inCh, n, 0]) } }
    Thread.sleep(forTimeInterval: 0.4)
    _ = link.drain()
    return got
  }
  func period(_ s: [UInt8]) -> Int { (1...max(1, s.count / 2)).first { p in (0..<(s.count - p)).allSatisfy { s[$0] == s[$0 + p] } } ?? s.count }
  var allGood = true
  var solo: [UInt8: [UInt8]] = [:], soloGap: [UInt8: Double] = [:]
  for a in arps {
    let r = play([a], seconds: 6.0)[a.outCh] ?? []
    let notes = r.dropFirst(2).map { $0.note }
    guard notes.count >= 12 else { print("FAIL  arp \(a.slot) alone: only \(notes.count) notes heard"); allGood = false; continue }
    solo[a.outCh] = Array(notes)
    let times = r.dropFirst(2).map { $0.time }
    soloGap[a.outCh] = (times.last! - times.first!) / Double(times.count - 1)
  }
  let together = play(arps, seconds: 8.0)
  for a in arps {
    guard let ref = solo[a.outCh], let gap = soloGap[a.outCh] else { continue }
    let p = period(ref)
    let cycle = Array(ref.prefix(p))
    let r = Array((together[a.outCh] ?? []).dropFirst(2))
    guard r.count >= 12 else { print("FAIL  arp \(a.slot) with the others: only \(r.count) notes heard"); allGood = false; continue }
    let k = cycle.firstIndex(of: r[0].note) ?? 0
    let wrong = r.enumerated().filter { $0.element.note != cycle[(k + $0.offset) % p] }.count
    let gaps = (0..<(r.count - 1)).map { r[$0 + 1].time - r[$0].time }
    let mean = gaps.reduce(0, +) / Double(gaps.count)
    let worstGap = gaps.map { abs($0 - mean) }.max()!
    let ok = wrong == 0 && abs(mean - gap) < 0.003 && worstGap < 0.006
    if !ok { allGood = false }
    print(String(format: "%@  arp %d (ch %d, %@ pattern of %d notes): %d steps, %d off pattern, steps %.1f ms apart (alone %.1f), worst %.1f ms", ok ? "PASS" : "FAIL", Int(a.slot), Int(a.outCh) + 1, "repeating", p, r.count, wrong, mean * 1000, gap * 1000, worstGap * 1000))
  }
  restoreAfterTestSetup(link, originals: originals)
  return allGood
}

// Soak test, two phases run separately so the TRS loopback can be plugged in for the second.
//   a: the deep path. Every feature at once, from a RAM-only setup: Priority and Round Robin routes, scale and chord, four arps, the looper recording
//      and clearing, floods of aftertouch, CC and bend. No saves and no preset switches, since a save would write the test routes into the unit.
//   b: saves and preset switches under flood, on the unit's own state, in adaptor mode, nothing of ours loaded. With the TRS loopback in, the DIN
//      input also hears the unit's own output, extra receive interrupts while it saves. Not for phase a, the loopback feeds routes and clock back.
// Each minute one line: stack deepest, heap lowest, longest main loop pass, restarts seen, stuck notes. Ends with a verdict.
func runSoak(_ link: Link, phase: String, minutes: Double) -> Bool {
  guard let before = link.memory() else { print("No answer from the unit"); return false }
  var originals: [UInt8: [UInt8]] = [:]
  let heavy = phase == "a"
  var globalBeforeB: [UInt8]? = nil
  if heavy {
    guard loadTestSetup(link, originals: &originals, subset: args.contains("--out-trs") ? [] : [0, 1, 2, 3, 4]) else { print("could not load the test setup"); return false }
    if args.contains("--out-trs") { // the same five routes, their output on the DIN jack only, so nothing flows back out over USB
      for r in testRoutes { var t = r; t[8] = 1; t[43] = 1; t[44] = 0; _ = ask(link, 0x24, pack7(t), reply: 0x7E) }
    }
    // three more arps on input channels 5 to 7, out on 11 to 13, and the looper with its trigger note on channel 16
    for (k, slot) in ((args.contains("--no-extra-arps") ? [] : [UInt8(2), 3, 4]) as [UInt8]).enumerated() {
      var block = testBlocks[0].bytes
      block[0] = UInt8(k % 2); block[1] = [12, 9, 14][k]; block[2] = UInt8(k + 1)
      _ = ask(link, 0x19, [3 + slot] + pack7(block), reply: 0x7E)
      var route = testRoutes[1]
      route[1] = UInt8(1 << (4 + k)); route[2] = 0
      route[9] = 0; route[10] = UInt8(1 << (2 + k))
      route[47] = slot
      _ = ask(link, 0x24, pack7(route), reply: 0x7E)
    }
    if let saved = ask(link, 0x17, [10], reply: 0x18) { originals[10] = unpack7(Array(saved.dropFirst())) }
    if !args.contains("--no-looper") { _ = ask(link, 0x19, [10] + pack7([1, 12, 0, 0, 0, 0, 1, 15, 120, 0, 0, 66] + [UInt8](repeating: 0, count: 36)), reply: 0x7E) }
    print("Phase a: the deep path. Test setup in memory only, nothing is saved")
  } else {
    print("Phase b: saves and preset switches under flood, on the unit's own state. Nothing of ours is loaded")
    // With the TRS loopback in, Router mode and no routes send everything the unit hears straight back out its own port, an endless loop that
    // holds the USB input back and makes the unit look dead. Adaptor mode never sends an input back to its own port. Set in memory, put back after.
    if let g = ask(link, 0x10, reply: 0x11), g.count >= 15, g[0] != 0 {
      globalBeforeB = g
      var adaptor = g; adaptor[0] = 0
      _ = ask(link, 0x12, adaptor, reply: 0x7E)
      print("The unit was in Router mode, set to Adaptor in memory for this test and put back after")
    }
  }
  defer { if let g = globalBeforeB { _ = ask(link, 0x12, g, reply: 0x7E) } }
  var outstanding: [Int: Int] = [:]
  var ons = 0, offs = 0, slots = 1, statusReplies = 0, restarts = 0, noAnswer = 0, saves = 0, switches = 0
  var stack = before.stackDeepest, heapKB = before.heapLowest / 1024, loopUs = 0, heldBack = 0, worstLoopUs = 0, minHeapKB = before.heapLowest / 1024
  var crash = 0, fault = false
  var routesExpected = heavy ? (args.contains("--no-extra-arps") ? 5 : 8) : 0
  var lastStatusAt = Date()
  func pump() {
    for r in link.drain() {
      let b = r.bytes
      if b.first == 0xF0 {
        guard b.count > 5, b[1] == 0x7D else { continue }
        if b[3] == 0x38, b.count > 8 { slots = Int(b[4]) }
        if b[3] == 0x21, b.count > 6 { // the route count: a restart wipes the RAM-only test routes, which the counter alone can miss
          let n = Int(b[4]) | (Int(b[5]) << 7)
          if heavy && n < routesExpected { restarts += 1; routesExpected = n; print(String(format: "  RESTART seen %.1f s in, the test routes are gone (%d left)", Date().timeIntervalSince(start), n)) }
        }
        if b[3] == 0x14, b.count > 4 + 42 {
          let p = Array(b[4..<(b.count - 1)])
          statusReplies += 1; lastStatusAt = Date()
          let deep = (Int(p[41]) | (Int(p[42]) << 7)) * 16
          if deep < stack { restarts += 1; print(String(format: "  RESTART seen %.1f s in, the stack counter went from %d to %d, last watchdog stage %d", Date().timeIntervalSince(start), stack, deep, Int(p[30]) | (Int(p[31]) << 7))) } // the counter only goes up, unless the unit restarted
          stack = deep
          heapKB = Int(p[28]) * 512 / 1024; minHeapKB = min(minHeapKB, heapKB)
          loopUs = Int(p[15]) | (Int(p[16]) << 7); worstLoopUs = max(worstLoopUs, loopUs); heldBack += Int(p[18])
          crash = Int(p[30]) | (Int(p[31]) << 7); fault = p[36] & 0x40 != 0
        }
      } else if b.count == 3, (b[0] & 0xE0) == 0x80 {
        let key = Int(b[0] & 0x0F) * 128 + Int(b[1])
        if (b[0] & 0xF0) == 0x90, b[2] > 0 { outstanding[key, default: 0] += 1; ons += 1 } else { outstanding[key, default: 0] -= 1; offs += 1 }
      }
    }
  }
  func wait(_ seconds: Double) { let until = Date().addingTimeInterval(seconds); while Date() < until { pump(); Thread.sleep(forTimeInterval: 0.002) } }
  func tap(_ hold: Double = 0.05) { link.send([0x9F, 120, 100]); wait(hold); link.send([0x8F, 120, 0]) }
  var tick = 0
  let floodChannels = option("--flood-channels").map { $0.split(separator: ",").compactMap { UInt8($0) }.map { $0 - 1 } } // 1 based
  let floodTypes = option("--flood-types") ?? "abc" // a poly aftertouch, c channel pressure, b pitch bend, m CC
  // A flood into the USB port with routes sending it back out over USB restarts the unit at about 1,000 messages a second and up, in every firmware
  // tried back to 1.110. Phase a stays under that by default, 4 of each every 20 ms is 600 a second. Phase b's flood goes to the DIN jack only.
  let floodTriples = option("--flood-triples").flatMap(Int.init) ?? (heavy ? 4 : 20)
  // one 20 ms slice of flood
  func floodSlice(_ channels: [UInt8]) {
    tick += 1
    for k in 0..<floodTriples {
      let ch = channels[(tick + k) % channels.count]
      if floodTypes.contains("a") { link.send([0xA0 | ch, UInt8(36 + (tick + k) % 60), UInt8(1 + (tick * 7 + k * 13) % 126)]) }
      if floodTypes.contains("c") { link.send([0xD0 | ch, UInt8(1 + (tick * 5 + k * 3) % 126)]) }
      if floodTypes.contains("b") { link.send([0xE0 | ch, UInt8((tick * 11 + k) % 128), UInt8((tick + k * 3) % 128)]) }
      if floodTypes.contains("m") { link.send([0xB0 | ch, UInt8(1 + (tick + k) % 30), UInt8((tick * 3 + k) % 128)]) }
    }
  }
  let chords: [(ch: UInt8, notes: [UInt8])] = [(0, [60, 64, 67]), (9, [48, 52, 55]), (1, [62, 65, 69]), (2, [50, 54, 57, 61]), (3, [40, 44, 47]), (4, [60, 64, 67]), (5, [48, 52, 55, 59]), (6, [72, 76, 79])]
  let start = Date()
  let end = start.addingTimeInterval(minutes * 60)
  var nextLine = start.addingTimeInterval(60), nextStatusAsk = start
  var cycle = 0
  print("Running for \(Int(minutes)) minutes. Each line: minutes in, stack deepest, heap lowest, longest main loop pass, restarts, notes heard, stuck")
  link.send([0xF0, 0x7D, 0x4D, 0x37, 0xF7]); wait(0.5)
  func line() {
    let stuck = outstanding.filter { $0.value > 0 }.count
    print(String(format: "%4.1f min  stack %5d  heap %2d KB  loop %5d us  restarts %d  notes %d  stuck %d  saves %d  switches %d", Date().timeIntervalSince(start) / 60, stack, minHeapKB, worstLoopUs, restarts, ons, stuck, saves, switches))
  }
  while Date() < end {
    cycle += 1
    let cycleStart = Date()
    let length: Double = heavy ? 60 : 30
    var done: Set<Int> = []
    var floodOn = false
    func at(_ t: Double, _ id: Int, _ action: () -> Void) { if !done.contains(id), Date().timeIntervalSince(cycleStart) >= t { done.insert(id); action() } }
    while Date().timeIntervalSince(cycleStart) < length && Date() < end {
      if Date() >= nextStatusAsk { link.send([0xF0, 0x7D, 0x4D, 0x13, 0xF7]); link.send([0xF0, 0x7D, 0x4D, 0x20, 0xF7]); nextStatusAsk = Date().addingTimeInterval(2) } // as Patchling polls
      if heavy {
        at(0.2, 1) { for c in chords { for n in c.notes { link.send([0x90 | c.ch, n, 90]) } } }
        at(0.5, 2) { floodOn = !args.contains("--no-flood") }
        at(8, 3) { if !args.contains("--no-looper") { tap() } }                                                    // arm the looper, the next note starts the take
        at(9, 4) { if !args.contains("--no-looper") { for n in [60, 62, 64, 65, 67] as [UInt8] { link.send([0x90, n, 100]); wait(0.18); link.send([0x80, n, 0]); wait(0.08) } } }
        at(38, 5) { if !args.contains("--no-looper") { link.send([0x9F, 120, 100]); wait(1.0); link.send([0x8F, 120, 0]) } } // hold clears the loop
        at(50, 6) { floodOn = false; for c in chords { for n in c.notes { link.send([0x80 | c.ch, n, 0]) } } }
      } else {
        at(0.2, 1) { floodOn = true }
        at(5, 2) { if slots > 1 { _ = ask(link, 0x3A, [1], reply: 0x7E, timeout: 3.0); switches += 1 } }
        at(10, 3) { let a = ask(link, 0x31, reply: 0x7E, timeout: 5.0); if a == nil { noAnswer += 1 } else { saves += 1 } }
        at(15, 4) { if slots > 1 { _ = ask(link, 0x3A, [0], reply: 0x7E, timeout: 3.0); switches += 1 } }
        at(20, 5) { let a = ask(link, 0x31, reply: 0x7E, timeout: 5.0); if a == nil { noAnswer += 1 } else { saves += 1 } }
        at(25, 6) { for t in [3, 4, 5, 8, 9, 10, 11, 14, 15] as [UInt8] { _ = ask(link, 0x17, [t], reply: 0x18, timeout: 1.0) } } // reads blocks like Patchling on connect
        at(27, 7) { floodOn = false }
      }
      if floodOn { floodSlice(floodChannels ?? (heavy ? [0, 1, 2, 3, 9] : [0, 1, 2])) }
      wait(0.02)
      if Date() >= nextLine { line(); nextLine = nextLine.addingTimeInterval(60) }
      if Date().timeIntervalSince(lastStatusAt) > 8 && statusReplies > 0 { noAnswer += 1; lastStatusAt = Date() }
    }
    floodOn = false
    wait(2.0)
  }
  if heavy { for c in chords { for n in c.notes { link.send([0x80 | c.ch, n, 0]) } } }
  wait(3.0)
  line()
  let stuck = outstanding.filter { $0.value > 0 }
  if heavy, let rc = ask(link, 0x20, reply: 0x21, timeout: 1.0), rc.count > 1, (Int(rc[0]) | (Int(rc[1]) << 7)) < routesExpected { restarts += 1; print("  RESTART seen at the end, the test routes are gone") }
  if heavy { restoreAfterTestSetup(link, originals: originals) }
  if let g = link.memory() {
    if g.stackDeepest < stack { restarts += 1; print("  RESTART seen at the end, the stack counter went from \(stack) to \(g.stackDeepest)") }
    stack = max(stack, g.stackDeepest)
  }
  let ok = restarts == 0 && stuck.isEmpty && noAnswer == 0 && crash == 0 && !fault && stack <= 6144 && minHeapKB >= 20
  print("SOAK PHASE \(phase.uppercased()): \(ok ? "PASS" : "LOOK AT THE LINES ABOVE"). \(Int(minutes)) minutes, \(cycle) cycles, notes heard \(ons), stuck \(stuck.count), restarts \(restarts), no answer \(noAnswer), watchdog stage \(crash), hard fault \(fault ? "YES" : "no"), longest loop pass \(worstLoopUs) us, input held back \(heldBack) times, stack \(stackText(stack)), heap lowest \(minHeapKB) KB")
  return ok
}

func runSession(_ link: Link, seconds: Double, channels: [UInt8], useSetup: Bool) {
  guard let id = link.identity() else { print("No answer from the FoxWizard"); exit(1) }
  guard let before = link.memory() else { print("This firmware has no memory readout, update to 1.108 or later"); exit(1) }
  print("FoxWizard firmware \(id.major).\(id.minor). Memory before: free heap lowest \(before.heapLowest / 1024) KB, main stack deepest \(stackText(before.stackDeepest))")
  // which presets there are
  _ = link.drain()
  link.send([0xF0, 0x7D, 0x4D, 0x37, 0xF7])
  var slots = 1, active = 0, routes = 0
  var until = Date().addingTimeInterval(0.8)
  while Date() < until {
    for r in link.drain() where r.bytes.count > 8 && r.bytes[3] == 0x38 { slots = Int(r.bytes[4]); active = Int(r.bytes[5]) }
    Thread.sleep(forTimeInterval: 0.01)
  }
  link.send([0xF0, 0x7D, 0x4D, 0x20, 0xF7])
  until = Date().addingTimeInterval(0.8)
  while Date() < until {
    for r in link.drain() where r.bytes.count > 6 && r.bytes[3] == 0x21 { routes = Int(r.bytes[4]) | (Int(r.bytes[5]) << 7) }
    Thread.sleep(forTimeInterval: 0.01)
  }
  var usbRoute = false
  for i in 0..<min(routes, 12) {
    link.send([0xF0, 0x7D, 0x4D, 0x22, UInt8(i & 0x7F), UInt8(i >> 7), 0xF7])
    let waitUntil = Date().addingTimeInterval(0.6)
    while Date() < waitUntil {
      var got = false
      for r in link.drain() where r.bytes.count > 12 && r.bytes[3] == 0x23 {
        var raw: [UInt8] = []
        let packed = Array(r.bytes[6..<(r.bytes.count - 1)])
        var k = 0
        while k < packed.count {
          let msbs = packed[k]
          for j in 0..<7 where k + 1 + j < packed.count { raw.append(packed[k + 1 + j] | (((msbs >> UInt8(j)) & 1) << 7)) }
          k += 8
        }
        if raw.count >= 45 {
          let outPorts = Int(raw[43]) | (Int(raw[44]) << 8), inPorts = Int(raw[41]) | (Int(raw[42]) << 8)
          let inCh = Int(raw[1]) | (Int(raw[2]) << 8), outCh = Int(raw[9]) | (Int(raw[10]) << 8)
          func names(_ m: Int) -> String { ["TRS", "USB", "Host1", "Host2", "Host3", "Host4", "TRS2", "TRS3", "TRS4", "TRS5", "TRS6", "Host5", "Host6", "Host7"].enumerated().filter { m & (1 << $0.offset) != 0 }.map { $0.element }.joined(separator: "+") }
          func chans(_ m: Int) -> String { m == 0xFFFF ? "all" : (0..<16).filter { m & (1 << $0) != 0 }.map { String($0 + 1) }.joined(separator: ",") }
          print("  route \(i + 1): in \(names(inPorts)) ch \(chans(inCh)) -> out \(names(outPorts)) ch \(chans(outCh))")
          if outPorts & 2 != 0 { usbRoute = true }
        }
        got = true
      }
      if got { break }
      Thread.sleep(forTimeInterval: 0.01)
    }
  }
  print("It has \(routes) routes and \(slots) preset slot\(slots == 1 ? "" : "s"), on slot \(active + 1). Playing for \(Int(seconds)) seconds on channels \(channels.map { String($0 + 1) }.joined(separator: ", "))\(slots > 1 ? ", switching presets with keys held, ending on slot \(active + 1)" : "")")
  var originals: [UInt8: [UInt8]] = [:]
  var setupLoaded = false
  let subset = (option("--only-routes") ?? "1,2,3,4,5").split(separator: ",").compactMap { Int($0) }.map { $0 - 1 }.filter { $0 >= 0 && $0 < 5 }
  if useSetup && routes == 0 {
    setupLoaded = loadTestSetup(link, originals: &originals, subset: subset)
    print(setupLoaded ? "Test setup loaded into memory (5 routes, nothing saved, restored at the end): Priority with a custom order and Live Ch triggers on ch 1, arp on 10, scale and chord on 2, Round Robin on 3, mono with Priority on 4" : "Could not load the test setup, running without it")
  } else if useSetup {
    print("The unit has routes, so the test setup is not loaded, nothing of yours is touched")
  }
  let events = buildSession(seconds: seconds, channels: setupLoaded ? subset.map { [0, 9, 1, 2, 3][$0] } : channels, slots: slots, active: active, routes: routes, seed: 20261029, live: setupLoaded && !args.contains("--no-live"), liveKeys: (option("--live-keys") ?? "120,121,122").split(separator: ",").compactMap { UInt8($0) }, skip: Set((option("--skip") ?? "").split(separator: ",").map(String.init)))
  var sent: [String: Int] = [:]
  var received: [String: Int] = [:]
  var outstanding: [Int: Int] = [:]
  var maxLoopUs = 0, heldBack = 0, maxUsbPct = 0, maxLinkPct = 0, maxTrsPct = 0
  var lowHeap = before.heapLowest, deepStack = before.stackDeepest
  var statusReplies = 0, acks = 0
  func handle(_ r: Received) {
    if r.bytes.first == 0xF0 {
      guard r.bytes.count > 5, r.bytes[1] == 0x7D else { return }
      if r.bytes[3] == 0x7E { acks += 1 }
      if r.bytes[3] == 0x14, r.bytes.count > 34 {
        statusReplies += 1
        let p = Array(r.bytes[4..<(r.bytes.count - 1)])
        maxLoopUs = max(maxLoopUs, Int(p[15]) | (Int(p[16]) << 7))
        heldBack += Int(p[18]); maxLinkPct = max(maxLinkPct, Int(p[19]) * 100 / 127); maxUsbPct = max(maxUsbPct, Int(p[21]) * 100 / 127); maxTrsPct = max(maxTrsPct, Int(p[20]) * 100 / 127)
        lowHeap = min(lowHeap, Int(p[28]) * 512); deepStack = max(deepStack, Int(p[29]) * 16)
      }
      return
    }
    for m in splitMessages(r.bytes) {
      let hi = m[0] & 0xF0
      if m[0] >= 0xF8 { continue }
      if (hi == 0x90 || hi == 0x80) && m.count == 3 {
        let key = Int(m[0] & 0x0F) * 128 + Int(m[1])
        if hi == 0x90 && m[2] > 0 { outstanding[key, default: 0] += 1; received["noteOn", default: 0] += 1 }
        else { outstanding[key, default: 0] -= 1; received["noteOff", default: 0] += 1 }
      } else { received[hi == 0xA0 ? "polyAT" : hi == 0xD0 ? "chanAT" : hi == 0xB0 ? "cc" : hi == 0xE0 ? "bend" : "other", default: 0] += 1 }
    }
  }
  _ = link.drain()
  let quietStart = Date().addingTimeInterval(0.5)
  while Date() < quietStart { for r in link.drain() { handle(r) }; Thread.sleep(forTimeInterval: 0.01) }
  if !received.isEmpty { print("Note: the unit was already sending MIDI before the session started, a loop or an arp may be running, stuck note results will not be clean") }
  received = [:]; outstanding = [:]
  let t0 = mach_absolute_time()
  var index = 0
  var nextReport = 10.0
  while index < events.count {
    let e = events[index]
    let due = t0 + secToTicks(e.at)
    while mach_absolute_time() < due {
      for r in link.drain() { handle(r) }
      Thread.sleep(forTimeInterval: 0.002)
    }
    link.send(e.bytes)
    sent[e.kind, default: 0] += 1
    index += 1
    let now = ticksToSec(mach_absolute_time() - t0)
    if now >= nextReport { print(String(format: "  %.0f s of %.0f s", now, seconds)); nextReport += 10 }
  }
  // let arps and gates finish, then every note should be down
  let settle = Date().addingTimeInterval(3.5)
  while Date() < settle { for r in link.drain() { handle(r) }; Thread.sleep(forTimeInterval: 0.01) }
  guard let after = link.memory() else { print("No memory reply at the end"); exit(1) }
  var routesLeft = -1
  if setupLoaded {
    routesLeft = ask(link, 0x20, reply: 0x21).map { Int($0[0]) | (Int($0[1]) << 7) } ?? -1
    restoreAfterTestSetup(link, originals: originals)
  }
  lowHeap = min(lowHeap, after.heapLowest); deepStack = max(deepStack, after.stackDeepest)
  print("")
  print("Sent: " + sent.sorted { $0.key < $1.key }.map { "\($0.value) \($0.key)" }.joined(separator: ", "))
  print("Back from the unit: \(received["noteOn", default: 0]) Note On, \(received["noteOff", default: 0]) Note Off, \(received["polyAT", default: 0]) poly AT, \(received["chanAT", default: 0]) channel AT, \(received["cc", default: 0]) CC, \(received["bend", default: 0]) bend, \(received["other", default: 0]) other, \(acks) acknowledgements, \(statusReplies) status replies")
  let stuck = outstanding.filter { $0.value > 0 }
  if received["noteOn", default: 0] == 0 { print("Notes: none came back on the USB port\(usbRoute ? "" : ", and no route sends to USB"), so the stuck note check could not run. Memory and loop numbers below are still real.") }
  else if stuck.isEmpty { print("Notes: every Note On the unit sent came to an end. No stuck notes.") }
  else { print("STUCK NOTES: \(stuck.count): " + stuck.sorted { $0.key < $1.key }.prefix(12).map { "ch \($0.key / 128 + 1) note \($0.key % 128)" }.joined(separator: ", ")) }
  let extraOff = outstanding.filter { $0.value < 0 }.count
  if extraOff > 0 { print("(\(extraOff) notes were ended more than once, harmless)") }
  print("Longest pass of the main loop: \(maxLoopUs) microseconds. Input held back \(heldBack) times. Fullest queues: link \(maxLinkPct)%, TRS \(maxTrsPct)%, USB \(maxUsbPct)%")
  if after.faultPc != 0 { print("The unit's last restart was a HARD FAULT at address 0x\(String(after.faultPc, radix: 16)), in stage \(after.crash)") }
  else if after.crash != 0 { print("The unit's last watchdog restart was in stage \(after.crash)") }
  let stackFree = stackSize - deepStack
  print("Memory after: free heap lowest \(lowHeap / 1024) KB (it was \(before.heapLowest / 1024) KB before), main stack deepest \(stackText(deepStack))")
  // A restart gives the unit's stack counter a fresh start, so a counter that went down is the sign of one. Test routes that are gone are
  // not, when the unit has more than one preset: every preset switch loads that slot over the RAM-only test routes.
  let restarted = after.stackDeepest < before.stackDeepest
  let routesGone = setupLoaded && routesLeft != subset.count
  if restarted { print("THE UNIT RESTARTED DURING THE SESSION: its stack counter went from \(before.stackDeepest) to \(after.stackDeepest)") }
  else if routesGone && slots > 1 { print("The test routes are gone, as expected: the unit has \(slots) presets and the session switched between them, each switch loads that slot's routes.") }
  else if routesGone { print("THE TEST ROUTES ARE GONE with no preset switch to explain it: it had \(subset.count) and has \(routesLeft) now") }
  let healthy = stuck.isEmpty && !restarted && (!setupLoaded || routesLeft == subset.count || slots > 1) && deepStack <= 6144 && lowHeap > 20 * 1024
  print(healthy ? "RESULT: healthy" : "RESULT: look at the lines above")
  exit(healthy ? 0 : 1)
}

// ---- main ------------------------------------------------------------------------------------------------------------

var args = Array(CommandLine.arguments.dropFirst())
func option(_ name: String) -> String? {
  if let i = args.firstIndex(of: name), i + 1 < args.count { let v = args[i + 1]; args.removeSubrange(i...(i + 1)); return v }
  return nil
}
let portName = option("--port") ?? "FoxWizard"
let expectArg = option("--expect").flatMap(Double.init)
let command = args.first ?? "help"

switch command {
case "list":
  print("MIDI sources:"); for i in 0..<MIDIGetNumberOfSources() { print("  ", endpointName(MIDIGetSource(i))) }
  print("MIDI destinations:"); for i in 0..<MIDIGetNumberOfDestinations() { print("  ", endpointName(MIDIGetDestination(i))) }
case "version":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else {
    print("No MIDI port with \"\(portName)\" in its name. Try: clock_check list"); exit(2)
  }
  if let id = link.identity() { print("FoxWizard main brain: firmware \(id.major).\(id.minor), protocol \(id.protocolVersion)") } else { print("No answer") ; exit(1) }
  if let st = link.status() { print("Sidecar: \(st.host ? "present, firmware \(st.fw)" : "not present")") }
  if let m = link.memory() { print("Memory: the free heap has been as low as \(m.heapLowest / 1024) KB since power on (of about 44 KB at start), the main stack has gone \(stackText(m.stackDeepest))"); if m.crash != 0 { print("The last watchdog restart was in stage \(m.crash)") } }
case "livetest":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  var originals: [UInt8: [UInt8]] = [:]
  func alive() -> Bool { (ask(link, 0x20, reply: 0x21).map { Int($0[0]) | (Int($0[1]) << 7) } ?? -1) == testRoutes.count }
  func fresh() { if !alive() { _ = loadTestSetup(link, originals: &originals) } }
  fresh()
  let steps: [(String, [[UInt8]])] = [
    ("Live Ch Up with no key held", [[0x90, 120, 100], [0x80, 120, 0]]),
    ("Live Ch Up with one key held on channel 1", [[0x90, 60, 100], [0x90, 120, 100], [0x80, 120, 0], [0x80, 60, 0]]),
    ("Live Ch Down with one key held", [[0x90, 62, 100], [0x90, 121, 100], [0x80, 121, 0], [0x80, 62, 0]]),
    ("Direct channel (122) with a chord held", [[0x90, 50, 100], [0x90, 54, 100], [0x90, 57, 100], [0x90, 122, 100], [0x80, 122, 0], [0x80, 50, 0], [0x80, 54, 0], [0x80, 57, 0]]),
    ("Direct channel again (back) with a chord held", [[0x90, 50, 100], [0x90, 54, 100], [0x90, 122, 100], [0x80, 122, 0], [0x90, 122, 100], [0x80, 122, 0], [0x80, 50, 0], [0x80, 54, 0]]),
    ("Up five times with keys held", [[0x90, 60, 100], [0x90, 64, 100]] + (0..<5).flatMap { _ in [[0x90, 120, 100], [0x80, 120, 0]] as [[UInt8]] } + [[0x80, 60, 0], [0x80, 64, 0]])
  ]
  for (label, msgs) in steps {
    fresh()
    for m in msgs { link.send(m); Thread.sleep(forTimeInterval: 0.08) }
    Thread.sleep(forTimeInterval: 0.4)
    print("\(alive() ? "ok      " : "CRASHED ") \(label)")
  }
  _ = ask(link, 0x28, reply: 0x7E)
  restoreAfterTestSetup(link, originals: originals)
case "loopswing":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  let ok = runLoopSwing(link)
  print(ok ? "LOOP SWING AND TRANSPOSE: ALL PASS" : "LOOP SWING AND TRANSPOSE: SOMETHING FAILED")
  exit(ok ? 0 : 1)
case "setbpm":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination), let g = ask(link, 0x10, reply: 0x11), g.count >= 15, let bpm = args.dropFirst().first.flatMap({ Int($0) }), bpm >= 20, bpm <= 300 else { print("clock_check setbpm 120   (memory only, power cycle undoes it)"); exit(2) }
  var changed = g; changed[7] = UInt8(bpm & 0x7F); changed[8] = UInt8(bpm >> 7)
  _ = ask(link, 0x12, changed, reply: 0x7E)
  print("Tempo set to \(bpm) BPM in memory")
  exit(0)
case "rrvelocity":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  let ok = runRrVelocity(link)
  print(ok ? "ROUND ROBIN VELOCITY: ALL PASS" : "ROUND ROBIN VELOCITY: SOMETHING FAILED")
  exit(ok ? 0 : 1)
case "commit":
  // Writes the unit's current settings to its flash, the same as Patchling's Save to Device. Used to read the stack depth that costs.
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  let reply = ask(link, 0x31, reply: 0x7E, timeout: 5.0)
  print(reply == nil ? "No answer to the save" : "Saved to the unit's flash")
  exit(reply == nil ? 1 : 0)
case "getblock":
  // Reads one feature block the way Patchling does, for finding which request raises the stack depth.
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination), let type = args.dropFirst().first.flatMap({ UInt8($0) }) else { print("clock_check getblock TYPE"); exit(2) }
  let r = ask(link, 0x17, [type], reply: 0x18, timeout: 2.0)
  print(r == nil ? "block \(type): no answer" : "block \(type): \(unpack7(Array(r!.dropFirst())).count) bytes")
  exit(0)
case "fourarps":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  let ok = runFourArps(link)
  print(ok ? "FOUR ARPS AT ONCE: ALL PASS" : "FOUR ARPS AT ONCE: SOMETHING FAILED")
  exit(ok ? 0 : 1)
case "dive":
  // Needs a firmware built with -DFW_DIAG. Makes the unit really use the stack, step by step, and reads the depth it reached. Stops at the first step
  // that gets no answer or after which the unit's stack counter went down (a restart).
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination), let start = link.memory() else { print("no port"); exit(2) }
  let steps = (args.dropFirst().first ?? "10,20,30,40,50,60,70").split(separator: ",").compactMap { UInt8($0) }
  print("Stack counter at the start: \(start.stackDeepest) bytes. Each step is about 70 bytes a frame on top of the unit's own use")
  var last = start.stackDeepest
  for frames in steps {
    guard let ack = ask(link, 0x7C, [frames], reply: 0x7E, timeout: 2.0) else { print("frames \(frames): NO ANSWER, the unit may have restarted or hung"); exit(1) }
    if ack.count > 1, ack[1] != 0 { print("frames \(frames): the unit answered with error \(ack[1]), this is not a -DFW_DIAG build"); exit(1) }
    Thread.sleep(forTimeInterval: 0.2)
    guard let now = link.memory(timeout: 2.0) else { print("frames \(frames): the unit did not answer the memory read afterwards"); exit(1) }
    let restarted = now.stackDeepest < last
    print("frames \(frames): stack deepest \(now.stackDeepest) bytes\(restarted ? "  RESTARTED, the counter went down" : "") (\(now.stackDeepest > 2048 ? "past the 2,048 the linker reserves" : "inside the reserved 2,048")), heap lowest \(now.heapLowest / 1024) KB")
    if restarted { exit(1) }
    last = now.stackDeepest
  }
  print("The unit survived every step")
  exit(0)
case "soak":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  let phase = (args.dropFirst().first ?? "a").lowercased()
  let minutes = args.dropFirst(2).first.flatMap(Double.init) ?? 30
  guard phase == "a" || phase == "b" else { print("clock_check soak a|b MINUTES"); exit(2) }
  exit(runSoak(link, phase: phase, minutes: minutes) ? 0 : 1)
case "update":
  // Flashes a firmware .bin over MIDI the way Patchling does: FW_BEGIN with the size and MD5, 128 byte chunks each confirmed by its checksum,
  // FW_END, which checks the MD5 of the whole file and restarts the unit into it. Nothing is installed if any step fails.
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination), let path = args.dropFirst().first, let file = FileManager.default.contents(atPath: path) else { print("clock_check update FILE.bin"); exit(2) }
  let md5Task = Process(); md5Task.executableURL = URL(fileURLWithPath: "/sbin/md5"); md5Task.arguments = ["-q", path]
  let md5Pipe = Pipe(); md5Task.standardOutput = md5Pipe
  try? md5Task.run(); md5Task.waitUntilExit()
  let md5 = (String(data: md5Pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
  guard md5.count == 32 else { print("could not take the MD5 of the file"); exit(2) }
  let bytes = [UInt8](file)
  func sum7(_ b: [UInt8]) -> UInt8 { UInt8(b.reduce(0) { $0 + Int($1) } % 128) }
  let begin: [UInt8] = [UInt8(bytes.count & 0x7F), UInt8((bytes.count >> 7) & 0x7F), UInt8((bytes.count >> 14) & 0x7F)] + Array(md5.utf8)
  guard let b = ask(link, 0x40, begin, reply: 0x7E, timeout: 10.0), b.count > 2, b[1] == 0, b[2] == sum7(begin) else { print("the unit did not accept the start of the update"); exit(1) }
  let chunks = (bytes.count + 127) / 128
  for i in 0..<chunks {
    let raw = Array(bytes[(i * 128)..<min(bytes.count, (i + 1) * 128)])
    var ok = false
    for _ in 0..<4 {
      if let d = ask(link, 0x41, [UInt8(i & 0x7F), UInt8((i >> 7) & 0x7F)] + pack7(raw), reply: 0x7E, timeout: 3.0), d.count > 2, d[1] == 0, d[2] == sum7(raw) { ok = true; break }
    }
    guard ok else { print("chunk \(i) of \(chunks) was not confirmed, the update was not installed"); exit(1) }
    if i % max(1, chunks / 10) == 0 { print("  \(i * 100 / chunks)%") }
  }
  guard let e = ask(link, 0x42, [], reply: 0x7E, timeout: 10.0), e.count > 1, e[1] == 0 else { print("the unit did not confirm the end of the update"); exit(1) }
  print("Update sent and confirmed, the unit restarts into it now")
  exit(0)
case "listen":
  // Counts what the unit is sending right now, by kind, and shows the first few messages.
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  let seconds = args.dropFirst().first.flatMap(Double.init) ?? 5
  _ = link.drain()
  var kinds: [String: Int] = [:], first: [String] = []
  let until = Date().addingTimeInterval(seconds)
  while Date() < until {
    for r in link.drain() {
      let b = r.bytes
      let name: String
      if b.first == 0xF0 { name = "sysex" } else if let f = b.first { name = f >= 0xF8 ? String(format: "realtime %02X", f) : String(format: "%@ ch%d", ["", "", "", "", "", "", "", "", "note off", "note on", "poly AT", "CC", "program", "chan AT", "bend", ""][Int(f >> 4)], Int(f & 0x0F) + 1) } else { name = "empty" }
      kinds[name, default: 0] += 1
      if first.count < 12 { first.append(b.map { String(format: "%02X", $0) }.joined(separator: " ")) }
    }
    Thread.sleep(forTimeInterval: 0.01)
  }
  if kinds.isEmpty { print("The unit sent nothing in \(Int(seconds)) s") } else {
    print("In \(Int(seconds)) s the unit sent: " + kinds.sorted { $0.value > $1.value }.prefix(12).map { "\($0.value) \($0.key)" }.joined(separator: ", "))
    print("First messages: " + first.joined(separator: " | "))
  }
  exit(0)
case "ident":
  // Asks the unit who it is again and again with a long wait each time, and shows every byte it sends back and how long it took.
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  let total = args.dropFirst().first.flatMap(Double.init) ?? 25
  let stop = Date().addingTimeInterval(total)
  var tries = 0
  while Date() < stop {
    tries += 1
    _ = link.drain()
    let sent = Date()
    link.send([0xF0, 0x7D, 0x4D, 0x01, 0xF7])
    var got: [String] = []
    while Date().timeIntervalSince(sent) < 5.0 && got.isEmpty {
      for r in link.drain() { got.append(r.bytes.prefix(12).map { String(format: "%02X", $0) }.joined(separator: " ") + (r.bytes.count > 12 ? " ..." : "")) }
      Thread.sleep(forTimeInterval: 0.01)
    }
    print(String(format: "try %d: ", tries) + (got.isEmpty ? "nothing in 5 s" : String(format: "%.2f s: ", Date().timeIntervalSince(sent)) + got.prefix(3).joined(separator: " | ")))
  }
  exit(0)
case "swingtest":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  var originals: [UInt8: [UInt8]] = [:]
  guard loadTestSetup(link, originals: &originals, subset: [1]) else { print("could not load the test setup"); exit(1) }
  print("Arp on channel 10, one held note, 1/16 steps at the unit's tempo. Measures the share of each pair the first step takes")
  let scenarios: [(String, UInt8, UInt8)] = [("straight", 0, 0), ("54% on a 16th", 54, 0), ("62% on a 16th", 62, 0), ("66% on a 16th", 66, 0), ("75% on a 16th", 75, 0), ("66% on an 8th", 66, 1), ("75% on an 8th", 75, 1)]
  for (label, pct, base) in scenarios {
    var arp = testBlocks[0].bytes
    arp[15] = pct; arp[44] = base
    _ = ask(link, 0x19, [4] + pack7(arp), reply: 0x7E)
    _ = link.drain()
    link.send([0x99, 60, 100])
    var times: [Double] = []
    let until = Date().addingTimeInterval(11.0)
    while Date() < until { for r in link.drain() where r.bytes.count == 3 && r.bytes[0] == 0x99 && r.bytes[2] > 0 { times.append(r.time) }; Thread.sleep(forTimeInterval: 0.002) }
    link.send([0x89, 60, 0])
    Thread.sleep(forTimeInterval: 0.5)
    times = Array(times.dropFirst(2)) // let it settle
    guard times.count > 24 else { print("\(label): only \(times.count) steps heard"); continue }
    let gaps = (0..<(times.count - 1)).map { times[$0 + 1] - times[$0] }
    let per = base == 1 ? 4 : 2 // steps in a pair
    var best = 0.0, bestShare = 0.5, pair = 0.0
    for offset in 0..<per {
      var first = 0.0, whole = 0.0, n = 0
      var i = offset
      while i + per <= gaps.count { first += gaps[i..<(i + per / 2)].reduce(0, +); whole += gaps[i..<(i + per)].reduce(0, +); i += per; n += 1 }
      if n > 3, first / whole > best { best = first / whole; bestShare = first / whole; pair = whole / Double(n) }
    }
    print(String(format: "%@: the first half of a pair takes %.1f%%, a pair lasts %.1f ms, %d steps", label, bestShare * 100, pair * 1000, times.count))
  }
  restoreAfterTestSetup(link, originals: originals)
case "dinflood":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  var originals: [UInt8: [UInt8]] = [:]
  for b in testBlocks { if let r = ask(link, 0x17, [b.type], reply: 0x18) { originals[b.type] = unpack7(Array(r.dropFirst())) } }
  var route = testRoutes[3]
  route[43] = args.contains("--both") ? 3 : 1; route[44] = 0 // out ports: TRS/DIN only (3 with --both: and USB, to see every message arrive), which carries about 3 KB a second
  route[9] = 0b1; route[10] = 0 // out channel 1 only, so each update is one message
  _ = ask(link, 0x28, reply: 0x7E)
  _ = ask(link, 0x24, pack7(route), reply: 0x7E)
  print("One route, channel 3 in to DIN out. Poly aftertouch for a held note, then a Note On and Off to see how long a note waits behind it")
  for rate in (option("--rates") ?? "100,400,800,1600").split(separator: ",").compactMap({ Double($0) }) {
    _ = link.drain()
    link.send([0x92, 60, 100]); Thread.sleep(forTimeInterval: 0.2)
    let n = Int(rate * 6)
    let t0 = mach_absolute_time()
    for i in 0..<n {
      mach_wait_until(t0 + secToTicks(Double(i) / rate))
      link.send([0xA2, 60, UInt8(20 + i % 100)])
    }
    link.send([0x82, 60, 0])
    // asked for the moment the flood ends: anything else that asks the unit for its status (Patchling every 2 s) resets these counts
    var onUsb = 0
    let st = ask(link, 0x13, reply: 0x14, timeout: 4.0)
    let untilBack = Date().addingTimeInterval(2.0)
    while Date() < untilBack { for r in link.drain() where r.bytes.count == 3 && r.bytes[0] & 0xF0 == 0xA0 { onUsb += 1 }; Thread.sleep(forTimeInterval: 0.01) }
    let held = st.map { Int($0[18]) } ?? -1, trs = st.map { Int($0[20]) * 100 / 127 } ?? -1
    let kept = st.map { $0.count >= 41 ? Int($0[37]) | (Int($0[38]) << 7) : -1 } ?? -1, thinned = st.map { $0.count >= 41 ? Int($0[39]) | (Int($0[40]) << 7) : -1 } ?? -1
    print("\(Int(rate)) aftertouch per second for 6 s: the DIN queue peaked at \(trs)%, input held back \(held) times, \(kept) updates went out to DIN and \(thinned) were thinned away, \(onUsb) aftertouch messages came back on USB (with a cable from DIN out to DIN in, that is what the DIN port sent)")
  }
  _ = ask(link, 0x28, reply: 0x7E)
  restoreAfterTestSetup(link, originals: originals)
case "atflood":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  var originals: [UInt8: [UInt8]] = [:]
  func alive() -> Bool { (ask(link, 0x20, reply: 0x21).map { Int($0[0]) | (Int($0[1]) << 7) } ?? -1) == 1 }
  _ = loadTestSetup(link, originals: &originals, subset: [3])
  for rate in [100.0, 200.0, 400.0, 800.0, 1600.0] {
    if !alive() { _ = loadTestSetup(link, originals: &originals, subset: [3]) }
    _ = link.drain()
    link.send([0x92, 60, 100]); Thread.sleep(forTimeInterval: 0.2)
    let n = Int(rate * 8)
    let t0 = mach_absolute_time()
    for i in 0..<n {
      mach_wait_until(t0 + secToTicks(Double(i) / rate))
      link.send([0xA2, 60, UInt8(20 + i % 100)])
    }
    link.send([0x82, 60, 0])
    var back = 0
    let until = Date().addingTimeInterval(1.5)
    while Date() < until { for r in link.drain() where r.bytes.count == 3 && r.bytes[0] & 0xF0 == 0xA0 { back += 1 }; Thread.sleep(forTimeInterval: 0.01) }
    let ok = alive()
    print("\(Int(rate)) poly AT per second for 8 s: sent \(n), came back \(back), \(ok ? "the unit survived" : "THE UNIT RESTARTED")")
  }
  if !alive() { _ = loadTestSetup(link, originals: &originals, subset: [3]) }
  restoreAfterTestSetup(link, originals: originals)
case "flood":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  var originals: [UInt8: [UInt8]] = [:]
  _ = loadTestSetup(link, originals: &originals)
  for rate in [100.0, 300.0, 600.0] {
    _ = link.drain()
    let n = Int(rate * 5)
    let t0 = mach_absolute_time()
    for i in 0..<n {
      mach_wait_until(t0 + secToTicks(Double(i) / rate))
      link.send([0xB1, 1, UInt8(i % 128)])
    }
    var back = 0
    let until = Date().addingTimeInterval(1.0)
    while Date() < until { for r in link.drain() where r.bytes.count == 3 && r.bytes[0] == 0xB1 && r.bytes[1] == 1 { back += 1 }; Thread.sleep(forTimeInterval: 0.01) }
    let st = ask(link, 0x13, reply: 0x14)
    let held = st.map { Int($0[18]) } ?? -1
    print("\(Int(rate)) CC per second for 5 s: sent \(n), came back \(back), the unit reports input held back \(held) times")
  }
  restoreAfterTestSetup(link, originals: originals)
case "probe":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  var originals: [UInt8: [UInt8]] = [:]
  let loaded = loadTestSetup(link, originals: &originals)
  let count = ask(link, 0x20, reply: 0x21).map { Int($0[0]) | (Int($0[1]) << 7) } ?? -1
  print("setup loaded \(loaded), routes now \(count)")
  func show(_ label: String, _ seconds: Double) {
    let until = Date().addingTimeInterval(seconds)
    var got: [String] = []
    while Date() < until { for r in link.drain() where r.bytes.first != 0xF0 && r.bytes != [0xF8] { got.append(r.bytes.map { String($0, radix: 16) }.joined(separator: " ")) }; Thread.sleep(forTimeInterval: 0.01) }
    print("\(label): \(got.isEmpty ? "nothing back" : got.joined(separator: " | "))")
  }
  for (label, bytes) in [("ch2 note 60 on (scale chord)", [0x91, 60, 100]), ("ch2 note 60 off", [0x81, 60, 0]), ("ch3 note 62 on (round robin)", [0x92, 62, 100]), ("ch3 note 62 off", [0x82, 62, 0]), ("ch1 note 64 on (priority)", [0x90, 64, 100]), ("ch1 note 64 off", [0x80, 64, 0]), ("ch10 note 40 on (arp)", [0x99, 40, 100])] as [(String, [UInt8])] {
    link.send(bytes); show(label, 0.5)
  }
  link.send([0x89, 40, 0]); show("ch10 note 40 off", 0.8)
  restoreAfterTestSetup(link, originals: originals)
case "session":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else {
    print("No MIDI port with \"\(portName)\" in its name. Try: clock_check list"); exit(2)
  }
  let secs = Double(args.dropFirst().first ?? "") ?? 90
  let chans = (option("--channels") ?? "1,10").split(separator: ",").compactMap { UInt8($0) }.filter { $0 >= 1 && $0 <= 16 }.map { $0 - 1 }
  let noSetup = args.contains("--no-setup")
  runSession(link, seconds: secs, channels: chans.isEmpty ? [0, 9] : chans, useSetup: !noSetup)
case "selftest":
  var selftestFailed = false
  defer { }
  for (label, error) in [("a clock that is accurate", 0.0), ("a clock that is off by 0.3 percent", -3000.0)] {
    print("== simulated FoxWizard with \(label)")
    results = []
    let fake = FakeFox(); fake.errorPpm = error
    Thread.sleep(forTimeInterval: 0.3)
    guard let link = Link(source: fake.source, destination: fake.destination) else { print("could not connect"); exit(2) }
    runAll(link, expectArg: nil)
    let code = summary()
    let expectedPass = error == 0
    let tempoCaught = results.contains { !$0.1 && $0.0.contains("tempo") && $0.0.contains("leading") }
    let ok = expectedPass ? code == 0 : (code != 0 && tempoCaught)
    print(ok ? "-> as it should be: \(expectedPass ? "all pass" : "the slow clock is caught")" : "-> NOT as it should be")
    if !ok { selftestFailed = true }
    fake.running = false
    Thread.sleep(forTimeInterval: 0.2)
  }
  print(selftestFailed ? "SELFTEST FAILED" : "SELFTEST OK: the checks pass a good clock and catch a bad one")
  exit(selftestFailed ? 1 : 0)
case "out", "in", "all":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else {
    print("No MIDI port with \"\(portName)\" in its name. Try: clock_check list"); exit(2)
  }
  // --with-arp: an arp with 66% swing plays on channel 10 while the clock is measured, loaded into memory only and restored after
  var arpOriginals: [UInt8: [UInt8]] = [:]
  let withArp = args.contains("--with-arp")
  if withArp {
    if loadTestSetup(link, originals: &arpOriginals, subset: [1]) {
      var arp = testBlocks[0].bytes
      let swingPercent = option("--arp-swing").flatMap(UInt8.init) ?? 66
      arp[15] = swingPercent; arp[44] = 0
      _ = ask(link, 0x19, [4] + pack7(arp), reply: 0x7E)
      link.send([0x99, 60, 100])
      print("An arp with \(swingPercent == 0 ? "no" : "\(swingPercent)%") swing is playing on channel 10 while the clock is checked")
    } else { print("could not load the arp") }
  }
  // --send-clock: Send Clock is turned on in memory only for the check, and put back after
  var globalOriginal: [UInt8]? = nil
  if args.contains("--send-clock"), let g = ask(link, 0x10, reply: 0x11), g.count >= 15, g[11] == 0 {
    globalOriginal = g
    var on = g; on[11] = 1
    _ = ask(link, 0x12, on, reply: 0x7E)
    print("Send Clock turned on for the check, memory only")
  }
  if command == "all" { runAll(link, expectArg: expectArg) }
  else if command == "out" {
    let secs = Double(args.dropFirst().first ?? "") ?? 20
    let st = link.status()
    checkOut(link, label: "clock out", expect: expectArg ?? Double(st?.bpm ?? 120), seconds: secs)
  } else {
    let bpm = Double(args.dropFirst().first ?? "") ?? 120
    let secs = args.count > 2 ? (Double(args[2]) ?? 10) : 10
    checkIn(link, bpm: bpm, seconds: secs, start: true, label: "clock in")
  }
  if withArp { link.send([0x89, 60, 0]); restoreAfterTestSetup(link, originals: arpOriginals) }
  if let g = globalOriginal { _ = ask(link, 0x12, g, reply: 0x7E) }
  exit(summary())
default:
  print("clock_check list | version | session [seconds] | all | out [seconds] | in [bpm] [seconds] | selftest   (--port NAME, --expect BPM, --with-arp, --arp-swing PERCENT, --send-clock)")
}
