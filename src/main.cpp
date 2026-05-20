#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

Preferences preferences;

// --- KONFIGURASI SENSOR ---
#define MAX_SENSORS 8  // Ubah ke 16 jika mau 16 sensor

const char* tireLabels[] = {
    "Front Left", "Front Left-2", 
    "Front Right", "Front Right-2", 
    "Rear Left", "Rear Left-2",
    "Rear Right", "Rear Right-2",
    "Front Left-3", "Front Right-3", "Rear Left-3", "Rear Right-3" 
    "Front Left-4", "Front Right-4", "Rear Left-4", "Rear Right-4" 
};
std::string pairedMacs[16] = {"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};

// --- DATA SENSOR ---
struct TireData {
    float voltage;
    int   celsius;
    float kpa;
    float psi;
    String statusStr;
    int   rssi;
    bool  hasData;
    unsigned long lastSeen; 
};
TireData tireData[16] = {};

unsigned long lastPrintTime = 0;
#define PRINT_INTERVAL_MS 10000  // 10 detik
unsigned long lastClearTime = 0;
unsigned long lastHeapLog = 0;

// --- SCAN TPMS ---
struct ScannedDevice {
    std::string mac;
    int rssi;
};
std::vector<ScannedDevice> scannedTpms;
bool scanTpmsMode = false;
bool scanTpmsDone = false;

void onScanDone(NimBLEScanResults results) {
    scanTpmsMode = false;
    scanTpmsDone = true;
}

int findTireIndex(const std::string& addr) {
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (!pairedMacs[i].empty() && pairedMacs[i] == addr) return i;
    }
    return -1;
}

bool isAlreadyPaired(const std::string& addr) {
    return findTireIndex(addr) != -1;
}

// ============================================================
// FLASH — Simpan dan Load pairing ke memori permanen
// ============================================================
void saveMacToFlash(int index, const String& mac) {
    preferences.begin("tpms", false);
    String key = "mac" + String(index);
    preferences.putString(key.c_str(), mac);
    preferences.end();
}

void loadMacsFromFlash() {
    preferences.begin("tpms", true);
    for (int i = 0; i < MAX_SENSORS; i++) {
        String key = "mac" + String(i);
        String saved = preferences.getString(key.c_str(), "");
        if (saved.length() > 0) {
            pairedMacs[i] = std::string(saved.c_str());
        }
    }
    preferences.end();
}

// ============================================================
// PRINT — Tabel dashboard semua sensor
// ============================================================
void printAllTires() {
    bool adaPairing = false;
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (!pairedMacs[i].empty()) { adaPairing = true; break; }
    }
    if (!adaPairing) return;

    unsigned long detik = millis() / 1000;
    unsigned int jam   = detik / 3600;
    unsigned int menit = (detik % 3600) / 60;
    unsigned int dtk   = detik % 60;

    Serial.println();
    Serial.println("========================================================================================================");
    Serial.printf("TPMS DASHBOARD (%d sensors) | Uptime %02d:%02d:%02d | Free Heap: %d bytes\n", MAX_SENSORS, jam, menit, dtk, ESP.getFreeHeap());
    Serial.println("========================================================================================================");
    Serial.printf("| %-16s | %-6s | %-4s | %-4s | %-5s | %-5s | %-4s | %-11s |\n",
                  "Tire", "Status", "Volt", "Temp", "KPa", "PSI", "RSSI", "Last Update");
    Serial.println("========================================================================================================");

    for (int i = 0; i < MAX_SENSORS; i++) {
        if (pairedMacs[i].empty()) continue;

        if (!tireData[i].hasData) {
            Serial.printf("| %-16s | %-6s | %-4s | %-4s | %-5s | %-5s | %-4s | %-11s |\n",
                          tireLabels[i], "NO DATA", "-", "-", "-", "-", "-", "-");
        } else {
            unsigned long elapsed = (millis() - tireData[i].lastSeen) / 1000;
            Serial.printf("| %-16s | %-6s | %.2f | %4d | %5.1f | %5.1f | %4d | %-11s |\n",
                          tireLabels[i],
                          tireData[i].statusStr.c_str(),
                          tireData[i].voltage,
                          tireData[i].celsius,
                          tireData[i].kpa,
                          tireData[i].psi,
                          tireData[i].rssi,
                          (String(elapsed) + "s").c_str());
        }
    }
    Serial.println("========================================================================================================");
    Serial.println();
}

