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
        return (Int(r.bytes[4 + 28]) * 512, Int(r.bytes[4 + 29]) * 16, r.bytes.count > 4 + 31 ? Int(r.bytes[4 + 30]) | (Int(r.bytes[4 + 31]) << 7) : 0, r.bytes.count > 4 + 36 && r.bytes[4 + 36] & 0x40 != 0 ? Int(r.bytes[4 + 32]) | (Int(r.bytes[4 + 33]) << 7) | (Int(r.bytes[4 + 34]) << 14) | (Int(r.bytes[4 + 35]) << 21) | (Int(r.bytes[4 + 36] & 0x0F) << 28) : 0)
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

func runSession(_ link: Link, seconds: Double, channels: [UInt8], useSetup: Bool) {
  guard let id = link.identity() else { print("No answer from the FoxWizard"); exit(1) }
  guard let before = link.memory() else { print("This firmware has no memory readout, update to 1.108 or later"); exit(1) }
  print("FoxWizard firmware \(id.major).\(id.minor). Memory before: free heap lowest \(before.heapLowest / 1024) KB, main stack deepest \(before.stackDeepest) of 2048 bytes")
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
  print("Memory after: free heap lowest \(lowHeap / 1024) KB (it was \(before.heapLowest / 1024) KB before), main stack deepest \(deepStack) of 2048 bytes")
  if setupLoaded && routesLeft != subset.count { print("THE UNIT RESTARTED DURING THE SESSION: it had \(subset.count) test routes and has \(routesLeft) now") }
  let healthy = stuck.isEmpty && (!setupLoaded || routesLeft == subset.count) && deepStack < 1600 && lowHeap > 20 * 1024
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
  if let m = link.memory() { print("Memory: the free heap has been as low as \(m.heapLowest / 1024) KB since power on (of about 44 KB at start), the main stack has gone \(m.stackDeepest) bytes deep (of 2048)"); if m.crash != 0 { print("The last watchdog restart was in stage \(m.crash)") } }
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
case "dinflood":
  guard let (source, destination) = findEndpoints(matching: portName), let link = Link(source: source, destination: destination) else { print("no port"); exit(2) }
  var originals: [UInt8: [UInt8]] = [:]
  for b in testBlocks { if let r = ask(link, 0x17, [b.type], reply: 0x18) { originals[b.type] = unpack7(Array(r.dropFirst())) } }
  var route = testRoutes[3]
  route[43] = 1; route[44] = 0 // out ports: TRS/DIN only, which carries about 3 KB a second
  route[9] = 0b1; route[10] = 0 // out channel 1 only, so each update is one message
  _ = ask(link, 0x28, reply: 0x7E)
  _ = ask(link, 0x24, pack7(route), reply: 0x7E)
  print("One route, channel 3 in to DIN out. Poly aftertouch for a held note, then a Note On and Off to see how long a note waits behind it")
  for rate in [100.0, 400.0, 800.0, 1600.0] {
    _ = link.drain()
    link.send([0x92, 60, 100]); Thread.sleep(forTimeInterval: 0.2)
    let n = Int(rate * 6)
    let t0 = mach_absolute_time()
    for i in 0..<n {
      mach_wait_until(t0 + secToTicks(Double(i) / rate))
      link.send([0xA2, 60, UInt8(20 + i % 100)])
    }
    link.send([0x82, 60, 0])
    Thread.sleep(forTimeInterval: 2.0)
    let st = ask(link, 0x13, reply: 0x14, timeout: 4.0)
    let held = st.map { Int($0[18]) } ?? -1, trs = st.map { Int($0[20]) * 100 / 127 } ?? -1
    print("\(Int(rate)) aftertouch per second for 6 s: the DIN queue peaked at \(trs)%, input held back \(held) times")
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
  exit(summary())
default:
  print("clock_check list | version | session [seconds] | all | out [seconds] | in [bpm] [seconds] | selftest   (--port NAME, --expect BPM)")
}
