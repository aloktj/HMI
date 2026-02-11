# Door HMI Web Application

## Overview

Web-based Human Machine Interface (HMI) for controlling train door systems via TRDP protocol through a CAN-TRDP gateway. The HMI runs on Linux as a Crow-framework web server that presents a real-time control panel in the browser.

## Architecture

```
┌────────────────────────────────────────────────────────────┐
│  Browser (http://hmi-ip:8080)                              │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Door HMI Control Panel                              │  │
│  │  ┌─────────────┐  ┌──────────────────────────────┐   │  │
│  │  │ Train Speed  │  │  EMERGENCY OPEN ALL DOORS    │   │  │
│  │  │ [___] km/h   │  │       ⚡ ACTIVATE            │   │  │
│  │  └─────────────┘  └──────────────────────────────┘   │  │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐   │  │
│  │  │ Door 1  │ │ Door 2  │ │ Door 3  │ │ Door 4  │   │  │
│  │  │ CLOSED  │ │  OPEN   │ │OBSTRUCT │ │ CLOSED  │   │  │
│  │  │[Open]   │ │ [Close] │ │[Close]  │ │ [Open]  │   │  │
│  │  └─────────┘ └─────────┘ └─greyed──┘ └─────────┘   │  │
│  └───────────── polls /api/status every 500ms ─────────┘  │
└─────────────────────┬──────────────────────────────────────┘
                      │  HTTP REST API
┌─────────────────────┴──────────────────────────────────────┐
│  HMI Application (hmi_webapp)                              │
│                                                            │
│  ┌─────────────────────┐   ┌───────────────────────────┐   │
│  │  Crow Web Server    │   │  TRDP Thread              │   │
│  │  (Thread 1)         │   │  (Thread 2)               │   │
│  │                     │   │                            │   │
│  │  GET  /api/status   │◄──┤  PD RX: ComId 2001        │   │
│  │  POST /api/speed    │──►│  Aggregated Door Status    │   │
│  │  POST /api/emergency│──►│  (64 bytes, 8×8)           │   │
│  │  POST /api/door/N/* │──►│                            │   │
│  │                     │   │  PD TX: ComId 2010         │   │
│  │  Shared State       │◄─►│  Aggregated Door Command   │   │
│  │  (mutex protected)  │   │  (64 bytes, 8×8)           │   │
│  │                     │   │                            │   │
│  │                     │   │  PD TX: ComId 2002         │   │
│  │                     │   │  HMI Heartbeat (8 bytes)   │   │
│  └─────────────────────┘   └──────────┬────────────────┘   │
└────────────────────────────────────────┤────────────────────┘
                                         │  TRDP/UDP
┌────────────────────────────────────────┤────────────────────┐
│  Gateway                               │                    │
│  TRDP ↔ CAN converter                  │                    │
│                                         │                    │
│  TRDP PD 2010 (agg cmd)  ──► CAN 0x401..0x408 (per door)  │
│  CAN  0x301..0x308 (status) ──► TRDP PD 2001 (agg status) │
└────────────────────────────────────────┤────────────────────┘
                                         │  CAN bus (500 kbps)
┌────────┐ ┌────────┐         ┌────────┐ │
│ Door 1 │ │ Door 2 │  ...    │ Door 8 │ │
│  DCU   │ │  DCU   │         │  DCU   │◄┘
└────────┘ └────────┘         └────────┘
```

## Data Flow Analysis

### CAN ICD ↔ TRDP Mapping

The gateway translates between per-door CAN frames and aggregated TRDP process data:

| Direction | CAN | TRDP | Size |
|-----------|-----|------|------|
| Door→GW→HMI | 0x301..0x308 (8 frames × 8B) | ComId 2001 (1 PD × 64B) | 8 doors × 8 bytes |
| HMI→GW→Door | ComId 2010 (1 PD × 64B) | 0x401..0x408 (8 frames × 8B) | 8 doors × 8 bytes |

### Byte Layout (per door, both directions)

**Door Status** (CAN ICD §4):
```
Byte  Signal           Values
B0    door_state       0=OPEN, 1=CLOSED
B1    obstruction      0=NO, 1=YES
B2    last_cmd         0=NONE, 1=OPEN, 2=CLOSE
B3    close_blocked    0=NO, 1=CLOSE blocked (obstruction)
B4    status_counter   Rolling counter
B5-7  Reserved         0x00
```

**Door Command** (CAN ICD §5):
```
Byte  Signal           Values
B0    cmd              0=NONE, 1=OPEN, 2=CLOSE
B1    alive_counter    Increments on command change only
B2-7  Reserved         0x00
```

## Changes from Original Code

| Aspect | Original (`hmi_main.c`) | New (`hmi_main.cpp`) |
|--------|------------------------|----------------------|
| Interface | CLI (stdin) | Crow HTTP web server |
| Door commands | Per-door PD publishers (ComId 2101-2108) | Single aggregated PD (ComId 2010) |
| Door status | Generic 64B PD subscription | Typed `AggregatedDoorStatus_T` |
| Language | C11 | C++17 (Crow requires C++) |
| Threading | Single-threaded + select() | 2 threads (Crow + TRDP) |
| Business logic | None (manual cmd entry) | Speed/emergency/obstruction rules |
| alive_counter | Manual increment | Auto-increment on command change |

## Business Rules

1. **Speed = 0 km/h**: Doors can be individually commanded OPEN (cmd=1) or CLOSE (cmd=2)
2. **Speed > 0 km/h**: All doors automatically commanded CLOSE; OPEN buttons disabled
3. **Emergency**: All doors commanded OPEN regardless of speed; pulsing red indicator
4. **Obstruction**: CLOSE button greyed out with "⚠ Obstructed" label (per CAN ICD §6c)
5. **alive_counter**: Increments only when HMI intent changes (per CAN ICD §7a.iii)

## REST API

| Endpoint | Method | Body | Description |
|----------|--------|------|-------------|
| `/` | GET | — | Serve control panel HTML |
| `/api/status` | GET | — | JSON: all door states, speed, emergency flag |
| `/api/speed` | POST | `{"speed": N}` | Set train speed (km/h) |
| `/api/emergency` | POST | `{"active": bool}` | Activate/deactivate emergency |
| `/api/door/<id>/open` | POST | `{}` | Command door to OPEN (if allowed) |
| `/api/door/<id>/close` | POST | `{}` | Command door to CLOSE (if allowed) |

## Build & Run

### Prerequisites
- Linux (Ubuntu 22.04+ recommended)
- GCC/G++ with C++17 support
- Boost ASIO (`libboost-all-dev`)
- TRDP library source in `import/3.0.0.0/`

### Build
```bash
sudo apt install build-essential libboost-all-dev libuuid-dev
make app          # builds TRDP libs + HMI application
```

### Run
```bash
./hmi_webapp [own_ip] [gw_ip] [mc_a] [mc_b] [web_port] [web_dir]

# Defaults:
./hmi_webapp 192.168.56.2 192.168.56.1 239.192.0.1 239.192.0.2 8080 web
```

Then open `http://<own_ip>:8080` in a browser.

## File Structure

```
├── include/
│   ├── hmi_trdp.h        # TRDP constants, payload structs (CAN-aligned)
│   └── crow_all.h        # Crow framework single header (auto-downloaded)
├── src/
│   └── hmi_main.cpp      # Main application (Crow + TRDP threads)
├── web/
│   └── index.html         # Control panel frontend
├── trdp_hmi.xml           # TRDP config DB (aggregated telegrams)
├── Makefile
└── README.md
```
