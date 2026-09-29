# Over-the-air updates

Network updates use the ESP-IDF `app_update` component: `esp_ota_begin`, `esp_ota_write`, `esp_ota_end`, `esp_ota_set_boot_partition`, and the rollback calls. That is the same write path as Espressif's `native_ota_example`. The transport is inverted. The example downloads from a URL. This board listens, because the tool you already have is a `.bin` file and the board's IP.

This is not the Arduino `espota.py` protocol. Pointing that script at the board will not work.

The partition layout, the `otadata` state machine, and the power-cut behavior are described in [architecture.md](architecture.md). This page is the operator's view.

## The script

`tools/ota_update.py` uses the Python standard library only. No pip install.

```bash
python3 tools/ota_update.py 192.168.1.50 build/wt32-ota-boilerplate.bin
```

The first argument is the address the board is using right now. Pass the compiled static IP when the serial log says the failsafe was installed, or when DHCP is disabled. Pass the leased address when the log says DHCP succeeded. The script cannot discover the board.

```bash
python3 tools/ota_update.py --port 8080 --token secret --timeout 120 192.168.1.50 firmware.bin
python3 tools/ota_update.py --check-only firmware.bin
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `ip` | required | Board address. |
| `bin` | required | Application image from `idf.py build`. |
| `--port` | `8080` | Must match `WT32_OTA_PORT`. |
| `--token` | empty | Sent as `X-OTA-Token`. Must match `WT32_OTA_TOKEN` when that string is non-empty. |
| `--timeout` | `120` | Seconds to wait, after the upload, for `/info` to show the new version in a state other than `pending_verify`. |
| `--check-only` | off | Read the image header and exit. Does not open a socket. `ip` may be omitted. |

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | `/info` reports the version embedded in the file, and the OTA state is no longer `pending_verify`. |
| 1 | The file is missing, too small, or not an ESP-IDF application image (`0xE9` magic and `0xABCD5432` description magic). |
| 2 | The board answered with HTTP 400 or higher, and the body is printed. The running image was not changed. A malformed command line also exits 2, because that is argparse's usage-error code. |
| 3 | The upload may have been accepted, but the new version did not become the running image in time. Rollback is the usual reason. Read the serial log. |

Progress lines go to stderr. The final `running …` line goes to stdout.

Before it sends anything, the script checks the same header fields the board checks, and it pulls `version` from file offset `0x30`. After HTTP 200 it polls `GET /info` every 2 seconds. The board reboots about 500 ms after that 200, so a few polls fail with a connection error. That is expected. A poll that returns the previous version is also expected until the new image has confirmed itself.

If the TCP connection drops at the end of the upload, the script still polls. The board may have rebooted before the client read the 200.

## What you do on the host

1. Bump `PROJECT_VER` in the top-level `CMakeLists.txt`.
2. `idf.py build`.
3. Run the script against the address from the serial log or from `curl http://<ip>:8080/info`.
4. Watch stderr until it prints `running <version> on ota_0` (or `ota_1`).

The `.bin` must be the application image, `build/wt32-ota-boilerplate.bin`, not `bootloader.bin`, not `partition-table.bin`, and not the merged `flash_args` image. Those other files are not application images and are rejected, and a merged image is not what `esp_ota_write` expects.

## HTTP contract

### `GET /info`

No authentication.

```json
{
  "project": "wt32-ota-boilerplate",
  "version": "0.1.0",
  "idf_version": "v5.4.2",
  "partition": "factory",
  "ota_state": "factory_or_unknown",
  "ip": "192.168.1.50",
  "addr_mode": "static"
}
```

`ota_state` is one of `factory_or_unknown`, `new`, `pending_verify`, `valid`, `invalid`, `aborted`, `undefined`, `unknown`. After a confirmed update it is `valid`. During the first seconds of a new image it may be `pending_verify`. The script does not treat `pending_verify` as success, because the board might still roll back.

`addr_mode` is `dhcp`, `static`, or `none`.

### `POST /ota`

Headers:

- `Content-Type: application/octet-stream`
- `Content-Length: <bytes>`
- `X-OTA-Token: <token>` when a token was compiled in

Body: the raw `.bin`.

Success, then reboot:

```json
{"status":"ok","version":"0.2.0","partition":"ota_0"}
```

Errors:

```json
{"error":"image header rejected"}
```

