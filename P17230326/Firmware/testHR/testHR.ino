/*
  Cấu hình in dữ liệu định kỳ cho ESP32-C3
  SDA -> IO6, SCL -> IO7
*/

#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

MAX30105 particleSensor;

const byte RATE_SIZE = 4; 
byte rates[RATE_SIZE];    
byte rateSpot = 0;
long lastBeat = 0;        

float beatsPerMinute;
int beatAvg;

// Biến quản lý thời gian in
unsigned long lastPrintTime = 0;
const unsigned long printInterval = 400; // 400ms = in ~25 kết quả trong 10 giây

void setup() {
  Serial.begin(115200);
  
  // Khởi tạo I2C cho ESP32-C3
  Wire.begin(6, 7); 

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 was not found.");
    while (1);
  }

  particleSensor.setup(); 
  particleSensor.setPulseAmplitudeRed(0x0A); 
  particleSensor.setPulseAmplitudeGreen(0);  
}

void loop() {
  long irValue = particleSensor.getIR();

  // Thuật toán phát hiện nhịp tim (Giữ nguyên)
  if (checkForBeat(irValue) == true) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute < 255 && beatsPerMinute > 20) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      beatAvg = 0;
      for (byte x = 0 ; x < RATE_SIZE ; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
  }

  // Cấu chế in dữ liệu theo chu kỳ (Không dùng delay)
  if (millis() - lastPrintTime >= printInterval) {
    lastPrintTime = millis();

    Serial.print("IR=");
    Serial.print(irValue);
    Serial.print(", BPM=");
    Serial.print(beatsPerMinute);
    Serial.print(", Avg BPM=");
    Serial.print(beatAvg);

    if (irValue < 50000) {
      Serial.print(" [No finger]");
    }
    Serial.println();
  }
}