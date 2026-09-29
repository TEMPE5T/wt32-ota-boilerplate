# Adding application code

The skeleton flashes and runs with no edits. The heartbeat task is there so a factory image proves the UART log, the link, and the address. Replace that task with the product. You do not need to open the Ethernet driver or the OTA server to add a FreeRTOS workload.

## Files you edit

**`components/user_app/user_app.c`**

`user_app_start` is called once, from `app_main`, after Ethernet is up and after a pending OTA image has been confirmed or was not pending. Create tasks here with `xTaskCreate` or `xTaskCreatePinnedToCore`. The sample creates `user_hb` at priority 5 with a 4096-byte stack. Delete that task when it is no longer useful; nothing else waits on it.

`user_app_diagnostics` is called only on the first boot of an OTA image, before the image is marked valid, and before `user_app_start`. Return `false` to roll back. Keep it short and deterministic. It runs on the main task. Do not start workers here that you expect to survive a rollback, and do not block for longer than you are willing to delay every OTA boot. The sample returns `true`.

**`components/user_app/CMakeLists.txt`**

Add `.c` files to `SRCS`. The component already requires `wt32_eth`, `esp_app_format`, `log`, and `freertos`. Add other ESP-IDF components to `REQUIRES` when you call into them (`nvs_flash`, `esp_timer`, `driver`, and so on).

**`CMakeLists.txt` at the project root**

`PROJECT_VER` is the version string in the image. Bump it for every build you send with `tools/ota_update.py`.

**`idf.py menuconfig` → WT32 Boilerplate**

Static IP, mask, gateway, DNS, DHCP on or off, DHCP wait, OTA port, OTA token, diagnostic wait. Set the address before the UART flash. The keys are listed in [configuration.md](configuration.md).

**`components/user_app/include/user_app.h`**

Only if you change the two function signatures. `ota_service` calls `user_app_diagnostics`. `app_main` calls `user_app_start`. Keep both.

## Files you should not need

| Path | Why it stays |
| --- | --- |
| `main/app_main.c` | Init order. Confirmation must stay before `user_app_start`, or a failing product task can run on an image that has already been marked valid. |
| `components/wt32_eth/` | WT32-ETH01 pinout, oscillator enable, DHCP-then-static. |
| `components/ota_service/` | HTTP server, `esp_ota_*`, rollback decision. |
| `partitions.csv` | Factory plus two OTA slots. Shrinking a slot below your image size makes `POST /ota` return 400. |
| `sdkconfig.defaults` | Rollback, 4 MB flash, external RMII clock on GPIO0. |

Changing pins because you have a different LAN8720 board belongs in `wt32_eth.c`. The WT32-ETH01 itself should keep the values that are there.

## Waiting for an address

`user_app_start` runs only after `wt32_eth_start` has returned. By then the board has either a DHCP lease, the static address, or neither (the failsafe string was invalid and DHCP did not answer). It does not wait for link. A task that needs a socket should wait:

```c
#include "wt32_eth.h"

static void product_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(wt32_eth_events(),
                         WT32_IP_READY_BIT,
                         pdFALSE,
                         pdTRUE,
                         portMAX_DELAY);
    char ip[16];
    wt32_eth_get_ip_str(ip, sizeof(ip));
    /* open sockets here */
    vTaskDelete(NULL);
}
```

`WT32_IP_READY_BIT` is cleared when the link drops if the address came from DHCP. It stays set when the static address is installed, because that address is still programmed on the netif. `wt32_eth_link_is_up()` and `wt32_eth_addr_mode()` are safe to call from any task.

Do not call `wt32_eth_start` again.

## What a diagnostic may assume

When `user_app_diagnostics` runs, `wt32_eth_driver_ready()` is already true or the OTA component has rolled back on its own. The link may be down. The address may be the static one, a lease, or absent. Do not fail the image only because `wt32_eth_link_is_up()` is false. The OTA component already treats a missing cable as "not a bad image", and a product check that undoes that will roll back every update performed with the cable unplugged.

A reasonable check is "the I2C sensor ACKs" or "the version byte in an external EEPROM matches". An unreasonable check is "a cloud server answered", because a bench with no route would reject every update.

## Pins the product must leave alone

GPIO0 (RMII clock in), GPIO16 (oscillator enable), and the RMII set GPIO18, GPIO19, GPIO21, GPIO22, GPIO23, GPIO25, GPIO26, GPIO27. GPIO1 and GPIO3 are the console and the UART download pins. The full table is in [hardware.md](hardware.md).

## Building the filled tree

Same commands as an empty one. From a machine with ESP-IDF exported:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The first programming of a new product image is still UART, even if the only difference from the skeleton is `user_app.c`. After that image is running and reachable, further revisions go through [ota.md](ota.md). OTA will refuse the binary if you changed the CMake `project()` name and the board is still running the old name. UART-flash once after a rename.

## FreeRTOS

Use the FreeRTOS API ESP-IDF builds with (`freertos/FreeRTOS.h`, `freertos/task.h`). Priorities above the idle task and below the Ethernet and Wi-Fi driver priorities (the Wi-Fi driver is not running) are appropriate for product work. The HTTP server and the EMAC tasks must still get CPU. A product task that spins at priority 20 will trip the 15 s task watchdog and reset the chip. After an image is confirmed, that reset reboots the same image. Before it is confirmed, that reset rolls back.

Stacks are in bytes, not words, with `xTaskCreate` in ESP-IDF. 4096 is enough for the sample log line. A task that does TLS or JSON needs more, and a stack overflow resets the chip the same way the watchdog does.

## Logging

`ESP_LOGI`, `ESP_LOGW`, and `ESP_LOGE` with a short tag. The default level is info. The heartbeat uses tag `user_app`. Follow that pattern so `idf.py monitor` traces stay readable next to `wt32_eth` and `ota_svc`.
