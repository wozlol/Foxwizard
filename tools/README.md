# Tools

`clock_check.swift` plays into a FoxWizard over USB and measures it: the clock, memory, loop time, stuck notes, restarts. Build it with `swiftc -O clock_check.swift -o clock_check`. The commands, what the numbers should be, and the crash tracker are in `../DIAGNOSTICS.md`.

## Commands

`version`, `ident` (the raw identity reply, again and again), `listen` (what the unit is sending), `list`, `session`, `all` (the clock, with `--with-arp`, `--arp-swing`, `--send-clock`), `out`, `in`, `selftest`, `swingtest` (the arp's swing), `loopswing` (the looper's Record Quantize and swing, `--note-ms`, `--no-quantize`), `fourarps`, `rrvelocity`, `dive` (needs a `-DFW_DIAG` build), `commit` (a save), `getblock`, `setbpm`, `update FILE.bin` (flashes over MIDI like Patchling), and `soak a|b MINUTES`. Soak phase a is the deep path from a RAM-only setup (`--no-flood`, `--no-looper`, `--no-extra-arps`, `--flood-types`, `--flood-triples`, `--flood-channels`, `--out-trs`), phase b is saves and preset switches under flood on the unit's own state, with the TRS loopback in.
