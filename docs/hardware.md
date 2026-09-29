# Hardware

The board this skeleton is written for is the Wireless-Tag WT32-ETH01: an ESP32 module and a LAN8720A Ethernet PHY on one PCB, with an RJ45 jack. Later revisions (v1.2 and v1.4 are the common ones) share this wiring. The PHY address is 1.

## Ethernet pins

The ESP32 RMII data pins are fixed by the chip. The two that the board wires itself are the management pins and the clock.

| Signal | GPIO | Direction | Notes |
| --- | --- | --- | --- |
| REF_CLK | 0 | Input to the ESP32 | 50 MHz from the PHY oscillator. Also a strapping pin. |
| Oscillator enable | 16 | Output, driven high by the app | Enables that oscillator. Not a PHY reset. |
| MDIO | 18 | Bidirectional | SMI data. |
| TXD0 | 19 | Output | Fixed RMII pin. |
| TX_EN | 21 | Output | Fixed RMII pin. |
| TXD1 | 22 | Output | Fixed RMII pin. |
| MDC | 23 | Output | SMI clock. |
| RXD0 | 25 | Input | Fixed RMII pin. |
| RXD1 | 26 | Input | Fixed RMII pin. |
| CRS_DV | 27 | Input | Fixed RMII pin. |
| PHY reset | — | Not connected | `reset_gpio_num` is `-1`. The PHY resets with power. |
| UART TX | 1 | Output | Console and download. |
| UART RX | 3 | Input | Console and download. |

Do not reuse GPIO0, GPIO16, or the RMII pins (18, 19, 21, 22, 23, 25, 26, 27) for product I/O. GPIO0 in particular cannot be a button on this board: it is the Ethernet clock.

## Why GPIO16 is driven high

GPIO0 is sampled at reset. If it is low, the ROM enters UART download mode and stays there until you reset again. The LAN8720 oscillator on this board is gated by GPIO16 so that it does not drive GPIO0 while the strap is sampled. The board pulls that enable down. Firmware must drive GPIO16 high after boot, or the PHY has no clock, the link LEDs stay off, and DHCP never starts.

`wt32_eth_start` does that before it installs the MAC, then waits 300 ms for the oscillator to settle. That matches Espressif's guidance for an external RMII clock on GPIO0: keep the clock quiet during strap, then enable it before the driver starts.

## Power

The ESP32 will run from a weak 5 V feed. The LAN8720 often will not. Power the board from a real 5 V supply (the `5V` pin, not the 3.3 V pin) that can hold the rail up when the PHY links. A USB-UART adapter's 5 V pin is a common reason the serial console looks fine and the Ethernet jack never links. Share ground between that supply, the adapter, and the board. Do not tie two 5 V sources together.

The jack LEDs are the quick check. If GPIO16 never goes high, they stay dark. If they light and the log still says the link is down, the cable or the switch port is next.

## USB-UART wiring

The WT32-ETH01 has no USB connector. A USB-UART adapter (3.3 V I/O, not 5 V I/O) is required for the first flash and for the serial log.

| Adapter | Board |
| --- | --- |
| GND | GND |
| RX | TXD (GPIO1) |
| TX | RXD (GPIO3) |
| RTS (optional) | EN |
| DTR (optional) | IO0 |

TX and RX are crossed. The ESP32 UART pins are 3.3 V. A 5 V TTL adapter will damage them.

`idf.py flash` can toggle EN and IO0 automatically when RTS and DTR are wired to the adapter (the usual auto-reset circuit: DTR to IO0, RTS to EN, through the capacitors or transistors on many adapter boards). A bare adapter with only TX, RX, and GND also works; you reset the board by hand. The sequence is in [flashing.md](flashing.md).

Leave IO0 open, or pulled up, for a normal boot. Holding it low keeps the ROM in the download window and the application never starts, so Ethernet never comes up. After a successful flash, release IO0 and pulse EN.

## Console

115200 8N1, no flow control. The log tags you will see from this tree are `app_main`, `wt32_eth`, `ota_svc`, and `user_app`.

## What this project does not enable

Wi-Fi is compiled out of the boot path. The external RMII clock (instead of the ESP32 APLL) is what makes Wi-Fi plus Ethernet possible later; the internal APLL clock fights the radio. Adding Wi-Fi is product work, not part of the skeleton, and it still must not reconfigure GPIO0.

There is no on-board user LED controlled by this firmware. The RJ45 LEDs are driven by the PHY.
