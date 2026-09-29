# WT32-ETH01 OTA boilerplate

FreeRTOS skeleton for the Wireless-Tag WT32-ETH01 (ESP32 + LAN8720), built with ESP-IDF 5.4 or newer. The image that ships here already brings up Ethernet, falls back to a compiled-in static IP when DHCP does not answer, and accepts an application binary over HTTP. Product code goes in one component. The first program is written over USB-UART. Later updates are pushed by `tools/ota_update.py`.

ESP-IDF is the firmware SDK. FreeRTOS is the kernel it already runs; this project does not vendor a second copy.

## Quick start

Install [ESP-IDF 5.4 or newer](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) and export it (`./export.sh`). From this directory:

```bash
idf.py set-target esp32
idf.py menuconfig          # WT32 Boilerplate → set the static IP for this network
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`idf.py flash` writes the factory image. With the cable plugged in, the serial log either reports a DHCP lease or the failsafe address (default `192.168.1.50`). A later update:

```bash
python3 tools/ota_update.py 192.168.1.50 build/wt32-ota-boilerplate.bin
```

Use the address the board actually has. That is the static address when DHCP failed or was disabled, and the leased address when DHCP succeeded.

## Where product code goes

| Change | File |
| --- | --- |
| Tasks and product logic | [components/user_app/user_app.c](components/user_app/user_app.c) |
| Extra source files | [components/user_app/CMakeLists.txt](components/user_app/CMakeLists.txt) |
| Image version | `PROJECT_VER` in [CMakeLists.txt](CMakeLists.txt) |
| Static IP, DHCP, OTA port, token | `idf.py menuconfig` → **WT32 Boilerplate** |

Leave `components/wt32_eth`, `components/ota_service`, and `main/app_main.c` alone unless the board wiring or the OTA policy changes. Details are in [docs/adding-application-code.md](docs/adding-application-code.md).

## Documentation

- [Architecture](docs/architecture.md) — boot order, tasks, partitions, rollback states
- [Hardware](docs/hardware.md) — WT32-ETH01 pins, 5 V supply, USB-UART wiring
- [Configuration](docs/configuration.md) — every menuconfig key and the version string
- [Flashing](docs/flashing.md) — UART download mode, build, erase, monitor
- [OTA](docs/ota.md) — HTTP contract, the Python script, and each failsafe
- [Adding application code](docs/adding-application-code.md) — the files product firmware should edit

## What this skeleton does not do

No Wi-Fi, no TLS, no secure boot, and no anti-rollback eFuses. OTA is plain HTTP on the Ethernet LAN, with an optional shared token. That is intentional for a bench and a trusted line. Put a token on any board that shares a network, and do not expose port 8080 beyond that network.

## License

Apache-2.0. See [LICENSE](LICENSE).
