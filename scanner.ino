/*
 * ESP32 Multi-Scanner: WiFi, Bluetooth Classic, BLE
 * Features: continuous scanning, web interface, sorting/filtering, 
 *           WiFi connection & diagnostics, CSV/JSON export
 * 
 * Dependencies: ESPAsyncWebServer, AsyncTCP (install via Library Manager)
 */

#include <WiFi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <vector>
#include <algorithm>
#include <cctype>
#include <ESPAsyncWebServer.h>

// ---------- Definitions ----------
#define BT_DISCOVERY_DURATION 10
#define BLE_SCAN_DURATION 5      // seconds
#define AP_SSID "ESP32-Scanner"
#define AP_PASSWORD "scanner123"

// ---------- Data structures ----------
struct WiFiNet {
    String ssid;
    String bssid;
    int rssi;
    String encryption;
};

struct BTDev {
    String address;
    String name;
    int rssi;
};

struct BLEDev {
    String address;
    String name;
    int rssi;
    int txPower;
    std::vector<String> serviceUUIDs;
};

// ---------- Global scan results ----------
std::vector<WiFiNet> wifiNetworks;
std::vector<BTDev> btDevices;
std::vector<BLEDev> bleDevices;

// ---------- Scanning flags & intervals ----------
bool scanInProgress = false;        // prevents overlapping manual scans
bool periodicScanEnabled = false;
unsigned long periodicInterval = 30000;   // default 30 sec
unsigned long lastScanMillis = 0;

// ---------- WiFi connection state ----------
bool staConnected = false;
String staSSID, staIP, staGW, staDNS;

// ---------- Bluetooth init flag ----------
bool btInitialized = false;

// ---------- BT discovery completion flag (set by GAP callback) ----------
volatile bool btDiscoveryDone = false;

// ---------- BLE scan callbacks ----------
class MyBLEAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        BLEDev dev;
        dev.address = advertisedDevice.getAddress().toString().c_str();
        if (advertisedDevice.haveName())
            dev.name = advertisedDevice.getName().c_str();
        else
            dev.name = "(unknown)";
        dev.rssi = advertisedDevice.getRSSI();
        dev.txPower = advertisedDevice.haveTXPower() ? advertisedDevice.getTXPower() : -128;
        
        // Extract service UUIDs
        if (advertisedDevice.haveServiceUUID()) {
            BLEUUID uuid = advertisedDevice.getServiceUUID();
            dev.serviceUUIDs.push_back(uuid.toString().c_str());
        }
        if (advertisedDevice.haveServiceData()) {
            // more UUIDs can be extracted, but keep simple for demo
        }
        
        bleDevices.push_back(dev);
        Serial.printf("  BLE: %s (%s) RSSI:%d TX:%d\n",
                      dev.name.c_str(), dev.address.c_str(), dev.rssi, dev.txPower);
    }
};

