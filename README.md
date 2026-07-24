# ESP32 Multi-Scanner

A single-sketch ESP32 tool that scans **WiFi networks**, **Bluetooth Classic**, and **BLE** devices, exposes the results through a built-in web dashboard, and supports control via a serial command interface. Includes sorting/filtering, CSV/JSON export, WiFi client connection with basic diagnostics, and configurable periodic scanning.

## Features

- **WiFi scanning** — SSID, BSSID, RSSI, and encryption type for nearby networks.
- **Bluetooth Classic scanning** — device address, name (resolved from BDNAME or EIR data), and RSSI.
- **BLE scanning** — device address, name, RSSI, TX power, and advertised service UUIDs.
- **Web dashboard** served directly from the ESP32 (`http://192.168.4.1`) with tabs for each scan type, live sorting by column, and text filtering.
- **Serial command interface** for scanning, sorting, filtering, exporting, and WiFi connection management.
- **Periodic scanning** — scan continuously in the background at a configurable interval, restricted to a specific radio (WiFi, BT, BLE) or all three.
- **CSV / JSON export** of scan results over Serial.
- **WiFi station mode** — connect the ESP32 to an existing network alongside its own access point, with basic connectivity diagnostics (ping-style TCP reachability test).

## Hardware / Requirements

- Any ESP32 dev board with WiFi + Bluetooth (e.g. ESP32-WROOM-32).
- Arduino IDE with the **ESP32 board package** installed.
- Libraries (install via Library Manager):
  - `ESPAsyncWebServer`
  - `AsyncTCP`
- Board's built-in `WiFi`, `BLEDevice`, and `esp_bt_*` APIs (bundled with the ESP32 core — no extra install needed).

## Flashing Notes

This sketch pulls in WiFi + Bluetooth Classic (Bluedroid) + BLE + an async web server simultaneously, which produces a fairly large binary. If you hit:

```
Sketch too big; ... text section exceeds available space in board
```

Go to **Tools → Partition Scheme** in the Arduino IDE and pick one with a larger app partition, e.g. `Minimal SPIFFS (1.9MB APP with OTA)` or `Huge APP (3MB No OTA/1MB SPIFFS)`. No code changes are needed.

## Getting Started

1. Open the sketch in Arduino IDE, select your ESP32 board, and set an appropriate partition scheme (see above).
2. Upload the sketch.
3. Open Serial Monitor at **115200 baud**.
4. Connect to the ESP32's access point:
   - **SSID:** `ESP32-Scanner`
   - **Password:** `scanner123`
5. Visit `http://192.168.4.1` in a browser for the web dashboard, or use the serial commands below.

> Note: the AP SSID/password are set via `AP_SSID` / `AP_PASSWORD` at the top of the sketch — change and reflash if desired. The softAP password must be at least 8 characters.

## Serial Commands

| Command | Description |
|---|---|
| `help` | List all commands |
| `wifi` | Scan WiFi and print results |
| `wifi sort rssi\|ssid` | Sort last WiFi scan by RSSI or SSID |
| `wifi filter open` | Show only open (unencrypted) networks |
| `wifi filter ssid <text>` | Show networks whose SSID contains `<text>` |
| `wifi search <text>` | Alias for SSID filtering |
| `bt` | Scan Bluetooth Classic and print results |
| `bt sort name\|rssi` | Sort last BT scan by name or RSSI |
| `bt filter addr <text>` | Show BT devices whose address contains `<text>` |
| `ble` | Scan BLE and print results |
| `ble sort rssi\|name` | Sort last BLE scan by RSSI or name |
| `ble filter name <text>` | Show BLE devices whose name contains `<text>` |
| `connect <ssid> [password]` | Connect the ESP32 to a WiFi network as a station |
| `disconnect` | Disconnect from the WiFi station connection |
| `status` | Show current station connection info (SSID, IP, gateway, DNS) |
| `ping` | Test internet reachability (TCP to `8.8.8.8:53`) |
| `save <wifi\|bt\|ble\|all> <csv\|json>` | Export scan results over Serial (`all` only supports `json`) |
| `interval <seconds>` | Set the periodic scan interval (minimum 5s) |
| `scan start [wifi\|bt\|ble\|all]` | Start periodic scanning — defaults to `all` if no type given |
| `scan stop` | Stop periodic scanning |

**Examples:**
```
wifi sort rssi
ble filter name iPhone
connect MyHomeNetwork mypassword
save all json
scan start ble
interval 60
```

## Web Dashboard

Navigate to `http://192.168.4.1` after connecting to the ESP32's AP. The dashboard provides:

- Tabs for **WiFi**, **Bluetooth**, and **BLE**, each with a "Scan" button.
- Click any sortable column header to sort ascending/descending.
- Text filter box per tab to narrow results live.
- A status bar showing current WiFi station connection info, refreshed every 5 seconds.

### API Endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Web dashboard (HTML) |
| `/api/scan?type=wifi\|bt\|ble` | GET | Trigger a scan for the given type (blocks until complete) |
| `/api/wifi` | GET | Last WiFi scan results (JSON) |
| `/api/bt` | GET | Last Bluetooth Classic scan results (JSON) |
| `/api/ble` | GET | Last BLE scan results (JSON) |
| `/api/status` | GET | Current WiFi station connection status (JSON) |

## Known Limitations

- `/api/scan` runs synchronously on the async web server's task — a Bluetooth Classic scan (~10s) will briefly block other web requests while it runs. Fine for personal/local use; if you need the dashboard fully responsive during scans, move scanning into a separate FreeRTOS task.
- The periodic scan interval (`interval <seconds>`) is global — all radios selected via `scan start` share the same interval.
- SSIDs or connect passwords containing spaces aren't supported by the serial command parser (it splits on whitespace).
- Combining WiFi + Bluetooth Classic + BLE + async web server produces a large binary; a non-default partition scheme is required (see **Flashing Notes**).

## License

Add your preferred license here (e.g. MIT).
