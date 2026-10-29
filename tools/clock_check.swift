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

  // The messages that came in since the last call. SysEx is put back together across packets.
  func drain() -> [Received] {
    var out: [Received] = []
    for r in inbox.take() {
      if r.bytes.count == 1 && r.bytes[0] >= 0xF8 { out.append(r); continue }
      var b = r.bytes
      if !sysexTail.isEmpty || b.first == 0xF0 { sysexTail += b; if sysexTail.last == 0xF7 { out.append(Received(time: r.time, bytes: sysexTail)); sysexTail = [] } ; continue }
      if !b.isEmpty { out.append(Received(time: r.time, bytes: b)); b = [] }
    }
    return out
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
  print("clock_check list | version | all | out [seconds] | in [bpm] [seconds] | selftest   (--port NAME, --expect BPM)")
}
