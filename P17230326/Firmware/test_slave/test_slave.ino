#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

// --- CẤU HÌNH PIN & ĐỊA CHỈ ---
#define SDA_PIN 6
#define SCL_PIN 7
#define MLX_ADDR 0x5A

// --- THÔNG SỐ HIỆU CHUẨN MLX90614 ---
const float BASE_OFFSET = 1.25;
const float REF_AMBIENT = 30.0;
const float K_ADJUST_RATE = 0.008;

// --- ĐỐI TƯỢNG VÀ BIẾN ---
MAX30105 particleSensor;
const byte RATE_SIZE = 10; 
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
long sumBPM = 0;
long sumSpO2 = 0;
float sumTemp = 0;
int validMeasurements = 0;

unsigned long lastPrintTime = 0;
float currentTempBody = 0;

// ===== HÀM HỖ TRỢ MLX90614 (CỦA BẠN) =====
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

float getSkinPeak() {
  float readings[15];
  int validCount = 0;
  for (int i = 0; i < 15; i++) {
    uint16_t raw;
    if (read16(0x07, raw)) {
      float t = rawToTemp(raw);
      if (t > 30.0 && t < 45.0) readings[validCount++] = t;
    }
    delay(35); 
  }
  if (validCount < 5) return NAN;
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = i + 1; j < validCount; j++) {
      if (readings[i] < readings[j]) {
        float temp = readings[i]; readings[i] = readings[j]; readings[j] = temp;
      }
    }
  }
  return (readings[0] + readings[1] + readings[2]) / 3.0;
}

float estimateBodyTemp(float T_skin, float T_ambient) {
  float k_env = 0.15 + (T_ambient < REF_AMBIENT ? (REF_AMBIENT - T_ambient) * K_ADJUST_RATE : 0);
  float delta = max(0.0f, T_skin - T_ambient);
  return T_skin + (k_env * delta) + BASE_OFFSET;
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  // Quan trọng: MLX90614 cần Standard Mode (100kHz) để ổn định
  Wire.begin(SDA_PIN, SCL_PIN, 100000);

  if (!particleSensor.begin(Wire, 100000)) {
    Serial.println("MAX30105 was not found.");
    while (1);
  }

  particleSensor.setup(); 
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0); 
  
  Serial.println("\n--- PETBOT READY: TEMP -> SpO2 -> BPM ---");
}

void loop() {
  long irValue = particleSensor.getIR();

  // Reset nếu không có tay (dựa trên mức IR ~9000 của bạn)
  if (irValue < 5000) {
    if (measurementCount > 0) {
      measurementCount = 0;
      validMeasurements = 0;
      sumBPM = 0; sumSpO2 = 0; sumTemp = 0;
      currentTempBody = 0;
      Serial.println("[!] Da nhac tay - Reset.");
    }
  }

  // Phát hiện nhịp tim để bắt đầu chu kỳ đo
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

      // Khi đủ dữ liệu nhịp tim để ổn định (mỗi chu kỳ 10 beats)
      if (rateSpot == 0) { 
        measurementCount++;
        Serial.println("\n--- BAT DAU CHU KY DO MOI ---");
        
        // BƯỚC 1: ĐO NHIỆT ĐỘ TRƯỚC (Ưu tiên số 1)
        float t_amb = getAmbient();
        float t_skin = getSkinPeak();
        
        if (!isnan(t_skin) && t_skin > (t_amb + 1.0)) {
          currentTempBody = estimateBodyTemp(t_skin, t_amb);
          Serial.print("[1] Temp Done: "); Serial.print(currentTempBody); Serial.println(" C");
        } else {
          currentTempBody = 0;
          Serial.println("[1] Temp Fail: Vui long de sat cam bien!");
        }

        // BƯỚC 2: ĐO SpO2 (100 mẫu ~ 2 giây)
        Serial.println("[2] Dang do SpO2... Giu tinh tay");
        for (byte i = 0 ; i < 100 ; i++) {
          while (particleSensor.available() == false) particleSensor.check();
          redBuffer[i] = particleSensor.getRed();
          irBuffer[i] = particleSensor.getIR();
          particleSensor.nextSample();
        }
        maxim_heart_rate_and_oxygen_saturation(irBuffer, 100, redBuffer, &n_spo2, &validSPO2, &n_hr_unused, &v_hr_unused);

        // BƯỚC 3: TỔNG HỢP TRUNG BÌNH (Bỏ qua lần đầu tiên - Warm up)
        if (measurementCount >= 2 && validSPO2 && n_spo2 > 90 && currentTempBody > 34.0) {
          validMeasurements++;
          sumBPM += beatAvg;
          sumSpO2 += n_spo2;
          sumTemp += currentTempBody;

          Serial.println("====================================");
          Serial.print("   KET QUA TB (Lan "); Serial.print(validMeasurements); Serial.println(")");
          Serial.print("   BPM: "); Serial.println(sumBPM / validMeasurements);
          Serial.print("   SpO2: "); Serial.print(sumSpO2 / validMeasurements); Serial.println("%");
          Serial.print("   TEMP: "); Serial.print(sumTemp / validMeasurements, 2); Serial.println(" C");
          Serial.println("====================================");
        } else {
          Serial.println("-> Dang on dinh (Warm-up)...");
        }
      }
    }
  }

  // In log nhanh để theo dõi trạng thái
  if (millis() - lastPrintTime >= 500) {
    lastPrintTime = millis();
    Serial.print("IR="); Serial.print(irValue);
    Serial.print(" | BPM="); Serial.print(beatAvg);
    Serial.print(" | SpO2="); Serial.print(n_spo2 > 0 ? String(n_spo2) : ".."); 
    Serial.print("% | Temp="); Serial.print(currentTempBody > 0 ? String(currentTempBody, 1) : ".."); Serial.println("C");
  }
}