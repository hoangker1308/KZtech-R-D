//khi đo nhiệt độ cần để cách ~ 2 cm, đối với cảm biến đo nhịp tym thì dùng lực vừa phải. 

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

// --- CẤU HÌNH PIN & ĐỊA CHỈ ---
#define SDA_PIN 6
#define SCL_PIN 7
#define MLX_ADDR 0x5A

// --- THÔNG SỐ HIỆU CHUẨN 
const float BASE_OFFSET = 1.55; 
const float REF_AMBIENT = 30.0;
const float K_ADJUST_RATE = 0.008;

// --- ESP-NOW CONFIG ---
uint8_t masterAddress[] = {0x00, 0x4B, 0x12, 0x38, 0x5A, 0xA8};
typedef struct struct_message {
  char type[8];
  float temp;
  int bpm;
  int spo2;
} struct_message;
struct_message msg;

bool isMeasuring = false;

// --- ĐỐI TƯỢNG VÀ BIẾN ---
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
float lastTempSent = 0; // Lưu để so sánh độ ổn định

unsigned long lastPrintTime = 0;
float currentTempBody = 0;

// ===== HÀM HỖ TRỢ MLX90614 (Tối ưu lọc nhiễu) =====
bool read16(uint8_t reg, uint16_t &data) {
  for (int retry = 0; retry < 3; retry++) {
    Wire.beginTransmission(MLX_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) { delay(5); continue; }
    if (Wire.requestFrom(MLX_ADDR, (uint8_t)3) == 3) {
      uint8_t low = Wire.read();
      uint8_t high = Wire.read();
      Wire.read(); 
      data = (high << 8) | low;
      if (data != 0x0000 && data != 0xFFFF) return true;
    }
  }
  return false;
}

float rawToTemp(uint16_t raw) { return (raw * 0.02) - 273.15; }

float getAmbient() {
  uint16_t raw;
  if (read16(0x06, raw)) return rawToTemp(raw);
  return NAN;
}

// Thuật toán lấy trung bình cắt ngọn (Trimmed Mean) cho Nhiệt độ
float getSkinSmooth() {
  const int NUM_SAMPLES = 12;
  float readings[NUM_SAMPLES];
  int validCount = 0;
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    uint16_t raw;
    if (read16(0x07, raw)) {
      float t = rawToTemp(raw);
      if (t > 31.0 && t < 43.0) readings[validCount++] = t;
    }
    delay(15); 
  }
  
  if (validCount < 6) return NAN;

  // Sắp xếp mảng
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = i + 1; j < validCount; j++) {
      if (readings[i] > readings[j]) {
        float temp = readings[i]; readings[i] = readings[j]; readings[j] = temp;
      }
    }
  }
  
  // Loại bỏ 2 mẫu thấp nhất và 2 mẫu cao nhất, lấy trung bình phần còn lại
  float sum = 0;
  for (int i = 2; i < validCount - 2; i++) {
    sum += readings[i];
  }
  return sum / (validCount - 4);
}

float estimateBodyTemp(float T_skin, float T_ambient) {
  float k_env = 0.15 + (T_ambient < REF_AMBIENT ? (REF_AMBIENT - T_ambient) * K_ADJUST_RATE : 0);
  float delta = max(0.0f, T_skin - T_ambient);
  return T_skin + (k_env * delta) + BASE_OFFSET;
}

// ===== CALLBACK NHẬN LỆNH =====
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (data[0] == 1) { 
    isMeasuring = true;
    measurementCount = 0;
    currentTempBody = 0;
    rateSpot = 0;
    beatAvg = 0; 
    for (byte x = 0; x < RATE_SIZE; x++) rates[x] = 0;
    Serial.println("\n🚀 [Hệ thống] BẮT ĐẦU ĐO MỚI...");
  }
}

// ===== SETUP  =====
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(4, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 4;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  if (!particleSensor.begin(Wire, 100000)) { while (1); }
  particleSensor.setup(); 
  particleSensor.setPulseAmplitudeRed(0x0A);
}

void loop() {
  if (!isMeasuring) {
    particleSensor.check();
    return;
  }

  long irValue = particleSensor.getIR();
  if (irValue < 5000) { // Nhấc tay
    if (beatAvg > 0) {
      beatAvg = 0; rateSpot = 0;
      for (byte x = 0; x < RATE_SIZE; x++) rates[x] = 0;
      Serial.println("[!] Vui lòng đặt tay ổn định...");
    }
    return;
  }

  if (checkForBeat(irValue) == true) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    float bpm = 60 / (delta / 1000.0);

    // Lọc bỏ BPM rác (Outlier)
    if (bpm > 45 && bpm < 160) {
      rates[rateSpot++] = (byte)bpm;
      rateSpot %= RATE_SIZE;
      long sum = 0;
      for (byte x = 0; x < RATE_SIZE; x++) sum += rates[x];
      beatAvg = sum / RATE_SIZE;

      if (rateSpot == 0) { 
        measurementCount++;
        
        // 1. ĐO NHIỆT ĐỘ (Dùng lọc mịn)
        float t_amb = getAmbient();
        float t_skin = getSkinSmooth();
        currentTempBody = estimateBodyTemp(t_skin, t_amb);

        // 2. ĐO SpO2 (Giữ nguyên 100 mẫu của bạn)
        for (byte i = 0 ; i < 100 ; i++) {
          while (particleSensor.available() == false) { particleSensor.check(); yield(); }
          redBuffer[i] = particleSensor.getRed();
          irBuffer[i] = particleSensor.getIR();
          particleSensor.nextSample();
        }
        maxim_heart_rate_and_oxygen_saturation(irBuffer, 100, redBuffer, &n_spo2, &validSPO2, &n_hr_unused, &v_hr_unused);
        
        if (n_spo2 > 99) n_spo2 = 99;

        // 3. KIỂM TRA ĐỘ ỔN ĐỊNH TRƯỚC KHI GỬI

        bool isStable = (measurementCount == 1) || (abs(currentTempBody - lastTempSent) < 1.5);

        if (validSPO2 && n_spo2 > 93 && currentTempBody > 34.5 && isStable) {
          
          lastTempSent = currentTempBody;
          
          msg.temp = currentTempBody;
          msg.bpm = beatAvg;
          msg.spo2 = (int)n_spo2;
          strcpy(msg.type, "data");

          esp_now_send(masterAddress, (uint8_t*)&msg, sizeof(msg));
          
          Serial.println("====================================");
          Serial.println("📤 ĐÃ GỬI DỮ LIỆU ỔN ĐỊNH");
          Serial.printf("   BPM: %d | SpO2: %d%% | Temp: %.2f C\n", beatAvg, (int)n_spo2, currentTempBody);
          Serial.println("====================================");
          
          isMeasuring = false; 
        } else {
          Serial.println("-> Dữ liệu chưa ổn định, đang đo lại...");
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