| Status | When |
| --- | --- |
| 400 | No `Content-Length`, image larger than the slot, header rejected, body shorter than a header, hash check failed, or the client closed early. |
| 401 | Token missing or wrong. |
| 408 | The body stalled for too long (more than five 30 s receive timeouts). |
| 409 | The running image is still `pending_verify`. Wait, or reset the board so rollback can finish, then try again. |
| 500 | `esp_ota_begin`, `esp_ota_write`, or `esp_ota_set_boot_partition` failed, or there is no inactive OTA slot. |

The handler reads and discards a rejected body before it sends the error, so the client can finish the upload and still see the status line. The serial log has the specific reason (bad magic, project name, invalid-version match, size). The HTTP body stays short on purpose.

## Failsafes

Each one is implemented. None of them depend on the Python script being well behaved, except that a client which never sends `Content-Length` is told to go away.

1. **Inactive slot only.** `esp_ota_get_next_update_partition` never returns the running partition. Factory is never the write target. A botched upload cannot erase the application you last flashed over UART.
2. **Boot partition changes last.** `esp_ota_set_boot_partition` runs only after `esp_ota_end` returns `ESP_OK`. Any earlier failure calls `esp_ota_abort` or simply skips the boot-partition update. The previous image is what the next reset runs.
3. **Hash check.** `esp_ota_end` verifies the SHA-256 stored in the image. A truncated upload fails here.
4. **Project name.** The description must match the running project. Random ESP32 firmware is rejected.
5. **Size.** `Content-Length` above the slot size (1,310,720 bytes) is rejected before `esp_ota_begin`.
6. **Last invalid version.** If this exact version string is the one in the last partition marked invalid or aborted, the upload is rejected. That blocks a retry loop of an image the board already refused. Change `PROJECT_VER` to ship a fixed build.
7. **One pending image at a time.** HTTP 409 while the running image has not confirmed. You cannot stack a second update on top of an unconfirmed one.
8. **First-boot diagnostics.** Driver must have started. Link up with no address by the diagnostic timeout rolls back. Link down does not. `user_app_diagnostics()` can add a product check and force a rollback by returning false.
9. **Crash before confirm.** Panic, watchdog, or power loss while the state is `PENDING_VERIFY` makes the bootloader mark the image aborted and boot the previous one. The watchdog is 15 s and it panics, so a new image that wedges the CPU does not stay selected.
10. **Static IP.** If DHCP does not answer, the board still has the address you compiled in, and the script can reach it. A bad update that still brings the network up can be replaced. A bad update that never confirms is replaced automatically by the previous image, which still has that address.
11. **Optional token.** Set `WT32_OTA_TOKEN` before flashing a board that is not on a bench you control. Without it, anyone who can open TCP 8080 can post an image built from this project.
12. **Factory remains.** Erasing `otadata` (`esptool.py erase_region 0xd000 0x2000`) or a full `erase-flash` plus UART flash returns you to a known image. Rollback itself does not need that; it is the manual escape hatch when you want factory specifically, rather than "whatever was previous".

Rollback does not protect you from a confirmed image that later misbehaves. Confirmation happens before `user_app_start`, and it is deliberate that an unplugged cable is not a failure. Once `ota_state` is `valid`, you are on that image until you push another one or you erase `otadata` and reset (which boots factory, not the previous OTA slot).

## Watching a rollback

1. In `components/user_app/user_app.c`, make `user_app_diagnostics` return `false`.
2. Bump `PROJECT_VER` and build.
3. Run the script. The upload returns 200. The board reboots, logs `user_app_diagnostics rejected this image; rolling back`, and reboots again.
4. The script times out with exit code 3. `GET /info` shows the previous version. The serial log shows the previous partition.

Put the diagnostic function back before the next real update. Pushing the same version string again is rejected until you change `PROJECT_VER` or erase `otadata`.

To see the bootloader path instead of the explicit one, leave the diagnostic function returning true and add `abort()` at the top of `user_app_diagnostics`, before it returns. The image never calls `esp_ota_mark_app_valid_cancel_rollback`. The next reset (the panic) comes up on the previous image. Remove the `abort()` afterwards.

## Security limits

Plain HTTP on a LAN. The token is a shared string in the binary and on the command line, not a signature. The image is not signed unless you turn on secure boot yourself, which this skeleton does not configure. The project-name check stops accidents, not someone who rebuilds this project with a malicious `user_app`. Treat the OTA port as a programming interface.
