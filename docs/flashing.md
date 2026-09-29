# Flashing over USB-UART

The first image, whether you have added product tasks or you are flashing the skeleton as it stands, goes over UART. OTA cannot install the factory image, and it cannot recover a board that has no reachable IP. Wiring is in [hardware.md](hardware.md). Configuration that must be right before this step is in [configuration.md](configuration.md).

You need ESP-IDF 5.4 or newer, the target `esp32`, and a 3.3 V USB-UART adapter. This repository does not vendor the SDK.

## One-time setup

```bash
. $IDF_PATH/export.sh
cd /path/to/wt32-ota-boilerplate
idf.py set-target esp32
```

`set-target` writes `sdkconfig` from `sdkconfig.defaults` and creates the `build/` directory. Both are gitignored. Run it again only if you delete `sdkconfig` or you switch chips.

Then set the failsafe address for the network you will plug into:

```bash
idf.py menuconfig
```

**WT32 Boilerplate → Failsafe static IP**, plus netmask and gateway. Save and exit.

## Build

```bash
idf.py build
```

The application binary is `build/wt32-ota-boilerplate.bin`. The flashable set (bootloader, partition table, factory app) is what `idf.py flash` uses; you do not pass those paths yourself. The app is linked for the factory offset `0x10000` because `factory` is the first application partition in `partitions.csv`.

A clean tree builds. The heartbeat task is enough to see life on the console. You can also add product code first and flash that tree. The UART steps are the same. See [adding-application-code.md](adding-application-code.md).

## Download mode, by hand

Use this when the adapter has TX, RX, and GND only.

1. Board powered from a solid 5 V supply, grounds shared with the adapter. Adapter TX to board RXD, adapter RX to board TXD.
2. Hold IO0 to GND.
3. Pulse EN to GND and release EN (reset). The ROM samples IO0 low and waits for esptool.
4. Start the flash (next section).
5. When esptool is past "Connecting", you may release IO0.
6. When the flash finishes, make sure IO0 is released, then pulse EN again.

If IO0 is still held when the chip comes out of reset, it stays in the ROM instead of running the application. Ethernet will not start. The console will not show `wt32_eth`.

If esptool cannot connect, the usual causes are TX/RX not crossed, wrong serial port, IO0 not low during the reset, or EN not actually pulsed.

## Download mode, automatic

If the adapter's DTR is wired to IO0 and RTS to EN (directly or through the usual auto-reset transistors), `idf.py flash` toggles them. You do not hold a button. There is no button tied to GPIO0 on a stock WT32-ETH01 anyway.

## Flash and watch the log

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

`-p` is the serial device. Linux often uses `/dev/ttyUSB0` or `/dev/ttyACM0`. macOS uses `/dev/cu.usbserial-…`. Windows uses `COMx`. Monitor baud is 115200. Exit the monitor with Ctrl-].

Flash baud defaults to 460800. If the transfer fails halfway, retry with a slower baud:

```bash
idf.py -p /dev/ttyUSB0 -b 115200 flash
```

What a healthy boot prints, in outline:

- the compiled failsafe address, and whether DHCP will be tried
- `PHY oscillator enabled on GPIO16`
- `link up` once the cable is lit, or silence on the link line if it is not
- either `DHCP lease a.b.c.d` or `failsafe static address a.b.c.d`
- `listening on http://a.b.c.d:8080`
- `running partition "factory"`
- `heartbeat wt32-ota-boilerplate 0.1.0` every 5 seconds

`otadata` is left erased by a normal flash, so the bootloader selects factory. That line should say `factory` until the first network update.

## Erase

A full erase clears `otadata`, NVS, and both OTA slots, then you flash again:

```bash
idf.py -p /dev/ttyUSB0 erase-flash
idf.py -p /dev/ttyUSB0 flash
```

Use that when a rollback state is stuck, when you want factory to boot again regardless of which OTA slot was selected, or when you are handing the board to someone else. Erase does not change the compiled static IP. That only changes by rebuilding and flashing.

To wipe just the OTA selection and keep the factory image:

```bash
esptool.py -p /dev/ttyUSB0 erase_region 0xd000 0x2000
```

The next reset boots factory, because an erased `otadata` means "no OTA selection".

## After the flash

Confirm the address before you depend on it:

```bash
curl -s http://192.168.1.50:8080/info
```

Use the leased address instead if the log shows DHCP succeeded. A typical body:

```json
{"project":"wt32-ota-boilerplate","version":"0.1.0","idf_version":"v5.4.2","partition":"factory","ota_state":"factory_or_unknown","ip":"192.168.1.50","addr_mode":"static"}
```

`addr_mode` is `dhcp` or `static`. `ota_state` stays `factory_or_unknown` until an OTA image is running.

If `curl` hangs and the log shows the static address, the host is not on that subnet, or the failsafe address is already used by another device. Fix the menuconfig value and flash again. OTA cannot fix an address you cannot route to.

## Building without flashing

`idf.py build` does not need the board. `idf.py flash` does. This tree was written against the ESP-IDF 5.4 and 6.1 Ethernet and OTA APIs (`esp_eth_mac_new_esp32`, `smi_gpio`, `esp_ota_*`, rollback). It does not support ESP-IDF 4.x, which used the older Ethernet config struct.
