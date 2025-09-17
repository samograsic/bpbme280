# bpbme280

**One-shot BME280 telemetry over DTN (ION BP).**
Reads the BME280 sensor on a Raspberry Pi, adds CPU temperature and 1-minute CPU load, optionally includes location string, wraps everything in a compact single-line JSON payload with short field names, sends **one BP bundle**, then exits.

> JSON output uses concise field names, 1 decimal precision, and single-line format for efficient data transmission.

---

## Features

- 🚀 **DTN/ION**: Sends a single bundle via ION's BP API (same pattern as `bpsource`).
- 🌡️ **Sensors**: BME280 temperature, pressure, humidity (no WiringPi needed).
- 🧠 **System stats**: CPU temperature & 1-minute load average.
- 📦 **Compact JSON**: with short field names and 1 decimal precision.
- 📍 **Location support**: Optional location string identifier.
- 🧰 **One-shot**: Suitable for cron/systemd timers.

---

## Hardware

- Raspberry Pi (Pi 5 tested; any Pi with I²C should work)
- BME280 sensor (I²C)
- I²C wiring (typical breakout):
  - **VCC → 3V3**
  - **GND → GND**
  - **SDA → GPIO 2 (SDA)**
  - **SCL → GPIO 3 (SCL)**
- I²C address: **0x76** (default) or **0x77** depending on board (SDO pin).

---

## Software Requirements

- Raspberry Pi OS / Debian-based Linux
- ION DTN (BPv7) runtime and headers installed (e.g., `bp.h`)
- Build tools and I²C utilities

```bash
sudo apt-get update
sudo apt-get install -y build-essential i2c-tools
```

> Make sure ION is built/installed so `<bp.h>` and libs like `-lbp` are available.

---

## Enable I²C

```bash
sudo raspi-config        # Interface Options → I2C → Enable
sudo reboot
# Verify:
sudo i2cdetect -y 1      # Expect to see 76 or 77
```

---

## Build

### Using Makefile (Recommended)
```bash
make
```

### Manual build
```bash
gcc -O2 -Wall -Wextra -std=c11 -I../ione-code/bpv7/include -I../ione-code/ici/include -c bpbme280.c
gcc bpbme280.o -o bpbme280 -L/usr/local/lib -lbp -lici -lm -lpthread
```

> Depending on your ION build, `-lici` may or may not be required. Keep `-lpthread` for the attendant.

---

## Run

```bash
# Basic (defaults: TTL=300s, i2c=/dev/i2c-1, addr=0x76)
./bpbme280 ipn:268484800.6 ipn:268484820.1

# Custom TTL
./bpbme280 ipn:268484800.6 ipn:268484820.1 -t600

# Different I²C address or bus
./bpbme280 ipn:268484800.6 ipn:268484820.1 -a0x77 -d/dev/i2c-0

# With location
./bpbme280 ipn:268484800.6 ipn:268484820.1 -locLaboratory_A
```

**Arguments**

- `<destEID>`: Destination endpoint ID, e.g. `ipn:268484800.6`
- `<sourceEID>`: Source endpoint ID, e.g. `ipn:268484820.1`
- `-t<ttl>`: Bundle TTL in seconds (default `300`)
- `-a<hex>`: BME280 I²C address (default `0x76`, use `0x77` if needed)
- `-d<path>`: I²C device path (default `/dev/i2c-1`)
- `-loc<location>`: Location string identifier (optional)

---

## Output

The program prints the JSON it sends, then exits:

```
JSON: {"src":"ipn:268484820.1","ts":1758074375,"temp":27.6,"press":967.2,"humid":63.1,"cpu_temp":56.8,"load":0.64,"loc":"Laboratory_A"}
[i] bpbme280 sent one bundle and will exit.
```

---

## JSON Payload

Compact single-line JSON with short field names and 1 decimal precision:

