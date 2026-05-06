#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

// --- CẤU HÌNH PIN & ĐỊA CHỈ (ESP32-C3) ---
#define SDA_PIN 6
#define SCL_PIN 7
#define MLX_ADDR 0x5A

// --- THÔNG SỐ HIỆU CHUẨN NHIỆT ĐỘ ---
const float BASE_OFFSET = 1.55; 
const float REF_AMBIENT = 30.0;
const float K_ADJUST_RATE = 0.008;

// --- BIẾN TOÀN CỤC & ESP-NOW ---
uint8_t masterAddress[] = {0x00, 0x4B, 0x12, 0x38, 0x5A, 0xA8}; 
typedef struct struct_message {
  char type[8];
  float temp;
  int bpm;
  int spo2;
} struct_message;
struct_message msg;

bool isMeasuring = false;
int currentChannel = 1;
bool isChannelLocked = false;

// --- BIẾN HỖ TRỢ ĐO ĐẠC ---
MAX30105 particleSensor;
const byte RATE_SIZE = 6; 
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
int beatAvg;

uint32_t irBuffer[100];
uint32_t redBuffer[100];
int32_t n_spo2;
int8_t validSPO2;
int32_t n_hr_unused;
int8_t v_hr_unused;

int measurementCount = 0;
int validMeasurements = 0;
long sumBPM = 0;
int sumSpO2 = 0;
float sumTemp = 0;
float currentTempBody = 0;
unsigned long lastPrintTime = 0;

// ===== HÀM HỖ TRỢ MLX90614 =====
bool read16(uint8_t reg, uint16_t &data) {
  Wire.beginTransmission(MLX_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MLX_ADDR, (uint8_t)3) == 3) {
    uint8_t low = Wire.read();
    uint8_t high = Wire.read();
    Wire.read(); // PEC
    data = (high << 8) | low;
    return true;
  }
  return false;
}

float rawToTemp(uint16_t raw) { return (raw * 0.02) - 273.15; }

float getAmbient() {
  uint16_t raw;
  if (read16(0x06, raw)) return rawToTemp(raw);
  return NAN;
}

float getSkinPeak() {
  float maxT = 0;
  for (int i = 0; i < 10; i++) {
    uint16_t raw;
    if (read16(0x07, raw)) {
      float t = rawToTemp(raw);
      if (t > maxT) maxT = t;
    }
    delay(10);
  }
  return (maxT > 30) ? maxT : NAN;
}

float estimateBodyTemp(float T_skin, float T_ambient) {
  float k_env = 0.15 + (T_ambient < REF_AMBIENT ? (REF_AMBIENT - T_ambient) * K_ADJUST_RATE : 0);
  float delta = max(0.0f, T_skin - T_ambient);
  return T_skin + (k_env * delta) + BASE_OFFSET;
}

// ===== ESP-NOW CALLBACKS =====
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) isChannelLocked = true;
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len > 0 && data[0] == 1) { 
    isMeasuring = true;
    measurementCount = 0;
    validMeasurements = 0;
    sumBPM = 0; sumSpO2 = 0; sumTemp = 0;
    currentTempBody = 0;
    rateSpot = 0;
    for (byte x = 0; x < RATE_SIZE; x++) rates[x] = 0;
    Serial.println("\n🚀 [Slave] NHAN LENH DO - BAT DAU...");
  }
}

void scanForMaster() {
  Serial.print("🔍 Tim Master");
  while (!isChannelLocked) {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, masterAddress, 6);
    peerInfo.channel = currentChannel;
    peerInfo.encrypt = false;
    if (esp_now_is_peer_exist(masterAddress)) esp_now_del_peer(masterAddress);
    esp_now_add_peer(&peerInfo);

    uint8_t ping = 0;
    esp_now_send(masterAddress, &ping, 1);
    delay(150); 
    if (isChannelLocked) {
      Serial.printf("\n✅ Thay Master tai kenh: %d\n", currentChannel);
      break;
    }
    currentChannel = (currentChannel % 13) + 1;
    Serial.print(".");
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) return;
  esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  scanForMaster();

  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  if (!particleSensor.begin(Wire, 100000)) {
    Serial.println("MAX30105 Fail!");
    while (1);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
}