// ---------- Bluetooth GAP callback ----------
// NOTE: esp_bt_gap_cb_param_t's disc_res_param does NOT carry a "dev_name" or
// "rssi" field directly. Instead, each discovered device reports a set of
// key/value properties (esp_bt_gap_dev_prop_t) in disc_res.prop[], and you
// must scan that array for the property types you care about
// (ESP_BT_GAP_DEV_PROP_BDNAME, ESP_BT_GAP_DEV_PROP_RSSI, ESP_BT_GAP_DEV_PROP_EIR, ...).
// The name is frequently absent from BDNAME and only available inside the
// EIR blob, so we fall back to parsing that too.
void btGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            BTDev dev;
            char addr[18];
            sprintf(addr, "%02X:%02X:%02X:%02X:%02X:%02X",
                    param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
                    param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);
            dev.address = String(addr);
            dev.name = "(unknown)";
            dev.rssi = 0;

            for (int i = 0; i < param->disc_res.num_prop; i++) {
                esp_bt_gap_dev_prop_t *prop = param->disc_res.prop + i;
                switch (prop->type) {
                    case ESP_BT_GAP_DEV_PROP_BDNAME: {
                        // val is not guaranteed to be null-terminated; use len explicitly
                        int len = prop->len;
                        if (len > 0) {
                            // Trim any accidental trailing null already included in len
                            if (((char*)prop->val)[len - 1] == '\0') len--;
                            dev.name = String((const char*)prop->val, len);
                        }
                        break;
                    }
                    case ESP_BT_GAP_DEV_PROP_RSSI: {
                        dev.rssi = *(int8_t*)(prop->val);
                        break;
                    }
                    case ESP_BT_GAP_DEV_PROP_EIR: {
                        if (dev.name == "(unknown)") {
                            uint8_t eirLen = 0;
                            uint8_t *eirName = esp_bt_gap_resolve_eir_data(
                                (uint8_t*)prop->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &eirLen);
                            if (!eirName) {
                                eirName = esp_bt_gap_resolve_eir_data(
                                    (uint8_t*)prop->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &eirLen);
                            }
                            if (eirName && eirLen > 0) {
                                dev.name = String((const char*)eirName, eirLen);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            btDevices.push_back(dev);
            Serial.printf("  BT: %s (%s) RSSI:%d\n", dev.name.c_str(), dev.address.c_str(), dev.rssi);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                btDiscoveryDone = true;
                Serial.println("BT discovery stopped.");
            }
            break;
        default:
            break;
    }
}

// ---------- Initialize Bluetooth ----------
void initBluetooth() {
    if (btInitialized) return;
    if (!btStart()) { Serial.println("BT controller init failed"); return; }
    if (esp_bluedroid_init() != ESP_OK) { Serial.println("Bluedroid init failed"); return; }
    if (esp_bluedroid_enable() != ESP_OK) { Serial.println("Bluedroid enable failed"); return; }
    esp_bt_gap_register_callback(btGapCallback);
    esp_bt_dev_set_device_name("ESP32_BT");
    btInitialized = true;
}

// ---------- BLE initialization ----------
BLEScan* pBLEScan = nullptr;
void initBLE() {
    if (pBLEScan) return;
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyBLEAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

// ---------- WiFi scan ----------
void performWiFiScan() {
    WiFi.mode(WIFI_AP_STA); // keep the softAP alive; don't drop back to plain STA
    int n = WiFi.scanNetworks();
    wifiNetworks.clear();
    for (int i = 0; i < n; i++) {
        WiFiNet net;
        net.ssid = WiFi.SSID(i);
        net.bssid = WiFi.BSSIDstr(i);
        net.rssi = WiFi.RSSI(i);
        switch (WiFi.encryptionType(i)) {
            case WIFI_AUTH_OPEN: net.encryption = "Open"; break;
            case WIFI_AUTH_WEP: net.encryption = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: net.encryption = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: net.encryption = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: net.encryption = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: net.encryption = "WPA2 Ent."; break;
            case WIFI_AUTH_WPA3_PSK: net.encryption = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: net.encryption = "WPA2/WPA3"; break;
            default: net.encryption = "Unknown";
        }
        wifiNetworks.push_back(net);
    }
    WiFi.scanDelete();
}

// ---------- BT scan ----------
void performBTScan() {
    initBluetooth();
    if (!btInitialized) return;
    btDevices.clear();
    btDiscoveryDone = false;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, BT_DISCOVERY_DURATION, 0);
    unsigned long start = millis();
    // Wait for the GAP callback to report discovery stopped, with a hard
    // timeout as a safety net in case the event is missed.
    while (!btDiscoveryDone && millis() - start < (unsigned long)(BT_DISCOVERY_DURATION * 1000 + 3000)) {
        delay(50);
    }
    if (!btDiscoveryDone) {
        esp_bt_gap_cancel_discovery();
    }
    delay(200);
}

// ---------- BLE scan ----------
void performBLEScan() {
    initBLE();
    bleDevices.clear();
    pBLEScan->start(BLE_SCAN_DURATION, false);
    delay(BLE_SCAN_DURATION * 1000 + 500); // wait for completion
    pBLEScan->stop();
    pBLEScan->clearResults();
}

// ---------- Master scan function (WiFi+BT+BLE) ----------
void fullScan() {
    if (scanInProgress) return;
    scanInProgress = true;
    Serial.println("=== Starting full scan cycle ===");
    performWiFiScan();
    performBTScan();
    performBLEScan();
    Serial.println("=== Scan complete ===");
    scanInProgress = false;
}

// ---------- Sorting helpers ----------
void sortWiFiByRSSI() {
    std::sort(wifiNetworks.begin(), wifiNetworks.end(),
        [](const WiFiNet& a, const WiFiNet& b) { return a.rssi > b.rssi; });
}
void sortWiFiBySSID() {
    std::sort(wifiNetworks.begin(), wifiNetworks.end(),
        [](const WiFiNet& a, const WiFiNet& b) { return a.ssid < b.ssid; });
}
void sortBTByName() {
    std::sort(btDevices.begin(), btDevices.end(),
        [](const BTDev& a, const BTDev& b) { return a.name < b.name; });
}
void sortBTByRSSI() {
    std::sort(btDevices.begin(), btDevices.end(),
        [](const BTDev& a, const BTDev& b) { return a.rssi > b.rssi; });
}
void sortBLEByRSSI() {
    std::sort(bleDevices.begin(), bleDevices.end(),
        [](const BLEDev& a, const BLEDev& b) { return a.rssi > b.rssi; });
}
void sortBLEByName() {
    std::sort(bleDevices.begin(), bleDevices.end(),
        [](const BLEDev& a, const BLEDev& b) { return a.name < b.name; });
}

// ---------- Serial print helpers ----------
void printWiFiResults() {
    if (wifiNetworks.empty()) { Serial.println("No WiFi networks stored."); return; }
    Serial.println("No. SSID                             RSSI  Encryption");
    for (size_t i = 0; i < wifiNetworks.size(); i++) {
        auto& n = wifiNetworks[i];
        Serial.printf("%2d. %-32s %4d dBm  %s\n", (int)(i+1), n.ssid.c_str(), n.rssi, n.encryption.c_str());
    }
}

void printBTResults() {
    if (btDevices.empty()) { Serial.println("No BT devices."); return; }
    Serial.println("No. Name                      Address            RSSI");
    for (size_t i = 0; i < btDevices.size(); i++) {
        auto& d = btDevices[i];
        Serial.printf("%2d. %-25s %17s %d\n", (int)(i+1), d.name.c_str(), d.address.c_str(), d.rssi);
    }
}

void printBLEResults() {
    if (bleDevices.empty()) { Serial.println("No BLE devices."); return; }
    Serial.println("No. Name                      Address            RSSI  TXPower");
    for (size_t i = 0; i < bleDevices.size(); i++) {
        auto& d = bleDevices[i];
        Serial.printf("%2d. %-25s %17s %4d  %d\n", (int)(i+1), d.name.c_str(), d.address.c_str(), d.rssi, d.txPower);
    }
}

// ---------- CSV/JSON export ----------
void exportWiFiCSV() {
    Serial.println("-----BEGIN WIFI CSV-----");
    Serial.println("SSID,BSSID,RSSI,Encryption");
    for (auto& n : wifiNetworks) {
        String ssid = n.ssid; ssid.replace("\"", "\"\"");
        Serial.printf("\"%s\",\"%s\",%d,%s\n", ssid.c_str(), n.bssid.c_str(), n.rssi, n.encryption.c_str());
    }
    Serial.println("-----END WIFI CSV-----");
}

void exportBTCSV() {
    Serial.println("-----BEGIN BT CSV-----");
    Serial.println("Name,Address,RSSI");
    for (auto& d : btDevices) {
        String name = d.name; name.replace("\"", "\"\"");
        Serial.printf("\"%s\",\"%s\",%d\n", name.c_str(), d.address.c_str(), d.rssi);
    }
    Serial.println("-----END BT CSV-----");
}

void exportBLECSV() {
    Serial.println("-----BEGIN BLE CSV-----");
    Serial.println("Name,Address,RSSI,TXPower,ServiceUUIDs");
    for (auto& d : bleDevices) {
        String name = d.name; name.replace("\"", "\"\"");
        String uuids = "";
        for (auto& u : d.serviceUUIDs) uuids += u + ";";
        uuids.replace("\"", "\"\"");
        Serial.printf("\"%s\",\"%s\",%d,%d,\"%s\"\n", name.c_str(), d.address.c_str(), d.rssi, d.txPower, uuids.c_str());
    }
    Serial.println("-----END BLE CSV-----");
}

String jsonEscape(const String& s) {
    String r = s;
    r.replace("\\", "\\\\");
    r.replace("\"", "\\\"");
    return r;
}

void exportAllJSON() {
    Serial.println("-----BEGIN ALL JSON-----");
    String json = "{";
    // WiFi array
    json += "\"wifi\":[";
    for (size_t i=0; i<wifiNetworks.size(); i++) {
        if (i>0) json += ",";
        json += "{\"ssid\":\"" + jsonEscape(wifiNetworks[i].ssid) + "\",\"bssid\":\"" + wifiNetworks[i].bssid + "\",\"rssi\":" + String(wifiNetworks[i].rssi) + ",\"enc\":\"" + wifiNetworks[i].encryption + "\"}";
    }
    json += "],\"bt\":[";
    for (size_t i=0; i<btDevices.size(); i++) {
        if (i>0) json += ",";
        json += "{\"name\":\"" + jsonEscape(btDevices[i].name) + "\",\"addr\":\"" + btDevices[i].address + "\",\"rssi\":" + String(btDevices[i].rssi) + "}";
    }
    json += "],\"ble\":[";
    for (size_t i=0; i<bleDevices.size(); i++) {
        if (i>0) json += ",";
        json += "{\"name\":\"" + jsonEscape(bleDevices[i].name) + "\",\"addr\":\"" + bleDevices[i].address + "\",\"rssi\":" + String(bleDevices[i].rssi) + ",\"txPower\":" + String(bleDevices[i].txPower) + ",\"uuids\":[";
        for (size_t j=0; j<bleDevices[i].serviceUUIDs.size(); j++) {
            if (j>0) json += ",";
            json += "\"" + jsonEscape(bleDevices[i].serviceUUIDs[j]) + "\"";
        }
        json += "]}";
    }
    json += "]}";
    Serial.println(json);
    Serial.println("-----END ALL JSON-----");
}

// ---------- WiFi connection & diagnostics ----------
void connectWiFi(String ssid, String pass) {
    WiFi.mode(WIFI_AP_STA);
    Serial.printf("Connecting to %s...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        staConnected = true;
        staSSID = ssid;
        staIP = WiFi.localIP().toString();
        staGW = WiFi.gatewayIP().toString();
        staDNS = WiFi.dnsIP().toString();
        Serial.println("\nConnected!");
        Serial.printf("IP: %s, GW: %s, DNS: %s\n", staIP.c_str(), staGW.c_str(), staDNS.c_str());
    } else {
        staConnected = false;
        Serial.println("\nConnection failed.");
    }
}

void disconnectWiFi() {
    WiFi.disconnect();
    staConnected = false;
    Serial.println("Disconnected.");
}

void pingTest() {
    if (!staConnected) {
        Serial.println("Not connected to any network.");
        return;
    }
    Serial.println("Testing connectivity (TCP to 8.8.8.8:53)...");
    WiFiClient client;
    if (client.connect("8.8.8.8", 53, 2000)) {
        Serial.println("Success! Internet is reachable.");
        client.stop();
    } else {
        Serial.println("Failed to reach 8.8.8.8.");
    }
}

// ---------- Web server ----------
AsyncWebServer server(80);

String generateHTML() {
    return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Scanner</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; margin: 0; padding: 20px; background: #f4f4f4; }
        h1 { color: #333; }
        .tabs { display: flex; border-bottom: 1px solid #ccc; margin-bottom: 10px; }
        .tab { padding: 10px 20px; cursor: pointer; background: #e0e0e0; margin-right: 5px; border-radius: 5px 5px 0 0; }
        .tab.active { background: #fff; border: 1px solid #ccc; border-bottom: none; }
        .panel { display: none; background: white; padding: 15px; border-radius: 0 5px 5px 5px; }
        .panel.active { display: block; }
        button { padding: 8px 16px; margin: 5px; cursor: pointer; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
        th { background-color: #4CAF50; color: white; cursor: pointer; }
        tr:nth-child(even) { background: #f9f9f9; }
        input[type=text] { padding: 5px; margin: 5px; width: 200px; }
        .status { margin-top: 20px; background: #e7f3fe; padding: 10px; border-left: 6px solid #2196F3; }
    </style>
</head>
<body>
    <h1>ESP32 Network Scanner</h1>
    <div class="tabs">
        <div class="tab active" onclick="openTab('wifi')">WiFi</div>
        <div class="tab" onclick="openTab('bt')">Bluetooth</div>
        <div class="tab" onclick="openTab('ble')">BLE</div>
    </div>
    <div id="wifi-panel" class="panel active">
        <button onclick="startScan('wifi')">Scan WiFi</button>
        <input type="text" id="wifi-filter" placeholder="Filter SSID" onkeyup="filterTable('wifi')">
        <table id="wifi-table"><thead><tr><th onclick="sortTable('wifi',0)">SSID</th><th>BSSID</th><th onclick="sortTable('wifi',2)">RSSI</th><th>Encryption</th></tr></thead><tbody></tbody></table>
    </div>
    <div id="bt-panel" class="panel">
        <button onclick="startScan('bt')">Scan BT</button>
        <input type="text" id="bt-filter" placeholder="Filter Name" onkeyup="filterTable('bt')">
        <table id="bt-table"><thead><tr><th onclick="sortTable('bt',0)">Name</th><th>Address</th><th onclick="sortTable('bt',2)">RSSI</th></tr></thead><tbody></tbody></table>
    </div>
    <div id="ble-panel" class="panel">
        <button onclick="startScan('ble')">Scan BLE</button>
        <input type="text" id="ble-filter" placeholder="Filter Name" onkeyup="filterTable('ble')">
        <table id="ble-table"><thead><tr><th onclick="sortTable('ble',0)">Name</th><th>Address</th><th onclick="sortTable('ble',2)">RSSI</th><th>TX Power</th></tr></thead><tbody></tbody></table>
    </div>
    <div class="status" id="connection-status">WiFi: Not connected</div>
    <script>
        function openTab(type) {
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
            document.querySelector(`div[onclick="openTab('${type}')"]`).classList.add('active');
            document.getElementById(`${type}-panel`).classList.add('active');
        }
        function startScan(type) {
            fetch(`/api/scan?type=${type}`).then(r => r.json()).then(data => {
                setTimeout(() => fetchResults(type), 2000);
            });
        }
        function fetchResults(type) {
            fetch(`/api/${type}`).then(r => r.json()).then(data => {
                const tbody = document.getElementById(`${type}-table`).getElementsByTagName('tbody')[0];
                tbody.innerHTML = '';
                data.forEach((item, idx) => {
                    let row = tbody.insertRow();
                    if (type === 'wifi') {
                        row.insertCell(0).innerText = item.ssid;
                        row.insertCell(1).innerText = item.bssid;
                        row.insertCell(2).innerText = item.rssi;
                        row.insertCell(3).innerText = item.encryption;
                    } else if (type === 'bt') {
                        row.insertCell(0).innerText = item.name;
                        row.insertCell(1).innerText = item.address;
                        row.insertCell(2).innerText = item.rssi;
                    } else if (type === 'ble') {
                        row.insertCell(0).innerText = item.name;
                        row.insertCell(1).innerText = item.address;
                        row.insertCell(2).innerText = item.rssi;
                        row.insertCell(3).innerText = item.txPower;
                    }
                });
                applyFilter(type);
            });
        }
        function filterTable(type) {
            const input = document.getElementById(`${type}-filter`);
            const filter = input.value.toUpperCase();
            const rows = document.getElementById(`${type}-table`).getElementsByTagName('tr');
            for (let i = 1; i < rows.length; i++) {
                let visible = false;
                for (let cell of rows[i].cells) {
                    if (cell.innerText.toUpperCase().indexOf(filter) > -1) { visible = true; break; }
                }
                rows[i].style.display = visible ? '' : 'none';
            }
        }
        function sortTable(type, col) {
            const table = document.getElementById(`${type}-table`);
            let rows, switching, i, x, y, shouldSwitch, dir, switchcount = 0;
            switching = true;
            dir = "asc";
            while (switching) {
                switching = false;
                rows = table.rows;
                for (i = 1; i < (rows.length - 1); i++) {
                    shouldSwitch = false;
                    x = rows[i].getElementsByTagName("TD")[col];
                    y = rows[i + 1].getElementsByTagName("TD")[col];
                    if (dir == "asc") {
                        if (isNaN(parseFloat(x.innerText)) ? x.innerText.toLowerCase() > y.innerText.toLowerCase() : parseFloat(x.innerText) > parseFloat(y.innerText)) {
                            shouldSwitch = true; break;
                        }
                    } else if (dir == "desc") {
                        if (isNaN(parseFloat(x.innerText)) ? x.innerText.toLowerCase() < y.innerText.toLowerCase() : parseFloat(x.innerText) < parseFloat(y.innerText)) {
                            shouldSwitch = true; break;
                        }
                    }
                }
                if (shouldSwitch) {
                    rows[i].parentNode.insertBefore(rows[i + 1], rows[i]);
                    switching = true;
                    switchcount++;
                } else {
                    if (switchcount == 0 && dir == "asc") {
                        dir = "desc"; switching = true;
                    }
                }
            }
        }
        function applyFilter(type) { filterTable(type); } // initial call

        // Update connection status
        setInterval(() => {
            fetch('/api/status').then(r => r.json()).then(s => {
                document.getElementById('connection-status').innerText = s.connected ? `WiFi connected: IP ${s.ip}, GW ${s.gw}, DNS ${s.dns}` : 'WiFi: Not connected';
            });
        }, 5000);

        window.onload = () => {
            ['wifi','bt','ble'].forEach(t => fetchResults(t));
        };
    </script>
</body>
</html>
)rawliteral";
}

void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html", generateHTML());
    });

    // NOTE: this handler blocks the AsyncTCP task for the duration of the
    // scan (up to ~10s for BT). That's tolerable for a small personal tool,
    // but if you see other requests stalling while a scan runs, consider
    // moving the scan into a FreeRTOS task and returning immediately here.
    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        if (scanInProgress) { request->send(200, "application/json", "{\"status\":\"busy\"}"); return; }
        String type;
        if (request->hasParam("type")) type = request->getParam("type")->value();
        else { request->send(400); return; }
        scanInProgress = true;
        if (type == "wifi") performWiFiScan();
        else if (type == "bt") performBTScan();
        else if (type == "ble") performBLEScan();
        scanInProgress = false;
        request->send(200, "application/json", "{\"status\":\"done\"}");
    });

    auto handleApi = [](AsyncWebServerRequest *request, const String& type) {
        String json = "[";
        if (type == "wifi") {
            for (size_t i=0; i<wifiNetworks.size(); i++) {
                if (i) json += ",";
                json += "{\"ssid\":\"" + jsonEscape(wifiNetworks[i].ssid) + "\",\"bssid\":\"" + wifiNetworks[i].bssid + "\",\"rssi\":" + String(wifiNetworks[i].rssi) + ",\"encryption\":\"" + wifiNetworks[i].encryption + "\"}";
            }
        } else if (type == "bt") {
            for (size_t i=0; i<btDevices.size(); i++) {
                if (i) json += ",";
                json += "{\"name\":\"" + jsonEscape(btDevices[i].name) + "\",\"address\":\"" + btDevices[i].address + "\",\"rssi\":" + String(btDevices[i].rssi) + "}";
            }
        } else if (type == "ble") {
            for (size_t i=0; i<bleDevices.size(); i++) {
                if (i) json += ",";
                json += "{\"name\":\"" + jsonEscape(bleDevices[i].name) + "\",\"address\":\"" + bleDevices[i].address + "\",\"rssi\":" + String(bleDevices[i].rssi) + ",\"txPower\":" + String(bleDevices[i].txPower) + "}";
            }
        }
        json += "]";
        request->send(200, "application/json", json);
    };

    server.on("/api/wifi", HTTP_GET, [handleApi](AsyncWebServerRequest *r){ handleApi(r, "wifi"); });
    server.on("/api/bt", HTTP_GET, [handleApi](AsyncWebServerRequest *r){ handleApi(r, "bt"); });
    server.on("/api/ble", HTTP_GET, [handleApi](AsyncWebServerRequest *r){ handleApi(r, "ble"); });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = staConnected ? 
            "{\"connected\":true,\"ssid\":\"" + staSSID + "\",\"ip\":\"" + staIP + "\",\"gw\":\"" + staGW + "\",\"dns\":\"" + staDNS + "\"}" :
            "{\"connected\":false}";
        request->send(200, "application/json", json);
    });

    server.begin();
}

// ---------- Command parsing ----------
void processCommand(String cmdLine) {
    cmdLine.trim();
    if (cmdLine.length() == 0) return;
    String parts[5];
    int p = 0, start = 0;
    for (int i=0; i<=cmdLine.length(); i++) {
        if (cmdLine[i] == ' ' || i == cmdLine.length()) {
            if (i > start) {
                parts[p++] = cmdLine.substring(start, i);
                if (p >= 5) break;
            }
            start = i+1;
        }
    }
    if (p == 0) return;

    String cmd = parts[0];
    cmd.toLowerCase();

    if (cmd == "help") {
        Serial.println("Commands:\n"
                       "  wifi [sort rssi|ssid] [filter open|ssid TEXT] [search TEXT]\n"
                       "  bt [sort name|rssi] [filter addr TEXT]\n"
                       "  ble [sort rssi|name] [filter name TEXT]\n"
                       "  connect <ssid> [password]\n"
                       "  disconnect\n"
                       "  status\n"
                       "  ping\n"
                       "  save <wifi|bt|ble|all> <csv|json>\n"
                       "  interval <seconds>\n"
                       "  scan start|stop\n");
    }
    else if (cmd == "wifi") {
        // optional subcommands
        bool printed = false;
        for (int i=1; i<p; i++) {
            if (parts[i] == "sort" && i+1 < p) {
                if (parts[i+1] == "rssi") sortWiFiByRSSI();
                else if (parts[i+1] == "ssid") sortWiFiBySSID();
                i++;
            } else if (parts[i] == "filter" && i+1 < p) {
                String filterType = parts[i+1];
                if (filterType == "open") {
                    Serial.println("--- Open WiFi networks ---");
                    for (auto& n : wifiNetworks) {
                        if (n.encryption == "Open")
                            Serial.printf("%-32s %4d dBm  %s\n", n.ssid.c_str(), n.rssi, n.encryption.c_str());
                    }
                    printed = true;
                    i++;
                } else if (filterType == "ssid" && i+2 < p) {
                    String key = parts[i+2];
                    key.toLowerCase();
                    Serial.printf("WiFi SSID contains \"%s\":\n", key.c_str());
                    for (auto& n : wifiNetworks) {
                        String lssid = n.ssid; lssid.toLowerCase();
                        if (lssid.indexOf(key) >= 0)
                            Serial.printf("%-32s %4d dBm  %s\n", n.ssid.c_str(), n.rssi, n.encryption.c_str());
                    }
                    printed = true;
                    i+=2;
                }
            } else if (parts[i] == "search" && i+1 < p) {
                String key = parts[i+1];
                key.toLowerCase();
                Serial.printf("Search WiFi SSID for \"%s\":\n", key.c_str());
                for (auto& n : wifiNetworks) {
                    String lssid = n.ssid; lssid.toLowerCase();
                    if (lssid.indexOf(key) >= 0)
                        Serial.printf("%-32s %4d dBm  %s\n", n.ssid.c_str(), n.rssi, n.encryption.c_str());
                }
                printed = true;
                i++;
            }
        }
        if (!printed) {
            performWiFiScan();
            printWiFiResults();
        }
    }
    else if (cmd == "bt") {
        bool printed = false;
        for (int i=1; i<p; i++) {
            if (parts[i] == "sort" && i+1 < p) {
                if (parts[i+1] == "name") sortBTByName();
                else if (parts[i+1] == "rssi") sortBTByRSSI();
                i++;
            } else if (parts[i] == "filter" && i+2 < p && parts[i+1] == "addr") {
                String key = parts[i+2];
                key.toLowerCase();
                Serial.printf("BT address contains \"%s\":\n", key.c_str());
                for (auto& d : btDevices) {
                    String addr = d.address; addr.toLowerCase();
                    if (addr.indexOf(key) >= 0)
                        Serial.printf("%-25s %17s %d\n", d.name.c_str(), d.address.c_str(), d.rssi);
                }
                printed = true; i+=2;
            }
        }
        if (!printed) {
            performBTScan();
            printBTResults();
        }
    }
    else if (cmd == "ble") {
        bool printed = false;
        for (int i=1; i<p; i++) {
            if (parts[i] == "sort" && i+1 < p) {
                if (parts[i+1] == "rssi") sortBLEByRSSI();
                else if (parts[i+1] == "name") sortBLEByName();
                i++;
            } else if (parts[i] == "filter" && i+2 < p && parts[i+1] == "name") {
                String key = parts[i+2];
                key.toLowerCase();
                Serial.printf("BLE name contains \"%s\":\n", key.c_str());
                for (auto& d : bleDevices) {
                    String name = d.name; name.toLowerCase();
                    if (name.indexOf(key) >= 0)
                        Serial.printf("%-25s %17s %4d  %d\n", d.name.c_str(), d.address.c_str(), d.rssi, d.txPower);
                }
                printed = true; i+=2;
            }
        }
        if (!printed) {
            performBLEScan();
            printBLEResults();
        }
    }
    else if (cmd == "connect" && p >= 2) {
        String ssid = parts[1];
        String pass = (p >= 3) ? parts[2] : "";
        connectWiFi(ssid, pass);
    }
    else if (cmd == "disconnect") disconnectWiFi();
    else if (cmd == "status") {
        if (staConnected) {
            Serial.printf("Connected to %s\nIP: %s, Gateway: %s, DNS: %s\n", staSSID.c_str(), staIP.c_str(), staGW.c_str(), staDNS.c_str());
        } else Serial.println("Not connected.");
    }
    else if (cmd == "ping") pingTest();
    else if (cmd == "save" && p >= 3) {
        String type = parts[1]; type.toLowerCase();
        String format = parts[2]; format.toLowerCase();
        if (type == "wifi" && format == "csv") exportWiFiCSV();
        else if (type == "bt" && format == "csv") exportBTCSV();
        else if (type == "ble" && format == "csv") exportBLECSV();
        else if (type == "all" && format == "json") exportAllJSON();
        else Serial.println("Invalid save format. Use: save <wifi|bt|ble|all> <csv|json>");
    }
    else if (cmd == "interval" && p >= 2) {
        periodicInterval = parts[1].toInt() * 1000;
        if (periodicInterval < 5000) periodicInterval = 5000;
        Serial.printf("Scan interval set to %lu seconds.\n", periodicInterval/1000);
    }
    else if (cmd == "scan" && p >= 2) {
        if (parts[1] == "start") {
            periodicScanEnabled = true;
            lastScanMillis = 0; // trigger immediately
            Serial.println("Periodic scanning started.");
        } else if (parts[1] == "stop") {
            periodicScanEnabled = false;
            Serial.println("Periodic scanning stopped.");
        }
    }
    else {
        Serial.println("Unknown command. Type 'help'.");
    }
}

// ---------- Setup ----------
void setup() {
    Serial.begin(115200);
    delay(500);
    // Start AP for web interface
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.println("AP started: " + String(AP_SSID) + " / " + AP_PASSWORD);
    setupWebServer();
    Serial.println("Web server running at http://192.168.4.1");
    Serial.println("Ready. Type 'help' for commands.");
}

// ---------- Loop ----------
void loop() {
    // Process serial commands
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        processCommand(line);
    }

    // Periodic scanning
    if (periodicScanEnabled && !scanInProgress) {
        if (millis() - lastScanMillis >= periodicInterval) {
            fullScan();
            lastScanMillis = millis();
        }
    }

    delay(10);
}
