#include <Arduino.h>

#include <NimBLEDevice.h>

// Ganti dengan MAC Address sensor kamu (lihat di nRF Connect)
// Contoh: "B9:41:FA:00:04:D0"
std::string targetDeviceAddress = "b9:41:fa:00:04:d0"; 

class MyAdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        // Cek apakah MAC Address cocok
        if (advertisedDevice->getAddress().toString() == targetDeviceAddress) {
            
            // Ambil data Manufacturer
            std::string strManufacturerData = advertisedDevice->getManufacturerData();
            
            if (strManufacturerData.length() >= 11) {
                uint8_t* data = (uint8_t*)strManufacturerData.data();

                // 1. Ambil Status (Byte 0-1)
                uint16_t status = (data[0] << 8) | data[1];
                String statusStr = (status == 0x0600) ? "PERINGATAN!" : "Normal";


                uint16_t volt = data[2];
                float voltage = (volt / 60.0f);

                // 2. Dekode Suhu (Byte 3)
                // Rumus: Nilai Desimal - 50
                int celcius = data[3] - 50;

                // 3. Dekode Tekanan (Byte 4-5)
                // Gabungin dua byte jadi satu angka desimal (KPa Absolut)
                uint16_t kpaAbsolut = (data[4] << 8) | data[5];
                
                // Rumus: (KPa Absolut - Tekanan Atmosfer) * Konversi PSI
                float kpa = (kpaAbsolut - 101.3);
                float psi = ((kpaAbsolut - 101.3) * 0.145 ) + 0.2;

                // 4. Print ke Serial Monitor
                Serial.println("--- DATA TPMS TERDETEKSI ---");
                Serial.printf("Raw bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10]);
                Serial.printf("Status  : %s\n", statusStr.c_str());
                Serial.printf("Voltage : %.2f\n", voltage); if (voltage < 2.5) {Serial.println("Warning: Ganti Baterai Sensor!");}
                Serial.printf("Suhu    : %d °C\n", celcius);
                Serial.printf("Tekanan : %.1f KPa\n", kpa);
                Serial.printf("Tekanan : %.1f PSI\n", psi);
                Serial.printf("RSSI    : %d dBm\n", advertisedDevice->getRSSI());
                Serial.println("----------------------------\n");
            }
        }
    }
};

void setup() {
    Serial.begin(115200);
    Serial.println("Memulai Scanner TPMS...");

    NimBLEDevice::init("");
    NimBLEScan* pNimBLEScan = NimBLEDevice::getScan();
    
    pNimBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
    pNimBLEScan->setActiveScan(true); // Biar dapet data lebih akurat
    pNimBLEScan->setInterval(100);
    pNimBLEScan->setWindow(99);
}

void loop() {
    // Scan selama 5 detik, lalu ulangi
    NimBLEDevice::getScan()->start(5, false);
}