```json
{"src":"ipn:268484820.1","ts":1758074375,"temp":27.6,"press":967.2,"humid":63.1,"cpu_temp":56.8,"load":0.64,"loc":"Laboratory_A"}
```

**Fields**

- `src`: Source endpoint ID (always included)
- `ts`: UNIX epoch seconds
- `temp`: BME280 temperature in °C (1 decimal)
- `press`: BME280 atmospheric pressure in hPa (1 decimal)
- `humid`: BME280 relative humidity in % (1 decimal)
- `cpu_temp`: Raspberry Pi CPU temperature in °C (1 decimal)
- `load`: System 1-minute load average (2 decimals)
- `loc`: Location string identifier (optional)

> Single-line format and compact field names minimize bandwidth usage. Location is included only when specified via command-line argument.

---

## Receiving the Bundle

Use your existing ION tools on the destination node (e.g., an app bound to the destination EID) to receive and parse the payload.

- A simple test setup is to point to an endpoint handled by a minimal BP sink/collector app that reads ZCOs and prints payloads to stdout, then parse the JSON.

---

## Scheduling (One-Shot by Design)

This tool sends **one** bundle and exits. To send periodically, use:

### systemd timer (recommended)

`/etc/systemd/system/bpbme280.service`
```ini
[Unit]
Description=Send one BME280 JSON bundle over BP

[Service]
Type=oneshot
ExecStart=/usr/local/bin/bpbme280 ipn:268484800.6 ipn:268484820.1 -t600
```

`/etc/systemd/system/bpbme280.timer`
```ini
[Unit]
Description=Run bpbme280 periodically

[Timer]
OnBootSec=1min
OnUnitActiveSec=5min
Unit=bpbme280.service

[Install]
WantedBy=timers.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now bpbme280.timer
```

**With location:**
```ini
[Service]
Type=oneshot
ExecStart=/usr/local/bin/bpbme280 ipn:268484800.6 ipn:268484820.1 -t600 -locLaboratory_A
```

### Cron (alternative)

```bash
*/5 * * * * /usr/local/bin/bpbme280 ipn:268484800.6 ipn:268484820.1 -t600 >/var/log/bpbme280.log 2>&1

# With location
*/5 * * * * /usr/local/bin/bpbme280 ipn:268484800.6 ipn:268484820.1 -t600 -locLaboratory_A >/var/log/bpbme280.log 2>&1
```

---

## Troubleshooting

- **`/dev/i2c-1` not found**  
  Enable I²C (`raspi-config`) and reboot.

- **No device at 0x76/0x77**  
  Check wiring, power (3V3), and address with `sudo i2cdetect -y 1`. Some boards use **0x77** (SDO high).

- **Permission denied on /dev/i2c-1**  
  Run with `sudo`, or add your user to the `i2c` group:
  ```bash
  sudo usermod -aG i2c $USER
  newgrp i2c
  ```

- **ION/BP errors (`bp_attach`, `bp_send`)**  
  Ensure ION is installed, running, and your destination EID is routable. Verify local BP config and neighbor links.

- **Unexpected BME280 chip-id**  
  The program warns if chip-id != `0x60`. Some clones misreport; most genuine BME280 report `0x60`. If measurements fail, recheck wiring and part type (BME vs BMP).

---

## Security & Reliability Notes

- JSON uses concise keys and no whitespace to minimize size. If you need signed/secured bundles, configure ION/BP security (BPSec) at the network level.
- For lossy links, prefer a TTL that exceeds your expected contact plan latency.
- Consider adding a node ID or sequence number if your receiver deduplicates payloads.

---

## Project Layout

```
.
├─ bpbme280.c     # main source
├─ Makefile       # build configuration
└─ readme.md      # this file
```

---

## License

Choose a license appropriate for your project (e.g., MIT, BSD-3-Clause, Apache-2.0) and add it here.

---

## Credits

- Based on ION’s `bpsource` pattern (Scott Burleigh / JPL) for bundle creation/sending.
- BME280 compensation formulas derived from the Bosch datasheet.
