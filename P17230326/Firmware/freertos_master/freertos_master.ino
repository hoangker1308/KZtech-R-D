#include <WiFi.h>
#include "esp_wifi.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <esp_now.h>
#include <LiquidCrystal_I2C.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ====================== PIN & CẤU HÌNH ======================
#define DF_RX_PIN 18      
#define DF_TX_PIN 17      
#define BTN_UP    32
#define BTN_DOWN  25
#define BTN_OK    33

#define PENDING_FILE "/pending.json"
#define ENABLE_WELCOME_SOUND 0

LiquidCrystal_I2C lcd(0x27, 20, 4);
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini myDFPlayer;

const char* ssid = "TP-Link_EAB8"; //
const char* password = "19844244"; //19844244
const char* scriptURL = "https://script.google.com/macros/s/AKfycbzi9Tuhdnte2XvkudKSenT9nzlh73yY8C_Y6bjuYD9jK-l1HitEp0QJ4x1aI5YRYWA/exec";
uint8_t measureMac[6] = {0x50, 0x78, 0x7D, 0xF7, 0x4E, 0xA4};

// ====================== FREERTOS OBJECTS ======================
QueueHandle_t audioQueue;
QueueHandle_t espnowQueue;
QueueHandle_t networkCmdQueue;
SemaphoreHandle_t patientsMutex;

// ====================== DATA STRUCTURES ======================

bool refreshNeeded = false;
struct Patient {
  String id;
  String name;
};
std::vector<Patient> patients;

struct Pending {
  String id;
  int scenIdx;
  float temp;
  int bpm;
  int spo2;
};
std::vector<Pending> pendingMeasurements;

struct Measurement {
  float temp;
  int bpm;
  int spo2;
  bool isError;
};

// Cấu trúc lệnh gửi cho Task Network
enum NetCommand {
  NET_FETCH_PATIENTS,
  NET_SYNC_DATA,
  NET_SEND_MEASUREMENT
};
struct NetRequest {
  NetCommand cmd;
  String patientId;
  int scenIdx;
  Measurement data;
};

// ====================== BIẾN TOÀN CỤC ======================
enum AppState { STATE_IDLE, STATE_MEASURING, STATE_MEASURE_DONE, STATE_SYNC_PENDING, STATE_FETCH_PATIENTS, STATE_OFFLINE };
AppState appState = STATE_IDLE;

Patient currentPatient;
Measurement lastMeasurement;
bool inListMode = true;
int pageStart = 0;
int cursor = 1;
bool showingResult = false;
unsigned long lastButtonTime = 0;

// ====================== ÂM THANH ======================
enum SoundID {
  SOUND_START_SETUP = 1, SOUND_START_MEASURING, SOUND_KO_ON_DINH, SOUND_WELCOME,
  SOUND_PATIENT_LIST_READY, SOUND_NO_PATIENT, SOUND_SELECTED, SOUND_BACK_TO_LIST,
  SOUND_NEXT_PATIENT, SOUND_MEASURING_GUIDE, SOUND_MEASUREMENT_COMPLETE, SOUND_ANALYZING,
  SOUND_OFFLINE_SAVED, SOUND_SYNC_SUCCESS, SOUND_CONNECT_FAIL, SOUND_THANK_YOU
};
const int SCENARIO_BASE = 18;

void enqueueSound(int fileNum) {
  xQueueSend(audioQueue, &fileNum, portMAX_DELAY);
}

