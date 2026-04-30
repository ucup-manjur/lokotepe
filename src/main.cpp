#include <Arduino.h>
#include <NimBLEDevice.h>

// --- 4 slot sensor (FL, FR, RL, RR) ---
const char* tireLabels[] = {"Front Left", "Front Right", "Rear Left", "Rear Right"};
std::string pairedMacs[4] = {"", "", "", ""}; // kosong sampai di-pair

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

// Cari index ban dari MAC, return -1 kalau tidak ditemukan
int findTireIndex(const std::string& addr) {
    for (int i = 0; i < 4; i++) {
        if (!pairedMacs[i].empty() && pairedMacs[i] == addr) return i;
    }
    return -1;
}

// Cek apakah MAC sudah terdaftar di salah satu slot
bool isAlreadyPaired(const std::string& addr) {
    return findTireIndex(addr) != -1;
}

class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        // Filter hanya device dengan Service UUID 0xA827
        if (!advertisedDevice->isAdvertisingService(NimBLEUUID((uint16_t)0xA827))) return;

        std::string addr = advertisedDevice->getAddress().toString();

        // --- MODE SCAN TPMS ---
        if (scanTpmsMode) {
            if (isAlreadyPaired(addr)) return; // skip yang sudah terdaftar
            for (auto& d : scannedTpms) {
                if (d.mac == addr) return; // skip duplikat
            }
            scannedTpms.push_back({addr, advertisedDevice->getRSSI()});
            Serial.printf("[SCAN] %s  RSSI: %d dBm\n", addr.c_str(), advertisedDevice->getRSSI());
            return;
        }

        // --- MODE MONITORING ---
        int idx = findTireIndex(addr);
        if (idx == -1) return;

        std::string mfr = advertisedDevice->getManufacturerData();
        if (mfr.length() < 11) return;

        uint8_t* data = (uint8_t*)mfr.data();

        uint16_t status = ((uint16_t)data[0] << 8) | data[1];
        String statusStr = (status == 0x0600) ? "PERINGATAN!" : "Normal";

        float voltage  = (data[2] / 60.0f);
        int   celcius  = data[3] - 50;

        uint16_t kpaAbsolut = ((uint16_t)data[4] << 8) | data[5];
        float kpa = (kpaAbsolut - 101.3f);
        float psi = ((kpaAbsolut - 101.3f) * 0.145f) + 0.2f;

        Serial.printf("\n=== [%s] ===\n", tireLabels[idx]);
        Serial.printf("Status  : %s\n", statusStr.c_str());
        Serial.printf("Voltage : %.2f V\n", voltage);
        if (voltage < 2.5f) Serial.println("Warning: Ganti Baterai Sensor!");
        Serial.printf("Suhu    : %d °C\n", celcius);
        Serial.printf("Tekanan : %.1f KPa\n", kpa);
        Serial.printf("Tekanan : %.1f PSI\n", psi);
        Serial.printf("RSSI    : %d dBm\n", advertisedDevice->getRSSI());
        Serial.println("----------------------------");
    }
};

void handleSerial() {
    if (!Serial.available()) return;

    String input = Serial.readStringUntil('\n');
    input.trim();
    input.toLowerCase();

    // --- scan_tpms ---
    if (input == "scan_tpms") {
        scannedTpms.clear();
        scanTpmsMode = true;
        scanTpmsDone = false;
        Serial.println("\n[INFO] Scanning TPMS 30 detik...");
        NimBLEDevice::getScan()->stop();
        NimBLEDevice::getScan()->start(30, onScanDone, false);
        return;
    }

    // --- p-fl/fr/rl/rr <MAC> ---
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
        else if (pos == "fr") idx = 1;
        else if (pos == "rl") idx = 2;
        else if (pos == "rr") idx = 3;

        if (idx == -1) {
            Serial.println("[ERROR] Posisi tidak valid. Gunakan: fl/fr/rl/rr");
            return;
        }

        if (!pairedMacs[idx].empty()) {
            Serial.printf("[ERROR] %s sudah dipasangkan ke: %s. Ketik 'unpair-%s' dulu.\n",
                tireLabels[idx], pairedMacs[idx].c_str(), pos.c_str());
            return;
        }

        pairedMacs[idx] = std::string(mac.c_str());
        Serial.printf("[OK] %s dipasangkan ke: %s\n", tireLabels[idx], pairedMacs[idx].c_str());
        return;
    }

    // --- unpair-fl/fr/rl/rr ---
    if (input.startsWith("unpair-")) {
        String pos = input.substring(7);
        int idx = -1;
        if      (pos == "fl") idx = 0;
        else if (pos == "fr") idx = 1;
        else if (pos == "rl") idx = 2;
        else if (pos == "rr") idx = 3;

        if (idx == -1) {
            Serial.println("[ERROR] Posisi tidak valid. Gunakan: fl/fr/rl/rr");
            return;
        }
        if (pairedMacs[idx].empty()) {
            Serial.printf("[ERROR] %s belum dipasangkan.\n", tireLabels[idx]);
            return;
        }
        pairedMacs[idx] = "";
        Serial.printf("[OK] %s berhasil di-unpair.\n", tireLabels[idx]);
        return;
    }

    // --- status ---
    if (input == "status") {
        Serial.println("\n[STATUS] Sensor terdaftar:");
        for (int i = 0; i < 4; i++) {
            Serial.printf("  %s: %s\n", tireLabels[i],
                pairedMacs[i].empty() ? "(belum dipasang)" : pairedMacs[i].c_str());
        }
        return;
    }

    Serial.println("[ERROR] Perintah: scan_tpms | p-fl/fr/rl/rr <MAC> | status");
}

void setup() {
    Serial.begin(115200);
    Serial.println("Memulai Scanner TPMS...");
    Serial.println("1. Ketik 'scan_tpms' untuk mencari sensor.");
    Serial.println("2. Ketik 'p-fl/fr/rl/rr <MAC>' untuk pair sensor ke posisi ban.");
    Serial.println("3. Ketik 'status' untuk lihat sensor terdaftar.\n");

    NimBLEDevice::init("");
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->setDuplicateFilter(false);
    pScan->start(0, nullptr, false);
}

void loop() {
    handleSerial();

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
    }
}
