# Architecture

The skeleton is an ESP-IDF project. `app_main` only sequences the pieces. Ethernet lives in `components/wt32_eth`. The HTTP OTA server and the rollback decision live in `components/ota_service`. Product tasks live in `components/user_app`.

ESP-IDF supplies FreeRTOS (SMP, both cores unless you change the sdkconfig). The boilerplate creates one application task, the heartbeat. The Ethernet driver, the default event loop, and the HTTP server create their own tasks.

## Boot order

1. `nvs_flash_init`. If the NVS partition is truncated or its version changed, it is erased once and initialized again. NVS is not used for the IP address. The failsafe address is compiled in so a wiped NVS cannot strand the board.
2. `esp_netif_init` and the default event loop.
3. `wt32_eth_start`:
   - Drive GPIO16 high and wait 300 ms so the LAN8720's 50 MHz oscillator is running before the MAC looks for a clock on GPIO0.
   - Install the ESP32 EMAC and a LAN87xx PHY at address 1. The PHY reset pin is not connected on this board.
   - Attach the default Ethernet netif and start the driver.
   - If **Try DHCP first** is on, wait `WT32_DHCP_TIMEOUT_S` (default 15 s) for `IP_EVENT_ETH_GOT_IP`. On timeout, stop the DHCP client and install the static address, mask, gateway, and DNS.
   - If DHCP is disabled, install the static address immediately. A later link-up event from the netif glue starts the DHCP client again; the Ethernet handler stops it and writes the static address back.
4. `ota_service_start` listens on `WT32_OTA_PORT` (default 8080).
5. `ota_service_confirm_image` runs diagnostics when this boot is the first boot of an OTA image. Failure reboots into the previous application and does not return.
6. `user_app_start` creates product tasks.

A factory image (the one `idf.py flash` writes) has no OTA state, so step 5 returns immediately. Confirmation exists only after a network update.

## Event bits

`wt32_eth_events()` returns a FreeRTOS event group:

| Bit | Name | Meaning |
| --- | --- | --- |
| `BIT0` | `WT32_IP_READY_BIT` | A DHCP lease or the static address is installed. Cleared on link-down while DHCP is the active mode. Left set across link-down when the static address is installed. |
| `BIT1` | `WT32_ETH_READY_BIT` | `esp_eth_start` succeeded. |
| `BIT2` | `WT32_LINK_UP_BIT` | PHY link is up. |

`wt32_eth_addr_mode()` returns `"dhcp"`, `"static"`, or `"none"`. `wt32_eth_get_ip_str` reads the address currently programmed on the netif.

## Tasks