// ====================== PHÂN TÍCH KỊCH BẢN ======================
int determineScenario(float temp, int bpm, int spo2) {
  if (spo2 < 90) return 9;
  if (bpm > 120 && spo2 < 95 && temp <= 37.5) return 11;
  if (temp >= 38.5 && bpm > 100) return 2;
  if (temp >= 37.5 && bpm > 100) return 1;
  if (bpm > 100 && temp <= 37.4 && spo2 >= 95) return 4;
  if (bpm < 60 && temp <= 37.4 && spo2 >= 95) return 5;
  if (temp <= 37.4 && spo2 < 95 && spo2 >= 90) return 6;
  if (temp >= 37.5 && bpm >= 60 && bpm <= 100 && spo2 >= 95) return 7;
  if ((temp >= 37.0 && temp <= 37.4) || (bpm >= 95 && bpm <= 105) || (spo2 >= 94 && spo2 <= 96)) return 8;
  if (temp <= 37.0 && bpm >= 70 && bpm <= 85 && spo2 >= 97) return 10;
  return 0; // Bình thường
}

// ====================== TIỆN ÍCH URL ======================
String encodeURL(String str) {
  String encoded = "";
  char c, code0, code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) { encoded += c; } 
    else {
      encoded += '%';
      code0 = (c >> 4) & 0xF; code1 = c & 0xF;
      encoded += char(code0 > 9 ? code0 + 'A' - 10 : code0 + '0');
      encoded += char(code1 > 9 ? code1 + 'A' - 10 : code1 + '0');
    }
  }
  return encoded;
}

// ====================== OFFLINE CACHE (Chạy nội bộ trong Network Task) ======================
void savePendingToFS() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto &p : pendingMeasurements) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = p.id; obj["scenario"] = p.scenIdx;
    obj["temp"] = p.temp; obj["bpm"] = p.bpm; obj["spo2"] = p.spo2;
  }
  File file = LittleFS.open(PENDING_FILE, "w");
  if (file) { serializeJson(doc, file); file.close(); }
}

void loadPendingFromFS() {
  pendingMeasurements.clear();
  if (!LittleFS.exists(PENDING_FILE)) return;
  File file = LittleFS.open(PENDING_FILE, "r");
  if (!file) return;
  String json = file.readString(); file.close();
  JsonDocument doc;
  if (deserializeJson(doc, json)) { LittleFS.remove(PENDING_FILE); return; }
  for (JsonObject obj : doc.as<JsonArray>()) {
    pendingMeasurements.push_back({obj["id"].as<String>(), obj["scenario"], obj["temp"], obj["bpm"], obj["spo2"]});
  }
}

void addPending(const String &id, int scen, float temp, int bpm, int spo2) {
  pendingMeasurements.push_back({id, scen, temp, bpm, spo2});
  savePendingToFS();
  enqueueSound(SOUND_OFFLINE_SAVED);
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) { vTaskDelay(500 / portTICK_PERIOD_MS); }
}

// ====================== LCD HELPER ======================
void displayPatientList() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("LIST BENH NHAN:");
  
  xSemaphoreTake(patientsMutex, portMAX_DELAY);
  if (patients.empty()) {
    lcd.setCursor(0, 1); lcd.print("Khong co benh nhan");
    lcd.setCursor(0, 2); lcd.print("Nhan OK de tai lai");
  } else {
    for (int i = 0; i < 3; i++) {
      int idx = pageStart + i;
      if (idx >= patients.size()) break;
      lcd.setCursor(0, i + 1);
      String prefix = (i + 1 == cursor ? "> " : "  ");
      lcd.print(prefix + patients[idx].name.substring(0, 17));
    }
  }
  xSemaphoreGive(patientsMutex);
}

// ====================== ESP-NOW CALLBACK ======================
typedef struct struct_message {
  char type[8]; float temp; int bpm; int spo2;
} struct_message;

// Gửi data sang Queue, KHÔNG chạy HTTP ở đây!
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  struct_message msg;
  memcpy(&msg, data, sizeof(msg));
  if (strcmp(msg.type, "data") == 0) {
    Measurement m = {msg.temp, msg.bpm, msg.spo2, false};
    xQueueSendFromISR(espnowQueue, &m, NULL);
  }
}

// ====================== TÁC VỤ (TASKS) ======================

