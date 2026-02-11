# HMI

TRDP-based HMI endpoint application built on top of the imported TRDP stack in `import/3.0.0.0`.

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

## TRDP channels used by HMI

- PD subscribe (`comId=2001`) from gateway over unicast and multicast
- PD publish (`comId=2002`) from HMI to gateway
- PD publish (`comId=2101..2108`) from HMI to gateway (one DoorCommandPayload frame per door, dataset 1002)
- MD listener (`comId=2201`) from gateway to HMI

HMI XML stack configuration is provided in `config/trdp_hmi.xml`.
Door command console input (runtime):

- Format: `<door_index> <cmd>`
- `door_index`: `0..7`
- `cmd`: `0` (open) or `1` (close)
- Each command updates payload bytes as: `B0=cmd`, `B1=alive_counter`, `B2..B7=0` and is sent on the selected door comId (`2101 + door_index`).