// ============================================================
// BLE CALLBACK
// ============================================================
class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        if (!advertisedDevice->isAdvertisingService(NimBLEUUID((uint16_t)0xA827))) return;

        std::string addr = advertisedDevice->getAddress().toString();

        // --- MODE SCAN TPMS ---
        if (scanTpmsMode) {
            if (isAlreadyPaired(addr)) return;
            for (auto& d : scannedTpms) {
                if (d.mac == addr) return;
            }
            scannedTpms.push_back({addr, advertisedDevice->getRSSI()});
            Serial.printf("[SCAN] %s  RSSI: %d dBm\n", addr.c_str(), advertisedDevice->getRSSI());
            return;
        }

        // --- MODE MONITORING —
        int idx = findTireIndex(addr);
        if (idx == -1) return;

        std::string mfr = advertisedDevice->getManufacturerData();
        if (mfr.length() < 11) return;

        uint8_t* data = (uint8_t*)mfr.data();

        uint16_t status = ((uint16_t)data[0] << 8) | data[1];
        tireData[idx].statusStr = (status == 0x0600) ? "ALARM" : "Normal";
        tireData[idx].voltage   = data[2] / 60.0f;
        tireData[idx].celsius   = data[3] - 50;
        
        uint16_t kpaAbsolut = ((uint16_t)data[4] << 8) | data[5];
        if (kpaAbsolut < 100) {
            tireData[idx].kpa = 0.0f;
            tireData[idx].psi = 0.0f;
        } else {
            tireData[idx].kpa = kpaAbsolut - 101.3f;
            tireData[idx].psi = (kpaAbsolut - 101.3f) * 0.145f;
        }
        
        tireData[idx].rssi      = advertisedDevice->getRSSI();
        tireData[idx].hasData   = true;
        tireData[idx].lastSeen  = millis();
    }
};