// 1. TÁC VỤ NETWORK: Chuyên xử lý HTTP, Google Sheets
void TaskNetwork(void *pvParameters) {
  
  NetRequest req;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  
  while (1) {
    if (appState == STATE_OFFLINE) { 
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  continue;
}
    if (xQueueReceive(networkCmdQueue, &req, portMAX_DELAY)) {
      ensureWiFi();
      
      if (req.cmd == NET_FETCH_PATIENTS) {
    if (WiFi.status() == WL_CONNECTED) {
        String url = String(scriptURL) + "?action=getPatients";
        http.begin(url);
        if (http.GET() > 0) {
            String payload = http.getString();
            JsonDocument doc;
            if (!deserializeJson(doc, payload) && doc["ok"]) {
                xSemaphoreTake(patientsMutex, portMAX_DELAY);
                patients.clear();
                for (JsonObject obj : doc["data"].as<JsonArray>()) {
                    patients.push_back({obj["id"].as<String>(), obj["name"].as<String>()});
                }
                xSemaphoreGive(patientsMutex);
                
                // Sau khi tải xong, báo cho UI biết cần vẽ lại màn hình
                refreshNeeded = true; 
                enqueueSound(patients.empty() ? SOUND_NO_PATIENT : SOUND_PATIENT_LIST_READY);
            }
        }
        http.end();
    }
    appState = STATE_IDLE;
}
      else if (req.cmd == NET_SEND_MEASUREMENT) {
        if (WiFi.status() == WL_CONNECTED) {
          String url = String(scriptURL) + "?action=addMeasurement&id=" + encodeURL(req.patientId) +
                       "&scenario=" + String(req.scenIdx) + "&temp=" + String(req.data.temp, 1) +
                       "&bpm=" + String(req.data.bpm) + "&spo2=" + String(req.data.spo2);
          http.begin(url);
          if (http.GET() > 0) {
            enqueueSound(SOUND_SYNC_SUCCESS);
          } else {
            addPending(req.patientId, req.scenIdx, req.data.temp, req.data.bpm, req.data.spo2);
          }
          http.end();
        } else {
          addPending(req.patientId, req.scenIdx, req.data.temp, req.data.bpm, req.data.spo2);
        }
        appState = STATE_SYNC_PENDING;
      }
      else if (req.cmd == NET_SYNC_DATA) {
        if (!pendingMeasurements.empty() && WiFi.status() == WL_CONNECTED) {
          Pending &p = pendingMeasurements[0];
          String url = String(scriptURL) + "?action=addMeasurement&id=" + encodeURL(p.id) +
                       "&scenario=" + String(p.scenIdx) + "&temp=" + String(p.temp, 1) +
                       "&bpm=" + String(p.bpm) + "&spo2=" + String(p.spo2);
          http.begin(url);
          if (http.GET() > 0) {
            pendingMeasurements.erase(pendingMeasurements.begin());
            if (pendingMeasurements.empty()) {
  LittleFS.remove(PENDING_FILE);
} else {
  savePendingToFS();
}
          }
          http.end();
        }
        vTaskDelay(3000 / portTICK_PERIOD_MS); // Tránh spam quá nhanh khi có nhiều pending
      }
    }
  }
}

// 2. TÁC VỤ AUDIO: Quản lý DFPlayer
void TaskAudio(void *pvParameters) {
  int trackToPlay;
  unsigned long audioStart = 0;
  bool isPlaying = false;

  while (1) {
    if (myDFPlayer.available()) {
      uint8_t type = myDFPlayer.readType();
      if (type == DFPlayerPlayFinished || type == DFPlayerError) {
        isPlaying = false;
      }
    }

    if (isPlaying && (millis() - audioStart > 5000)) {
      isPlaying = false; // Reset timeout
    }

    if (!isPlaying) {
      if (xQueueReceive(audioQueue, &trackToPlay, 50 / portTICK_PERIOD_MS)) {
        myDFPlayer.playMp3Folder(trackToPlay);
        isPlaying = true;
        audioStart = millis();
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Khoảng cách giữa 2 câu
      }
    } else {
      vTaskDelay(50 / portTICK_PERIOD_MS);
    }
  }
}