// ===== LOOP (LOGIC CỦA BẠN) =====
void loop() {
  // 1. CHỜ LỆNH
  if (!isMeasuring) {
    particleSensor.check();
    return;
  }

  // 2. VÒNG LẶP ĐO ĐẠC
  long irValue = particleSensor.getIR();

  // Reset nếu không có tay
  if (irValue < 5000) {
    if (measurementCount > 0) {
      measurementCount = 0;
      validMeasurements = 0;
      sumBPM = 0; sumSpO2 = 0; sumTemp = 0;
      currentTempBody = 0;
      rateSpot = 0;
      Serial.println("[!] Da nhac tay - Reset. Vui long dat lai...");
    }
    return; 
  }

  // Phát hiện nhịp tim
  if (checkForBeat(irValue) == true) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    float bpm = 60 / (delta / 1000.0);

    if (bpm < 200 && bpm > 40) {
      rates[rateSpot++] = (byte)bpm;
      rateSpot %= RATE_SIZE;
      long sum = 0;
      for (byte x = 0; x < RATE_SIZE; x++) sum += rates[x];
      beatAvg = sum / RATE_SIZE;

      if (rateSpot == 0) { 
        measurementCount++;
        Serial.println("\n--- BAT DAU CHU KY DO MOI ---");
        
        // BƯỚC 1: ĐO NHIỆT ĐỘ
        float t_amb = getAmbient();
        float t_skin = getSkinPeak();
        
        if (!isnan(t_skin) && t_skin > (t_amb + 1.0)) {
          currentTempBody = estimateBodyTemp(t_skin, t_amb);
          Serial.print("[1] Temp Done: "); Serial.print(currentTempBody); Serial.println(" C");
        } else {
          currentTempBody = 0;
          Serial.println("[1] Temp Fail: Vui long de sat cam bien!");
        }

        // BƯỚC 2: ĐO SpO2
        Serial.println("[2] Dang do SpO2... Giu tinh tay");
        for (byte i = 0 ; i < 100 ; i++) {
          while (particleSensor.available() == false) particleSensor.check();
          redBuffer[i] = particleSensor.getRed();
          irBuffer[i] = particleSensor.getIR();
          particleSensor.nextSample();
        }
        maxim_heart_rate_and_oxygen_saturation(irBuffer, 100, redBuffer, &n_spo2, &validSPO2, &n_hr_unused, &v_hr_unused);

        // BƯỚC 3: TỔNG HỢP VÀ GỬI ĐI
        // Điều kiện: Ít nhất 2 chu kỳ nhịp tim (Warm-up), SpO2 và Temp hợp lệ
        if (measurementCount >= 2 && validSPO2 && n_spo2 > 90 && currentTempBody > 34.0) {
          validMeasurements++;
          sumBPM += beatAvg;
          sumSpO2 += n_spo2;
          sumTemp += currentTempBody;

          int finalBPM = sumBPM / validMeasurements;
          int finalSpO2 = sumSpO2 / validMeasurements;
          float finalTemp = sumTemp / validMeasurements;

          Serial.println("====================================");
          Serial.print("   KET QUA TB (Lan "); Serial.print(validMeasurements); Serial.println(")");
          Serial.print("   BPM: "); Serial.println(finalBPM);
          Serial.print("   SpO2: "); Serial.print(finalSpO2); Serial.println("%");
          Serial.print("   TEMP: "); Serial.print(finalTemp, 2); Serial.println(" C");
          Serial.println("====================================");

          strcpy(msg.type, "data");
          msg.temp = finalTemp;
          msg.bpm = finalBPM;
          msg.spo2 = finalSpO2;

          esp_now_send(masterAddress, (uint8_t*)&msg, sizeof(msg));
          Serial.println("📤 DA GUI KET QUA VE MASTER!");
          
          isMeasuring = false; // Kết thúc phiên đo
        } else {
          Serial.println("-> Dang on dinh (Warm-up)...");
        }
      }
    }
  }

  // Log nhanh
  if (millis() - lastPrintTime >= 500) {
    lastPrintTime = millis();
    Serial.printf("IR=%ld | BPM=%d | Temp=%.1fC\n", irValue, beatAvg, currentTempBody);
  }
}