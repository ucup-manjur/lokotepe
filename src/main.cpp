#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

Preferences preferences;

// --- 8 slot sensor (FL, FL-2, FR, FR-2, RL, RL-2, RR, RR-2) ---
const char* tireLabels[] = {"Front Left", "Front Left-2", "Front Right", "Front Right-2", "Rear Left", "Rear Left-2", "Rear Right", "Rear Right-2"};
std::string pairedMacs[8] = {"", "", "", "", "", "", "", ""};

// --- DATA SENSOR — disimpan sementara, dicetak tiap 10 detik ---
struct TireData {
    float voltage;
    int   celsius;
    float kpa;
    float psi;
    String statusStr;
    int   rssi;
    bool  hasData;
    unsigned long lastSeen;  // ← tambah ini

};
TireData tireData[8] = {};

unsigned long lastPrintTime = 0;
#define PRINT_INTERVAL_MS 10000  // 10 detik

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
    for (int i = 0; i < 8; i++) {
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
void saveMacToFlash(int index, String mac) {
    preferences.begin("tpms", false);
    preferences.putString(("mac" + String(index)).c_str(), mac);
    preferences.end();
}

void loadMacsFromFlash() {
    preferences.begin("tpms", true);
    for (int i = 0; i < 8; i++) {
        String saved = preferences.getString(("mac" + String(i)).c_str(), "");
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
    for (int i = 0; i < 8; i++) {
        if (!pairedMacs[i].empty()) { adaPairing = true; break; }
    }
    if (!adaPairing) return;

    unsigned long detik = millis() / 1000;
    unsigned int  jam   = detik / 3600;
    unsigned int  menit = (detik % 3600) / 60;
    unsigned int  dtk   = detik % 60;

    Serial.println("\n+==============+========+========+========+========+===========+");
    Serial.printf( "|  TPMS Dashboard          Uptime: %02d:%02d:%02d              |\n", jam, menit, dtk);
    Serial.println("+==============+========+========+========+========+===========+");
    Serial.println("|  Posisi      |  Volt  |  Suhu  |  PSI   |  RSSI  |  Update   |");
    Serial.println("+==============+========+========+========+========+===========+");

    for (int i = 0; i < 8; i++) {
        if (pairedMacs[i].empty()) continue;

        if (!tireData[i].hasData) {
            Serial.printf("|  %-12s|  ---   |  ---   |  ---   |  ---   |  waiting  |\n", tireLabels[i]);
        } else {
            unsigned long elapsed = (millis() - tireData[i].lastSeen) / 1000;
            bool isSleep = elapsed >= 60;

            if (isSleep) {
                Serial.printf("|  %-12s|  ---   |  ---   |  ---   |  ---   |  SLEEP    |\n", tireLabels[i]);
            } else {
                char volt[8], suhu[8], psi[8], rssi[8], upd[10];
                snprintf(volt, sizeof(volt), "%.1fV",  tireData[i].voltage);
                snprintf(suhu, sizeof(suhu), "%dC",    tireData[i].celsius);
                snprintf(psi,  sizeof(psi),  "%.1f",   tireData[i].psi);
                snprintf(rssi, sizeof(rssi), "%d",     tireData[i].rssi);
                snprintf(upd,  sizeof(upd),  "%lus",   elapsed);
                Serial.printf("|  %-12s|  %-6s|  %-6s|  %-6s|  %-6s|  %-9s|%s\n",
                    tireLabels[i], volt, suhu, psi, rssi, upd,
                    tireData[i].voltage < 2.5f ? " !BAT" : "");
            }
        }
        Serial.println("+==============+========+========+========+========+===========+");
    }
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

        // --- MODE MONITORING — simpan data, jangan langsung print ---
        int idx = findTireIndex(addr);
        if (idx == -1) return;

        std::string mfr = advertisedDevice->getManufacturerData();
        if (mfr.length() < 11) return;

        uint8_t* data = (uint8_t*)mfr.data();

        uint16_t status         = ((uint16_t)data[0] << 8) | data[1];
        tireData[idx].statusStr = (status == 0x0600) ? "PERINGATAN!" : "Normal";
        tireData[idx].voltage   = data[2] / 60.0f;
        tireData[idx].celsius   = data[3] - 50;
        uint16_t kpaAbsolut     = ((uint16_t)data[4] << 8) | data[5];
        // validasi
        if (kpaAbsolut < 100) {
            tireData[idx].kpa = 0.0f;
            tireData[idx].psi = 0.0f;
        } else {
            tireData[idx].kpa = kpaAbsolut - 101.3f;
            tireData[idx].psi = (kpaAbsolut - 101.3f) * 0.145f;
        }
        tireData[idx].rssi      = advertisedDevice->getRSSI();
        tireData[idx].hasData   = true;
        tireData[idx].lastSeen = millis();  // ← tambah ini
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
        if      (pos == "fl") idx = 0;
        else if (pos == "fl-2") idx = 1;
        else if (pos == "fr") idx = 2;
        else if (pos == "fr-2") idx = 3;
        else if (pos == "rl") idx = 4;
        else if (pos == "rl-2") idx = 5;
        else if (pos == "rr") idx = 6;
        else if (pos == "rr-2") idx = 7;

        if (idx == -1) {
            Serial.println("[ERROR] Posisi tidak valid. Gunakan: fl/fl-2/fr/fr-2/rl/rl-2/rr/rr-2");
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
        if      (pos == "fl") idx = 0;
        else if (pos == "fl-2") idx = 1;
        else if (pos == "fr") idx = 2;
        else if (pos == "fr-2") idx = 3;
        else if (pos == "rl") idx = 4;
        else if (pos == "rl-2") idx = 5;
        else if (pos == "rr") idx = 6;
        else if (pos == "rr-2") idx = 7;

        if (idx == -1) {
            Serial.println("[ERROR] Posisi tidak valid. Gunakan: fl/fl-2/fr/fr-2/rl/rl-2/rr/rr-2");
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
        for (int i = 0; i < 8; i++) {
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
    for (int i = 0; i < 8; i++) {
        if (!pairedMacs[i].empty()) adaPairing = true;
    }
    if (adaPairing) {
        Serial.println("[INFO] Pairing tersimpan ditemukan:");
        for (int i = 0; i < 8; i++) {
            if (!pairedMacs[i].empty())
                Serial.printf("  %s: %s\n", tireLabels[i], pairedMacs[i].c_str());
        }
        Serial.println();
    }

    for (int i = 0; i < 8; i++) tireData[i].hasData = false;

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
    pScan->setActiveScan(true);
    pScan->setInterval(50);
    pScan->setWindow(50);
    pScan->setDuplicateFilter(false);
    pScan->start(0, nullptr, false);
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    handleSerial();

    // Cetak semua data tiap 10 detik
    unsigned long now = millis();
    if (now - lastPrintTime >= PRINT_INTERVAL_MS) {
        lastPrintTime = now;
        printAllTires();
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
        NimBLEDevice::getScan()->start(0, nullptr, false);
        NimBLEDevice::getScan()->clearResults();

    }
}