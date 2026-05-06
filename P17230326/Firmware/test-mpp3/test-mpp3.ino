#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

#define DF_RX_PIN 18
#define DF_TX_PIN 17

HardwareSerial dfSerial(2);
DFRobotDFPlayerMini myDFPlayer;

void setup() {
  Serial.begin(115200);

  // init UART2
  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);

  Serial.println("Init DFPlayer...");

  if (!myDFPlayer.begin(dfSerial, true)) {
    Serial.println("❌ DFPlayer NOT FOUND");
    while (true); // stop luôn
  }

  Serial.println("✅ DFPlayer OK");

  myDFPlayer.volume(25);   // 0–30
  delay(1000);

  Serial.println("▶ Playing 0001.mp3");
  myDFPlayer.playMp3Folder(1);
}

void loop() {
}