| Task | Who creates it | Role |
| --- | --- | --- |
| `main` | ESP-IDF | `app_main`. Blocks inside the DHCP wait, then returns after `user_app_start`. Stack is 8192 bytes (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`). |
| event loop | `esp_event_loop_create_default` | Ethernet and IP events. |
| EMAC / RX | `esp_eth` | MAC driver internal tasks. |
| `httpd` | `esp_http_server` | `GET /info` and `POST /ota`. Stack is 10240 bytes. The OTA body is written to flash from this task. |
| `user_hb` | `user_app_start` | Sample. Logs project, version, IP, address mode, and link every 5 s. Priority 5, stack 4096 bytes. Replace it. |
| IDLE0 / IDLE1 | FreeRTOS | Subscribed to the task watchdog. |

The task watchdog is 15 s and panics. A task that starves the idle tasks resets the chip. If that reset happens before a new OTA image confirms itself, the bootloader will not boot that image again.

## Flash map

4 MB flash. Application slots are 1,310,720 bytes (`0x140000`) and 64 KB aligned. An image larger than that is rejected before it is written.

| Name | Type | Offset | Size | Role |
| --- | --- | --- | --- | --- |
| bootloader | — | `0x1000` | up to the partition table | Second-stage bootloader. Not updated by this project. |
| partition table | — | `0x8000` | `0xC00` | Generated from `partitions.csv`. |
| `nvs` | data | `0x9000` | 16 KB | IDF NVS. Not the IP configuration. |
| `otadata` | data | `0xD000` | 8 KB | Two sectors. Selects `ota_0` or `ota_1` and stores the rollback state. |
| `phy_init` | data | `0xF000` | 4 KB | PHY calibration data used by ESP-IDF. |
| `factory` | app | `0x10000` | 1280 KB | Image written by `idf.py flash`. Booted when `otadata` is erased (`0xFF`). |
| `ota_0` | app | `0x150000` | 1280 KB | First network update, then every other update. |
| `ota_1` | app | `0x290000` | 1280 KB | The other network slot. |

`0x290000 + 0x140000 = 0x3D0000`. The flash ends at `0x400000`, so about 192 KB is unused.

`idf.py flash` programs the bootloader, the partition table, and the application at the factory offset. It does not write `otadata`. An erased `otadata` makes the bootloader select factory. The first `POST /ota` therefore lands in `ota_0`. The next one lands in `ota_1`, then `ota_0` again. `esp_ota_get_next_update_partition` picks the slot that is not the running one. Factory is never the destination of a network update, so a bad OTA cannot erase the UART-flashed image.

## How an update is stored

`POST /ota` streams the body into the inactive slot:

1. `esp_ota_begin` with the `Content-Length`. The running slot is not touched.
2. `esp_ota_write` for every received block. The body is not assembled in RAM. The receive buffer is 4096 bytes.
3. After the first 288 bytes are in hand (24-byte image header, 8-byte segment header, 256-byte `esp_app_desc`), the handler checks the image. A rejection calls `esp_ota_abort`. The boot partition is unchanged. The inactive slot may be partly erased; that is harmless because it is not selected.
4. `esp_ota_end` checks the image hash. Failure does not call `esp_ota_set_boot_partition`.
5. Only then `esp_ota_set_boot_partition` updates `otadata` and marks the image `ESP_OTA_IMG_NEW`.
6. The handler sends HTTP 200 and reboots 500 ms later.

A power cut during `esp_ota_write` or during `esp_ota_end` leaves `otadata` pointing at the previous application. A power cut while `otadata` itself is being written is covered by the two sectors in that partition: they are written as a pair, and the bootloader keeps the sector with the newer sequence counter if they disagree. This is the ESP-IDF "safe update" path for application images. It is not the unsafe path used for bootloader or partition-table updates. This project does not OTA those.

## Rollback states

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on. These states live in `otadata`, not in the application binary. Factory is not an OTA slot and does not go through this machine.

| State | Who writes it | What the next bootloader pass does |
| --- | --- | --- |
| `ESP_OTA_IMG_NEW` | `esp_ota_set_boot_partition` | Selects this image once and rewrites the state to `ESP_OTA_IMG_PENDING_VERIFY`. |
| `ESP_OTA_IMG_PENDING_VERIFY` | Bootloader, from `NEW` | If this state is still here at the start of a later boot, rewrites it to `ESP_OTA_IMG_ABORTED` and picks the previous image. |
| `ESP_OTA_IMG_VALID` | `esp_ota_mark_app_valid_cancel_rollback` | No restriction. This is the image that will keep booting. |
| `ESP_OTA_IMG_INVALID` | `esp_ota_mark_app_invalid_rollback_and_reboot` | Will not be selected. |
| `ESP_OTA_IMG_ABORTED` | Bootloader, when `PENDING_VERIFY` was not cleared | Will not be selected. |
| `ESP_OTA_IMG_UNDEFINED` | Older images / no state | Selected, with no rollback tracking. |

So a new image gets one boot to prove itself. The check in `ota_service_confirm_image` is:

- The Ethernet driver must have reached `esp_eth_start`. If it has not, the image is marked invalid and the chip reboots into the previous app.
- If the link is already up, an IP address must be installed, waiting up to `WT32_DIAG_TIMEOUT_S` (default 20 s) for `WT32_IP_READY_BIT`. DHCP has already had its own wait inside `wt32_eth_start`, and a failed lease has already installed the static address, so this second wait is only the gap between "link up" and "address programmed".
- If the link is down, the image is not failed. An unplugged cable, or a switch that has not lit the link yet, must not discard a good update. The board still has the compiled-in static address ready for when the cable returns, or it will take a DHCP lease then.
- `user_app_diagnostics()` must return true. The sample returns true. A product check (a required peripheral, a version of a companion MCU, a calibration page) belongs here.

Passing all of that calls `esp_ota_mark_app_valid_cancel_rollback`. Failing calls `esp_ota_mark_app_invalid_rollback_and_reboot`.

A panic, a task-watchdog reset, or a power cut before that call leaves `PENDING_VERIFY` in place. The following reset becomes `ABORTED` and the previous image runs. There is no second chance for that same slot until a new image is written over it.

`POST /ota` is refused with HTTP 409 while the running image is still `PENDING_VERIFY`. The script waits until `/info` reports the new version and a state other than `pending_verify`.

Confirmation runs before `user_app_start`. A bug in a long-running product task, after the image has been marked valid, does not roll back. Put checks that must reject an update into `user_app_diagnostics`, which runs while the image is still pending.

## Header checks before the hash

These run as soon as the description is in RAM, and they abort the OTA handle on failure:

- First byte is `0xE9` (`ESP_IMAGE_HEADER_MAGIC`).
- `esp_app_desc.magic_word` is `0xABCD5432`.
- `project_name` matches the running image. A binary built from another ESP-IDF project is rejected even if it is a valid ESP32 image. The project name comes from `project(wt32-ota-boilerplate)` in the top-level `CMakeLists.txt`.
- The version string is not equal to the version in `esp_ota_get_last_invalid_partition()`. That stops the script from immediately pushing back an image the board just rolled back. Bump `PROJECT_VER`, or erase `otadata` over UART, to try a rejected version again on purpose.
- `Content-Length` is greater than zero, at least a full header, and no larger than the inactive slot.

`esp_ota_end` then checks the SHA-256 appended to the image. A truncated or corrupted body fails there, after the slot write, and the boot partition is left alone.

The same header layout is what `tools/ota_update.py` reads so it knows which version to wait for. The description starts at file offset `0x20`. `version` is 32 bytes at offset `0x30`. `project_name` is 32 bytes at offset `0x50`.

## HTTP surface

| Method and path | Body | Result |
| --- | --- | --- |
| `GET /info` | none | JSON: `project`, `version`, `idf_version`, `partition`, `ota_state`, `ip`, `addr_mode`. Always open, including when a token is set, so a rolled-back board can still be identified. |
| `POST /ota` | raw application `.bin`, `Content-Type: application/octet-stream`, `Content-Length` set | `200` and `{"status":"ok","version":"...","partition":"ota_0"}` then reboot. Errors are JSON `{"error":"..."}` with 400, 401, 408, 409, or 500. |

Chunked transfer encoding is rejected. The script sets `Content-Length`. When `WT32_OTA_TOKEN` is non-empty, `POST /ota` must send the same value in `X-OTA-Token`. The comparison walks every byte of both strings so it does not stop at the first mismatch.

There is no TLS. Anyone who can route to the port can upload an image that carries this project's name, unless the token is set. See [ota.md](ota.md).

## What is logged

Serial output is at info level. Expect, in order: the compiled failsafe address, oscillator enable, link up or the DHCP wait, either a lease or a warning that the static address was installed, the HTTP URL, the running partition, then a heartbeat every 5 s. An OTA adds byte-count lines every 64 KB, the slot name, and the reboot line. A rollback logs the reason before `esp_ota_mark_app_invalid_rollback_and_reboot`.