// ============================================================
// SERIAL COMMAND HANDLER
// ============================================================
void handleSerial() {
    if (!Serial.available()) return;

    String input = Serial.readStringUntil('\n');
    input.trim();
    input.toLowerCase();

    if (input == "scan_tpms") {
        scannedTpms.clear();
        scanTpmsMode = true;
        scanTpmsDone = false;
        Serial.println("\n[INFO] Scanning TPMS 30 detik...");
        NimBLEDevice::getScan()->stop();
        NimBLEDevice::getScan()->start(30, onScanDone, false);
        return;
    }

    if (input.startsWith("p-")) {
        int spaceIdx = input.indexOf(' ');
        if (spaceIdx == -1) {
            Serial.println("[ERROR] Format: p-fl <MAC>");
            return;
        }
        String pos = input.substring(2, spaceIdx);
        String mac = input.substring(spaceIdx + 1);
        mac.trim();

        if (mac.length() != 17) {
            Serial.println("[ERROR] Format MAC tidak valid. Contoh: b9:41:fa:00:04:d0");
            return;
        }

        int idx = -1;
        if      (pos == "fl")   idx = 0;
        else if (pos == "fl-2") idx = 1;
        else if (pos == "fl-3") idx = 2;
        else if (pos == "fr")   idx = 3;
        else if (pos == "fr-2") idx = 4;
        else if (pos == "fr-3") idx = 5;
        else if (pos == "rl")   idx = 6;
        else if (pos == "rl-2") idx = 7;
        else if (pos == "rl-3") idx = 8;
        else if (pos == "rr")   idx = 9;
        else if (pos == "rr-2") idx = 10;
        else if (pos == "rr-3") idx = 11;

        if (idx == -1) {
            Serial.println("[ERROR] Posisi tidak valid. Gunakan: fl/fl-2/fl-3/fr/fr-2/fr-3/rl/rl-2/rl-3/rr/rr-2/rr-3");
            return;
        }

        if (!pairedMacs[idx].empty()) {
            Serial.printf("[ERROR] %s sudah dipasangkan ke: %s. Ketik 'unpair-%s' dulu.\n",
                tireLabels[idx], pairedMacs[idx].c_str(), pos.c_str());
            return;
        }

        pairedMacs[idx] = std::string(mac.c_str());
        saveMacToFlash(idx, mac);
        Serial.printf("[OK] %s dipasangkan ke: %s\n", tireLabels[idx], pairedMacs[idx].c_str());
        Serial.println("[OK] Pairing tersimpan — tidak hilang saat restart.");
        return;
    }

    if (input.startsWith("unpair-")) {
        String pos = input.substring(7);
        int idx = -1;
        if      (pos == "fl")   idx = 0;
        else if (pos == "fl-2") idx = 1;
        else if (pos == "fl-3") idx = 2;
        else if (pos == "fr")   idx = 3;
        else if (pos == "fr-2") idx = 4;
        else if (pos == "fr-3") idx = 5;
        else if (pos == "rl")   idx = 6;
        else if (pos == "rl-2") idx = 7;
        else if (pos == "rl-3") idx = 8;
        else if (pos == "rr")   idx = 9;
        else if (pos == "rr-2") idx = 10;
        else if (pos == "rr-3") idx = 11;

        if (idx == -1) {
            Serial.println("[ERROR] Posisi tidak valid. Gunakan: fl/fl-2/fl-3/fr/fr-2/fr-3/rl/rl-2/rl-3/rr/rr-2/rr-3");
            return;
        }
        if (pairedMacs[idx].empty()) {
            Serial.printf("[ERROR] %s belum dipasangkan.\n", tireLabels[idx]);
            return;
        }
        pairedMacs[idx] = "";
        saveMacToFlash(idx, "");
        tireData[idx].hasData = false;
        Serial.printf("[OK] %s berhasil di-unpair.\n", tireLabels[idx]);
        return;
    }

    if (input == "status") {
        Serial.println("\n[STATUS] Sensor terdaftar:");
        for (int i = 0; i < MAX_SENSORS; i++) {
            Serial.printf("  %s: %s\n", tireLabels[i],
                pairedMacs[i].empty() ? "(belum dipasang)" : pairedMacs[i].c_str());
        }
        return;
    }

    Serial.println("[ERROR] Perintah: scan_tpms | p-fl/fr/rl/rr <MAC> | unpair-fl/fr/rl/rr | status");
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    loadMacsFromFlash();

    Serial.println("Memulai Scanner TPMS...");
    Serial.println("1. Ketik 'scan_tpms' untuk mencari sensor.");
    Serial.println("2. Ketik 'p-fl/fr/rl/rr <MAC>' untuk pair sensor ke posisi ban.");
    Serial.println("3. Ketik 'status' untuk lihat sensor terdaftar.");
    Serial.printf("4. Data ditampilkan setiap %d detik.\n\n", PRINT_INTERVAL_MS / 1000);

    bool adaPairing = false;
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (!pairedMacs[i].empty()) adaPairing = true;
    }
    if (adaPairing) {
        Serial.println("[INFO] Pairing tersimpan ditemukan:");
        for (int i = 0; i < MAX_SENSORS; i++) {
            if (!pairedMacs[i].empty())
                Serial.printf("  %s: %s\n", tireLabels[i], pairedMacs[i].c_str());
        }
        Serial.println();
    }

    for (int i = 0; i < MAX_SENSORS; i++) tireData[i].hasData = false;

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
    pScan->setActiveScan(false);
    pScan->setInterval(50);  // lebih cepat untuk 12 sensor
    pScan->setWindow(50);
    pScan->setDuplicateFilter(false);
    pScan->start(0, nullptr, false);
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    handleSerial();

    unsigned long now = millis();

    if (now - lastClearTime >= 60000) {
        lastClearTime = now;
        NimBLEDevice::getScan()->clearResults();
        Serial.println("[INFO] Cache scan dibersihkan.");
    }
    
    // Monitor heap setiap 5 menit
    if (now - lastHeapLog >= 60000) {
        lastHeapLog = now;
        Serial.printf("[HEAP] Free: %d | Min ever: %d\n",
            ESP.getFreeHeap(),
            ESP.getMinFreeHeap());
    }

    if (now - lastPrintTime >= PRINT_INTERVAL_MS) {
        lastPrintTime = now;
        unsigned long t = millis();
        printAllTires();
        Serial.printf("[DEBUG] Print took %lums\n", millis() - t);
    }

    if (scanTpmsDone) {
        scanTpmsDone = false;
        if (scannedTpms.empty()) {
            Serial.println("[INFO] Tidak ada sensor TPMS baru ditemukan.");
        } else {
            Serial.printf("\n[HASIL] %d sensor TPMS ditemukan:\n", scannedTpms.size());
            for (auto& d : scannedTpms) {
                Serial.printf("  %s  RSSI: %d dBm\n", d.mac.c_str(), d.rssi);
            }
            Serial.println("Gunakan: p-fl/fr/rl/rr <MAC>");
        }
        NimBLEDevice::getScan()->clearResults();
        NimBLEDevice::getScan()->start(0, nullptr, false);
    }
}