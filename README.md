# HMI

TRDP-based console HMI application for door control, built on top of the imported TRDP stack in `import/3.0.0.0`.

## What the application does

The HMI provides a text console where an operator can:

- monitor door state for a fixed set of doors (`HMI_DOOR_COUNT`, default: 4)
- send commands to an individual door (`open`, `close`, `stop`)
- send commands to all doors (`open-all`, `close-all`, `stop-all`)

Door status is tracked locally in the HMI and can also be updated from incoming TRDP PD status messages.

## Build flow

1. Inspect TRDP stack build help:

```bash
make trdp-help
```

2. Build TRDP static libraries (`libtrdp.a` and `libtrdpap.a`):

```bash
make trdp-lib
```

3. Build the HMI application:

```bash
make app
```

The executable is generated as `./HMI`.

## Runtime

```bash
./HMI [runtime_seconds] [own_ip] [gateway_ip] [mc_a] [mc_b]
```

If only `runtime_seconds` is provided, defaults are used for IPs. If omitted, process runs continuously.

Default IP layout:

- HMI host IP: `192.168.56.2`
- Gateway host IP: `192.168.56.1`
- Multicast groups listened by HMI: `239.192.0.1`, `239.192.0.2`

## Console commands

- `status` — print current door states
- `open <id>` — open one door (`id` in `1..4`)
- `close <id>` — close one door
- `stop <id>` — stop one door
- `open-all` — open all doors
- `close-all` — close all doors
- `stop-all` — stop all doors
- `help` — show help
- `quit` — exit

## TRDP channels used by HMI

- PD subscribe (`comId=2001`) from gateway over unicast and multicast
- PD publish (`comId=2002`) from HMI to gateway, carrying current door status snapshot
- MD notify send (`comId=2101`) from HMI to gateway, carrying door commands
- MD listener (`comId=2102`) from gateway to HMI

HMI XML stack configuration is provided in `config/trdp_hmi.xml`.
