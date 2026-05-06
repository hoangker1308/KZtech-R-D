#include <WiFi.h>
#include <esp_now.h>

// Lưu MAC peer
uint8_t peerMac[6];
bool peerAdded = false;

// Callback khi gửi
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");

  // In MAC đích
  Serial.print("To: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", info->des_addr[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
}

// Callback khi nhận
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  Serial.print("Received from: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", info->src_addr[i]);
    if (i < 5) Serial.print(":");
  }

  Serial.print(" | Data: ");
  Serial.write(data, len);
  Serial.println();
}

// Convert MAC string -> byte array
bool parseMac(String macStr, uint8_t *mac) {
  if (macStr.length() != 17) return false;

  int values[6];
  if (6 == sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x",
                  &values[0], &values[1], &values[2],
                  &values[3], &values[4], &values[5])) {
    for (int i = 0; i < 6; ++i) {
      mac[i] = (uint8_t) values[i];
    }
    return true;
  }
  return false;
}

// Add peer
void addPeer(uint8_t *mac) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    Serial.println("Peer added successfully!");
    peerAdded = true;
  } else {
    Serial.println("Failed to add peer!");
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.println("\nESP-NOW Pairing Example");

  // In MAC
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("Enter peer MAC (format AA:BB:CC:DD:EE:FF):");
}

void loop() {
  // Đọc MAC từ Serial
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    Serial.print("You entered: ");
    Serial.println(input);

    if (parseMac(input, peerMac)) {
      addPeer(peerMac);
    } else {
      Serial.println("Invalid MAC format!");
    }
  }

  // Nếu đã add peer → gửi thử mỗi 3s
  static unsigned long lastSend = 0;
  if (peerAdded && millis() - lastSend > 3000) {
    const char *msg = "Hello from ESP32";
    esp_now_send(peerMac, (uint8_t *)msg, strlen(msg));
    lastSend = millis();
  }
}