// 3. TÁC VỤ UI (Nút bấm, LCD, State Machine)
void TaskUI(void *pvParameters) {
  #if ENABLE_WELCOME_SOUND
  enqueueSound(SOUND_WELCOME);
#endif

  Measurement newMeas;
  
  while (1) {
    if (refreshNeeded) {
            refreshNeeded = false;
            displayPatientList();
        }
    // 1. Kiểm tra dữ liệu từ ESP-NOW
    if (xQueueReceive(espnowQueue, &newMeas, 0)) {

  if (newMeas.temp < 35.0) {
    enqueueSound(30);
  } else {
    lastMeasurement = newMeas;
    int scenIdx = determineScenario(newMeas.temp, newMeas.bpm, newMeas.spo2);

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(currentPatient.name.substring(0, 20));
    lcd.setCursor(0, 1); lcd.print("Nhiet: " + String(newMeas.temp, 1));
    lcd.setCursor(0, 2); lcd.print("Tim: " + String(newMeas.bpm));
    lcd.setCursor(0, 3); lcd.print("SpO2: " + String(newMeas.spo2));

    enqueueSound(SCENARIO_BASE + scenIdx);

    if (appState == STATE_OFFLINE || WiFi.status() != WL_CONNECTED) {
  addPending(currentPatient.id, scenIdx, newMeas.temp, newMeas.bpm, newMeas.spo2);
} else {
  NetRequest req = {NET_SEND_MEASUREMENT, currentPatient.id, scenIdx, newMeas};
  xQueueSend(networkCmdQueue, &req, portMAX_DELAY);
}

    showingResult = true;
    appState = STATE_MEASURE_DONE;
  }
}

    // 2. Xử lý Nút Bấm
    if (millis() - lastButtonTime > 200) {
      if (digitalRead(BTN_OK) == LOW) {
        lastButtonTime = millis();
        if (showingResult) {
  showingResult = false; 
  inListMode = true;
  appState = STATE_IDLE;   

  displayPatientList();

  NetRequest req = {NET_SYNC_DATA, "", 0, {}};
  xQueueSend(networkCmdQueue, &req, portMAX_DELAY);
}
        else if (appState == STATE_IDLE) {
          if (inListMode) {
            xSemaphoreTake(patientsMutex, portMAX_DELAY);
            bool emptyList = patients.empty();
            xSemaphoreGive(patientsMutex);
            
            if (emptyList) {
              lcd.clear(); lcd.print("Dang tai...");
              NetRequest req = {NET_FETCH_PATIENTS, "", 0, {}};
              xQueueSend(networkCmdQueue, &req, portMAX_DELAY);
              appState = STATE_FETCH_PATIENTS;
            } else {
              xSemaphoreTake(patientsMutex, portMAX_DELAY);
              int idx = pageStart + cursor - 1;
              if (idx < patients.size()) {
                currentPatient = patients[idx];
                enqueueSound(SOUND_SELECTED); enqueueSound(SOUND_START_MEASURING);
                lcd.clear(); lcd.setCursor(0, 0); lcd.print(currentPatient.name);
                lcd.setCursor(0, 1); lcd.print("Dang doi ket qua...");
                uint8_t cmd = 1; esp_now_send(measureMac, &cmd, 1);
                appState = STATE_MEASURING; inListMode = false;
              }
              xSemaphoreGive(patientsMutex);
            }
          } else {
            inListMode = true; displayPatientList();
          }
        }
      }
      
      if (digitalRead(BTN_UP) == LOW && inListMode && appState == STATE_IDLE && !showingResult) {
        lastButtonTime = millis();
        xSemaphoreTake(patientsMutex, portMAX_DELAY);
        if (!patients.empty()) {
          cursor--;
          if (cursor < 1) {
            cursor = 3; pageStart -= 3;
            if (pageStart < 0) pageStart = ((patients.size() - 1) / 3) * 3;
          }
          xSemaphoreGive(patientsMutex);
          displayPatientList();
        } else xSemaphoreGive(patientsMutex);
      }
      
      if (digitalRead(BTN_DOWN) == LOW && inListMode && appState == STATE_IDLE && !showingResult) {
        lastButtonTime = millis();
        xSemaphoreTake(patientsMutex, portMAX_DELAY);
        if (!patients.empty()) {
          cursor++;
          if (cursor > 3 || pageStart + cursor - 1 >= patients.size()) {
            cursor = 1; pageStart += 3;
            if (pageStart >= patients.size()) pageStart = 0;
          }
          xSemaphoreGive(patientsMutex);
          displayPatientList();
        } else xSemaphoreGive(patientsMutex);
      }
    }
    
    // Tự động kiểm tra đồng bộ ngầm định kỳ (khi rảnh)
    static unsigned long lastAutoSync = 0;
    if (appState == STATE_IDLE && millis() - lastAutoSync > 15000) {
      lastAutoSync = millis();
      NetRequest req = {NET_SYNC_DATA, "", 0, {}};
      xQueueSend(networkCmdQueue, &req, 0);
    }
    
    vTaskDelay(20 / portTICK_PERIOD_MS); // Giảm tải CPU
  }
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);

  lcd.init(); lcd.backlight(); lcd.clear(); lcd.print("Dang khoi dong...");

  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  if (myDFPlayer.begin(dfSerial, true)) {
    myDFPlayer.volume(25);
    myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
  }

  // Khởi tạo FreeRTOS Objects
  audioQueue = xQueueCreate(10, sizeof(int));
  espnowQueue = xQueueCreate(5, sizeof(Measurement));
  networkCmdQueue = xQueueCreate(5, sizeof(NetRequest));
  patientsMutex = xSemaphoreCreateMutex();

  enqueueSound(SOUND_START_SETUP);

  if (LittleFS.begin(true)) {
    loadPendingFromFS();
  }

  WiFi.mode(WIFI_STA);
