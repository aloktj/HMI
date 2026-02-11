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
- PD publish (`comId=2101`) from HMI to gateway (DoorCommandAggregated, dataset 1002)
- MD listener (`comId=2102`) from gateway to HMI

HMI XML stack configuration is provided in `config/trdp_hmi.xml`.
