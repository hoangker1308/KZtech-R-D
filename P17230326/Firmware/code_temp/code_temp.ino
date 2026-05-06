#include <Wire.h>

// Cấu hình chân I2C cho ESP32-C3
#define SDA_PIN 6
#define SCL_PIN 7
#define MLX_ADDR 0x5A

// Thông số hiệu chuẩn (Đã tối ưu cho khoảng cách 1cm)
const float BASE_OFFSET = 1.25;      // Offset bạn đã xác định ở 1cm
const float REF_AMBIENT = 30.0;    // Điểm mốc nhiệt độ môi trường
const float K_ADJUST_RATE = 0.008; // Hệ số điều chỉnh k_env theo môi trường

// ===== HÀM ĐỌC RAW 16-BIT TỪ MLX90614 =====
bool read16(uint8_t reg, uint16_t &data) {
  for (int retry = 0; retry < 5; retry++) {
    Wire.beginTransmission(MLX_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
      delay(10);
      continue;
    }

    if (Wire.requestFrom(MLX_ADDR, (uint8_t)3) == 3) {
      uint8_t low  = Wire.read();
      uint8_t high = Wire.read();
      Wire.read(); // Bỏ qua PEC (Packet Error Code)
      
      data = (high << 8) | low;
      if (data != 0x0000 && data != 0xFFFF) return true;
    }
  }
  return false;
}

float rawToTemp(uint16_t raw) {
  return (raw * 0.02) - 273.15;
}

// ===== ĐỌC NHIỆT ĐỘ MÔI TRƯỜNG =====
float getAmbient() {
  uint16_t raw;
  if (read16(0x06, raw)) return rawToTemp(raw);
  return NAN;
}

// ===== ĐỌC NHIỆT ĐỘ DA (SKIN) VỚI BỘ LỌC ĐỈNH =====
float getSkinPeak() {
  float readings[15];
  int validCount = 0;

  // Lấy 15 mẫu trong 0.5 giây để tìm điểm nóng nhất (động mạch trán)
  for (int i = 0; i < 15; i++) {
    uint16_t raw;
    if (read16(0x07, raw)) {
      float t = rawToTemp(raw);
      if (t > 30.0 && t < 45.0) { // Chỉ nhận giá trị trong dải nhiệt độ người
        readings[validCount++] = t;
      }
    }
    delay(35);
  }

  if (validCount < 5) return NAN;

  // Sắp xếp đơn giản để lấy các giá trị cao nhất (giảm nhiễu)
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = i + 1; j < validCount; j++) {
      if (readings[i] < readings[j]) {
        float temp = readings[i];
        readings[i] = readings[j];
        readings[j] = temp;
      }
    }
  }
  // Trả về trung bình của 3 mẫu cao nhất
  return (readings[0] + readings[1] + readings[2]) / 3.0;
}

// ===== THUẬT TOÁN BÙ NHIỆT ĐỘNG (CORE LOGIC) =====
float estimateBodyTemp(float T_skin, float T_ambient) {
  // 1. Tính k_env động: Trời càng lạnh, k_env càng tăng để bù mất nhiệt da
  float k_env = 0.15; 
  if (T_ambient < REF_AMBIENT) {
    k_env = 0.15 + (REF_AMBIENT - T_ambient) * K_ADJUST_RATE;
  }

  // 2. Tính Delta chênh lệch
  float delta = T_skin - T_ambient;
  if (delta < 0) delta = 0;

  // 3. Công thức tính Body (Linear Compensation)
  // T_body = T_skin + k_env * (T_skin - T_amb) + Offset
  float body = T_skin + (k_env * delta) + BASE_OFFSET;

  return body;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Khởi tạo I2C cho ESP32-C3
  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  
  Serial.println("\n--- MLX90614 PRO SYSTEM READY ---");
  Serial.println("Khoang cach do: CO DINH 1CM");
  Serial.println("Che do: Bu nhiet dong theo moi truong < 30C");
}

void loop() {
  float t_amb  = getAmbient();
  float t_skin = getSkinPeak();

  if (isnan(t_amb) || isnan(t_skin)) {
    Serial.println("[!] Dang cho doi tuong...");
  } 
  else if (t_skin < t_amb + 1.5) {
    Serial.println("[?] Vui long dua sat tran (1cm)");
  }
  else {
    float t_body = estimateBodyTemp(t_skin, t_amb);

    // Xuất dữ liệu
    Serial.print("Amb: "); Serial.print(t_amb, 2);
    Serial.print(" | Skin: "); Serial.print(t_skin, 2);
    Serial.print(" => BODY: "); Serial.print(t_body, 2);
    
    if (t_body >= 37.5) Serial.println(" [CANH BAO SOT!]");
    else if (t_body < 35.0) Serial.println(" [DO SAI/XA]");
    else Serial.println(" [BINH THUONG]");
  }

  delay(1000); // Đợi 1 giây trước lần đo tiếp theo
}