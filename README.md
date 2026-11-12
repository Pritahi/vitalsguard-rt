# VitalsGuard-RT

**A deterministic real-time patient-vitals alarm engine for QNX 8.0 — with the numbers to prove it.**

VitalsGuard-RT implements the safety-critical core of a patient monitor:
continuous ECG / SpO2 / respiration analysis with IEC 60601-1-8 style alarm
priorities, a self-measured real-time latency budget, and a fail-safe
watchdog — built entirely on QNX 8.0 native mechanisms, and demonstrated
live under 100% CPU load.

## Demo & codelab

- Demo video (2 min): https://youtu.be/2y7aooyxrtA
- Step-by-step codelab (free QNX 8.0 + QEMU): https://pritahi.github.io/vitalsguard-rt/

## Why this exists

Alarm systems in medical devices are not allowed to be "usually fast".
A bradycardia alarm that arrives 200 ms late because the UI thread was
busy is a patient-safety bug. General-purpose operating systems cannot
guarantee this; QNX can. VitalsGuard-RT shows *how*, with source you can
audit and measurements you can reproduce.

## Architecture

| Component | QNX mechanism | Role |
|---|---|---|
| `analyzer` | `SCHED_FIFO` prio 60, `CLOCK_MONOTONIC` periodic timer pulse, `MsgReceivePulse` | 100 Hz acquisition + QRS/SpO2/apnea analysis; measures its own wake-up latency every cycle |
| `alarmd` | `name_attach` + native pulses (`MsgSendPulse`), `TimerTimeout` | Alarm state machine: severity, latch, escalate, acknowledge; watchdog over the analyzer |
| `vitalsd` | `resmgr_attach` (`/dev/vitals/*`) | Live state as files: `cat /dev/vitals/alarm` |
| `httpd` | sockets, prio 10 | Live dashboard (strip chart) — deliberately low priority |
| `loadhog` | prio 1 busy loop | Best-effort load injector for the determinism demo |

Everything is a message: the analyzer never logs, never allocates, and
never blocks on anything but its timer pulse. The alarm manager fails
safe: if the analyzer misses its heartbeat by 1.5 s, a HIGH watchdog
alarm trips.

## Build

Prerequisites: QNX SDP 8.0 (free for non-commercial use via
[QNX Everywhere](https://qnx.software/en/developers/get-started/qnx-everywhere)).

```sh
source ~/qnx800/qnxsdp-env.sh        # path to your SDP 8.0 install
make                                  # qcc -Vgcc_ntox86_64
```

Produces `vitalsguard`, an x86_64 QNX ELF.

## Run (QEMU x86_64 target)

Tested on the QNX 8.0 Quick Start Target Image for QEMU
(`com.qnx.qnx800.quickstart.qemu`), QEMU, 1 vCPU / 1 GB:

```sh
# boot the QSTI, then from the VM console:
vitalsguard -p 8080 &               # start engine + dashboard
cat /dev/vitals/alarm               # live status line
echo 3 > /dev/vitals/scenario       # trigger asystole -> HIGH alarm
echo ack > /dev/vitals/alarm        # acknowledge
```

Dashboard: forward guest port 8080 (QEMU user networking hostfwd) and
open `http://localhost:8080/` on the host.

## The determinism demo

```sh
vitalsguard &                       # watch worst-case wake latency
echo 1 > /proc/loadhog (or -l flag) # pin CPU at 100% with prio-1 hog
cat /dev/vitals/alarm               # latency budget still met
```

`latency_worst_us` is the worst observed wake-up jitter of the 100 Hz
analysis loop. Under full CPU load on QNX it stays within budget; that
is the entire point of a microkernel RTOS — and the reason QNX runs in
275M vehicles and countless medical devices.

## Alarm conditions implemented

| Condition | Trigger | Severity |
|---|---|---|
| Asystole | no QRS for 4.2 s | HIGH |
| Bradycardia | HR < 40 | HIGH |
| Tachycardia | HR > 130 | MEDIUM |
| Hypoxia | SpO2 < 90 % | MEDIUM |
| Apnea | no breath for 15 s | MEDIUM |
| Analyzer watchdog | no heartbeat for 1.5 s | HIGH |

## Evidence of QNX use

- Native message passing: `MsgSendPulse` / `MsgReceivePulse` /
  `name_attach` between analyzer and alarm manager (`src/analyzer.c`,
  `src/alarmd.c`)
- Resource manager: `/dev/vitals/*` via `resmgr_attach`
  (`src/vitalsd.c`)
- Real-time scheduling: `SCHED_FIFO` priority 60 with a periodic
  `CLOCK_MONOTONIC` timer pulse (`src/analyzer.c`)
- Fail-safe supervision: `TimerTimeout`-based watchdog
  (`src/alarmd.c`)
- Toolchain: built with `qcc -Vgcc_ntox86_64` from QNX SDP 8.0
- Target: QNX 8.0 Quick Start Target Image, QEMU x86_64

## Repository layout

    src/       C sources (single binary: vitalsguard)
    docs/      codelab + demo script
    tools/     host-side latency plotting for the demo video

## License

MIT (see LICENSE). QNX SDP 8.0 used under the QNX Everywhere
non-commercial license; this repository contains no QNX proprietary code.
