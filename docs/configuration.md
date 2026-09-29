# Configuration

Two places decide what gets compiled into the image: `sdkconfig.defaults` (committed, applied the first time `idf.py` creates `sdkconfig`) and the **WT32 Boilerplate** menu (`main/Kconfig.projbuild`). `sdkconfig` itself is gitignored. It is the local result of menuconfig. Do not commit it.

Change the static address before the UART flash. It is a constant in the binary, not something the running board can be told later. If DHCP is down and the compiled address is wrong for the subnet, the only recovery is another UART flash or a DHCP server that does answer.

## Menu: WT32 Boilerplate

Open it with `idf.py menuconfig`.

### `WT32_STATIC_IP`

String. Default `192.168.1.50`.

The IPv4 address installed when DHCP is disabled, and when DHCP does not deliver a lease in time. Must be a dotted quad. `wt32_eth` parses it with `esp_netif_str_to_ip4` at startup and logs an error naming the field if it cannot. An invalid failsafe is not installed. The driver still starts, so a factory image does not reboot-loop, but the board then has no address unless DHCP works.

### `WT32_STATIC_NETMASK`

String. Default `255.255.255.0`.

Netmask written with the static address. Required even when you expect DHCP to succeed, because the failsafe has to be complete.

### `WT32_STATIC_GATEWAY`

String. Default `192.168.1.1`.

Gateway written with the static address. The boilerplate does not open outbound connections. User tasks that do will use this route after a static fallback.

### `WT32_STATIC_DNS`

String. Default `192.168.1.1`.

Main DNS server (`ESP_NETIF_DNS_MAIN`) written with the static address. Unused by the OTA server, which never resolves a name. User tasks that call `getaddrinfo` after a fallback see this server.

### `WT32_USE_DHCP`

Bool. Default on.

On: start the DHCP client, wait `WT32_DHCP_TIMEOUT_S`, then fall back. Off: do not wait, install the static address before `esp_eth_start` returns, and put the static address back any time the link comes up (the IDF netif glue would otherwise start DHCP again on `ETHERNET_EVENT_CONNECTED`).

Turn this off on a network that has no DHCP server and a long timeout you do not want to sit through on every boot. The static address is still mandatory in the image either way.

### `WT32_DHCP_TIMEOUT_S`

Integer 1–120. Default 15. Hidden when DHCP is off.

Seconds `wt32_eth_start` blocks in `app_main` waiting for `IP_EVENT_ETH_GOT_IP`. The wait runs even if the cable is unplugged, because the lease cannot arrive until the link is up and this code does not try to guess why it has not. Serial output is quiet during the wait, then either logs the lease or the static fallback.

### `WT32_OTA_PORT`

Integer 1–65535. Default 8080.

TCP port for `GET /info` and `POST /ota`. `tools/ota_update.py` defaults to the same port. Pass `--port` if you change it. The HTTP control port stays at the IDF default (32768) and is bound on localhost by the server task; do not set `WT32_OTA_PORT` to 32768.

### `WT32_OTA_TOKEN`

String. Default empty.

Empty: `POST /ota` does not check a header. The serial log says so at startup.

Non-empty: `POST /ota` requires header `X-OTA-Token` with the same bytes. A missing or wrong token is HTTP 401. `GET /info` does not require the token. The script flag is `--token`. The value is compiled into the image. It is not printed in the log. Pick something other than a password you use elsewhere; it is sent as a header on plain HTTP.

### `WT32_DIAG_TIMEOUT_S`

Integer 1–120. Default 20.

Used only on the first boot after an OTA, and only when the link is already up but `WT32_IP_READY_BIT` is not set yet. If the bit is still clear at the end, the image is rolled back. A down link does not start this timer.

## Version

`PROJECT_VER` in the top-level `CMakeLists.txt` is the version written into `esp_app_desc` (default `0.1.0`). It must be set before the `project()` line or the build ignores it.

Bump it for every image you push. The script reads the version out of the `.bin` and waits until `GET /info` reports that same string. Two different binaries that share a version look like a successful update as soon as the old image answers. A version that matches the last image the board marked invalid is rejected, so a rolled-back version cannot be pushed again until the string changes or `otadata` is erased.

The project name in the image is `wt32-ota-boilerplate`, from `project(wt32-ota-boilerplate)`. `POST /ota` rejects any other project name. Renaming the CMake project means the board will refuse binaries from the old name until you UART-flash the renamed image once.

## `sdkconfig.defaults`

These are not in the WT32 menu. They turn on behavior the skeleton depends on. Changing them without reading [architecture.md](architecture.md) will boot a board that cannot roll back or cannot see the PHY.

| Option | Value | Why it is set |
| --- | --- | --- |
| `CONFIG_IDF_TARGET` | `esp32` | The WT32-ETH01 is an ESP32, not S2/S3/C3. |
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | y | Module flash size. A 2 MB setting will not hold factory plus two 1280 KB slots. |
| `CONFIG_ESPTOOLPY_FLASHMODE_DIO` | y | Normal mode for this module. |
| `CONFIG_ESPTOOLPY_FLASHFREQ_40M` | y | Conservative flash clock. |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | Use `partitions.csv` instead of the single-app table. |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `partitions.csv` | Path relative to the project root. |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | y | Without this, `PENDING_VERIFY` is never used and a bad image stays selected. |
| `CONFIG_ETH_ENABLED` | y | Build the Ethernet driver. |
| `CONFIG_ETH_USE_ESP32_EMAC` | y | Internal MAC, not an SPI PHY. |
| `CONFIG_ETH_RMII_CLK_INPUT` | y | Clock comes from the PHY. Required for `ETH_ESP32_EMAC_DEFAULT_CONFIG()` on ESP-IDF 5.4, which errors out of the header if no clock mode is selected. |
| `CONFIG_ETH_RMII_CLK_IN_GPIO` | 0 | ESP32 can only input the RMII clock on GPIO0. |
| `CONFIG_ESP_TASK_WDT_EN` | y | Watchdog on. |
| `CONFIG_ESP_TASK_WDT_INIT` | y | Start it during startup. Harmless if your IDF version does not have this exact key. |
| `CONFIG_ESP_TASK_WDT_PANIC` | y | Timeout resets the chip instead of only printing. That reset is what triggers rollback when a new image hangs. |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | 15 | Long enough for a flash erase inside `esp_ota_begin` / `esp_ota_write`. |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 8192 | `app_main` brings up netif, Ethernet, and HTTP before returning. |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | 1024 | Room for the token header. |
| `CONFIG_LOG_DEFAULT_LEVEL_INFO` | y | The boot and OTA traces above are info, not debug. |

Not enabled, on purpose:

- `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` burns eFuse secure-version bits and then refuses older images forever. A boilerplate that people will reflash with experiments must not do that.
- Secure boot and flash encryption. They change the UART flashing procedure and are out of scope.
- Wi-Fi.

## After you change menuconfig

`idf.py build` then `idf.py flash` (or the OTA script, if a previous image is already running and reachable). Menuconfig edits the local `sdkconfig` only. Another checkout still has the defaults until someone runs menuconfig there too. If the static IP for a product line should be something other than `192.168.1.50`, change the `default` lines in `main/Kconfig.projbuild` so a fresh tree already carries it.