WiFi.begin(ssid, password);

unsigned long start = millis();
bool wifiOK = false;

while (millis() - start < 10000) {
  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true;
    break;
  }
  delay(500);
}
if (wifiOK) {
  Serial.println("WiFi CONNECTED");
  appState = STATE_FETCH_PATIENTS;
} else {
  Serial.println("OFFLINE MODE");

  appState = STATE_OFFLINE;

  enqueueSound(SOUND_CONNECT_FAIL); // báo mất mạng
}

  int ch = WiFi.channel();
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, measureMac, 6);
    peerInfo.channel = ch; peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  // Yêu cầu lấy dữ liệu lần đầu
  NetRequest req = {NET_FETCH_PATIENTS, "", 0, {}};
  xQueueSend(networkCmdQueue, &req, portMAX_DELAY);
  appState = STATE_FETCH_PATIENTS;

  // Tạo Tasks
  // Task UI: Mức độ ưu tiên cao nhất, chạy xử lý nhanh
  xTaskCreate(TaskUI, "UI_Task", 4096, NULL, 3, NULL);
  
  // Task Network: Stack lớn vì thao tác HTTPS (TLS/SSL) cần nhiều RAM
  xTaskCreate(TaskNetwork, "Net_Task", 8192, NULL, 1, NULL);
  
  // Task Audio: Mức độ ưu tiên trung bình
  xTaskCreate(TaskAudio, "Audio_Task", 2048, NULL, 2, NULL);
}

// Loop không làm gì vì FreeRTOS Tasks đã quản lý hết
void loop() {
  vTaskDelete(NULL); // Hủy task loop mặc định để tiết kiệm RAM
}