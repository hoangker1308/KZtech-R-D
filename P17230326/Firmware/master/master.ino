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
#include <queue>


// ====================== PIN & CẤU HÌNH ======================
#define DF_RX_PIN 18      
#define DF_TX_PIN 17      
#define BTN_UP    32
#define BTN_DOWN  25
#define BTN_OK    33

#define PENDING_FILE "/pending.json"
#define MAX_SOUND_QUEUE 10

LiquidCrystal_I2C lcd(0x27, 20, 4);           // LCD2004 I2C
HardwareSerial dfSerial(2);                   // UART2
DFRobotDFPlayerMini myDFPlayer;

// WiFi & Google Apps Script (THAY BẰNG GIÁ TRỊ CỦA BẠN)
const char* ssid = ".KZtech"; //TP-Link_EAB8
const char* password = "88888888"; //19844244
const char* scriptURL = "https://script.google.com/macros/s/AKfycbzi9Tuhdnte2XvkudKSenT9nzlh73yY8C_Y6bjuYD9jK-l1HitEp0QJ4x1aI5YRYWA/exec";

// MAC của thiết bị đo (ESP32-C3) - THAY BẰNG MAC THỰC
uint8_t measureMac[6] = {0x50, 0x78, 0x7D, 0xF7, 0x4E, 0xA4};   

// ====================== BIẾN TOÀN CỤC ======================
std::queue<int> soundQueue;

enum AppState {
  STATE_IDLE,
  STATE_MEASURING,
  STATE_MEASURE_DONE,
  STATE_SYNC_PENDING,
  STATE_FETCH_PATIENTS
};

enum AudioState {
  AUDIO_IDLE,
  AUDIO_PLAYING
};

AudioState audioState = AUDIO_IDLE;
int currentTrack = -1;

AppState appState = STATE_IDLE;

unsigned long httpStartTime = 0;
bool httpRunning = false;


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
int syncIndex = 0;
unsigned long lastSyncTime = 0;

unsigned long lastSoundTime = 0;

struct Measurement {
  float temp;
  int bpm;
  int spo2;
  bool isError;
};

Patient currentPatient;
Measurement lastMeasurement;
bool inListMode = true;
int pageStart = 0;
int cursor = 1;
unsigned long lastButtonTime = 0;
const unsigned long DEBOUNCE = 200;
bool littleFS_OK = false;
bool receivedAck = false;

unsigned long resultTime = 0;
bool showingResult = false;

// Âm thanh (số file trên thẻ SD mp3/0001.mp3, 0002.mp3...)
enum SoundID {
  SOUND_START_SETUP = 1,
  SOUND_START_MEASURING,
  SOUND_KO_ON_DINH,
  SOUND_WELCOME,
  SOUND_PATIENT_LIST_READY,
  SOUND_NO_PATIENT,
  SOUND_SELECTED,
  SOUND_BACK_TO_LIST,
  SOUND_NEXT_PATIENT,
  SOUND_MEASURING_GUIDE,
  SOUND_MEASUREMENT_COMPLETE,
  SOUND_ANALYZING,
  SOUND_OFFLINE_SAVED,
  SOUND_SYNC_SUCCESS,
  SOUND_CONNECT_FAIL,
  SOUND_THANK_YOU
};

const int SCENARIO_BASE = 18;
const String scenarioNames[13] = {
  "Binh thuong",
  "Sot nhe + nhip tim cao",
  "Sot cao + tim cao",
  "SpO2 thap",
  "Nhip tim cao nhung nhiet do binh thuong",
  "Nhip tim thap",
  "Nhiet do binh thuong nhung SpO2 thap",
  "Sot nhe nhung chi so binh thuong",
  "Gia tri can bien",
  "Canh bao khan cap",
  "Muc do hoan hao",
  "Canh bao: tim cao + SpO2 thap"
};

static unsigned long last = 0;
static bool printedNoPending = false;

unsigned long audioStartTime = 0;
const unsigned long AUDIO_TIMEOUT = 5000;

// ====================== HÀM PHÁT ÂM THANH ======================
#define MAX_SOUND_QUEUE 10

void enqueueSound(int fileNum) {
  Serial.print("ADD SOUND: ");
  Serial.println(fileNum);
  soundQueue.push(fileNum);
}
bool canPlaySound() {
  
  if (millis() - lastSoundTime < 1000) return false;
  lastSoundTime = millis();
  return true;
}
void processSoundQueue() {
  if (audioState == AUDIO_IDLE && !soundQueue.empty()) {

    if (!canPlaySound()) return;

    int next = soundQueue.front();
    soundQueue.pop();

    Serial.print("Queue play: ");
    Serial.println(next);

    playSound(next);
  }
}
void playSound(int fileNum) {
  if (audioState == AUDIO_PLAYING) return;

  Serial.print("▶ Play: ");
  Serial.println(fileNum);

  myDFPlayer.playMp3Folder(fileNum);

  audioState = AUDIO_PLAYING;
  audioStartTime = millis();
}

// ====================== PHÂN TÍCH KỊCH BẢN ======================
int determineScenario(float temp, int bpm, int spo2) {
  if (spo2 < 90) return 9;                    // kịch bản 11
  if (bpm > 120 && spo2 < 95 && temp <= 37.5) return 11; // kịch bản 13 mới
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

String buildURL(String action, String id = "", int scenario = -1) {
  String url = String(scriptURL);
  url += "?action=" + action;

  if (id != "") {
    url += "&id=" + id;
  }

  if (scenario != -1) {
    url += "&scenario=" + String(scenario);
  }

  return url;
}

// ====================== OFFLINE CACHE ======================
void savePendingToFS() {

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (auto &p : pendingMeasurements) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = p.id;
obj["scenario"] = p.scenIdx;
obj["temp"] = p.temp;
obj["bpm"] = p.bpm;
obj["spo2"] = p.spo2;
  }

  File file = LittleFS.open(PENDING_FILE, "w");
  if (!file) {
    Serial.println("❌ Cannot write pending.json");
    return;
  }

  serializeJson(doc, file);
  file.close();

  Serial.println("✅ Saved pending.json (JSON)");
}

void loadPendingFromFS() {
  pendingMeasurements.clear();

  if (!LittleFS.exists(PENDING_FILE)) {
    Serial.println("⚠️ No pending.json");
    return;
  }

  File file = LittleFS.open(PENDING_FILE, "r");
  if (!file) return;

  String json = file.readString();
  file.close();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);

  if (err) {
    Serial.println("❌ JSON parse error → reset cache");
    LittleFS.remove(PENDING_FILE);
    return;
  }

  for (JsonObject obj : doc.as<JsonArray>()) {
    String id = obj["id"].as<String>();
    int scen = obj["scenario"];

    pendingMeasurements.push_back({
  obj["id"].as<String>(),
  obj["scenario"],
  obj["temp"],
  obj["bpm"],
  obj["spo2"]
});
  }

  Serial.printf("✅ Loaded %d pending (JSON)\n", pendingMeasurements.size());
}



void addPending(const String &id, int scen, float temp, int bpm, int spo2) {
  pendingMeasurements.push_back({
    id,
    scen,
    temp,
    bpm,
    spo2
  });

  savePendingToFS();
}

void ensureWiFi() {
  static unsigned long lastTry = 0;

  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastTry < 3000) return;

  lastTry = millis();

  Serial.println("🔄 Reconnecting WiFi...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
}

String httpGET(String url) {
  ensureWiFi();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(8000);

  // 🔥 QUAN TRỌNG
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  Serial.println("📡 GET: " + url);

  if (!http.begin(client, url)) {
    Serial.println("❌ HTTP begin FAILED");
    return "";
  }

  int code = http.GET();

  Serial.print("HTTP Code: ");
  Serial.println(code);

  String payload = "";

  if (code > 0) {
    payload = http.getString();
    Serial.println("RAW JSON:");
    Serial.println(payload);
  } else {
    Serial.println("❌ HTTP GET FAILED");
  }

  http.end();
  return payload;
}

String httpPOST(String url, String body) {
  ensureWiFi();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(5000);

  Serial.println("📡 POST: " + url);
  Serial.println("BODY:");
  Serial.println(body);

  if (!http.begin(client, url)) {
    Serial.println("❌ HTTP begin FAILED");
    return "";
  }

  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);

  Serial.print("HTTP Code send: ");
  Serial.println(code);

  String payload = "";

  if (code > 0) {
    payload = http.getString();
    Serial.println("Response:");
    Serial.println(payload);
  } else {
    Serial.println("❌ HTTP POST FAILED");
  }

  http.end();
  return payload;
}

bool sendHTTP(String url) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(8000);

  if (!http.begin(client, url)) return false;

  int code = http.GET();
  bool ok = (code > 0);

  if (ok) {
    Serial.println(http.getString());
  }

  http.end();
  return ok;
}

// ====================== GOOGLE SHEET ======================
void fetchPatients() {
  Serial.println("== fetchPatients START ==");

  String url = String(scriptURL) + "?action=getPatients";

  String payload = httpGET(url);

  if (payload == "") {
    Serial.println("❌ No data");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.println("❌ JSON parse failed");
    return;
  }

  if (!doc["ok"]) {
    Serial.println("❌ API not OK");
    return;
  }

  JsonArray arr = doc["data"];
  patients.clear();

  for (JsonObject obj : arr) {
    Patient p;
    p.id = obj["id"].as<String>();
    p.name = obj["name"].as<String>();
    patients.push_back(p);
  }

  // ✅ DÒNG BẠN CẦN THÊM / SỬA
  Serial.printf("✅ Loaded %d patients\n", patients.size());

  Serial.println("== fetchPatients END ==");
}



String encodeURL(String str) {
  String encoded = "";
  char c;
  char code0;
  char code1;

  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);

    if (isalnum(c)) {
      encoded += c;
    } else {
      encoded += '%';
      code0 = (c >> 4) & 0xF;
      code1 = c & 0xF;
      encoded += char(code0 > 9 ? code0 + 'A' - 10 : code0 + '0');
      encoded += char(code1 > 9 ? code1 + 'A' - 10 : code1 + '0');
    }
  }
  return encoded;
}

bool sendToSheet(const String& id, int scenIdx) {

  // 🔥 THÊM Ở ĐÂY
  ensureWiFi();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected → save pending");
    addPending(
  id,
  scenIdx,
  lastMeasurement.temp,
  lastMeasurement.bpm,
  lastMeasurement.spo2
);
    return false;
  }

  String url = String(scriptURL) +
               "?action=addMeasurement" +
               "&id=" + encodeURL(id) +
               "&scenario=" + String(scenIdx) +
               "&temp=" + String(lastMeasurement.temp, 1) +
               "&bpm=" + String(lastMeasurement.bpm) +
               "&spo2=" + String(lastMeasurement.spo2);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  http.begin(client, url);
  ensureWiFi();
  int code = http.GET();

  Serial.print("HTTP Code send: ");
  Serial.println(code);

  String payload = http.getString();
  Serial.println(payload);

  if (code > 0) {
    Serial.println("✅ Sent OK");
    http.end();
    return true;
  }

  http.end();

  addPending(
  id,
  scenIdx,
  lastMeasurement.temp,
  lastMeasurement.bpm,
  lastMeasurement.spo2
);
  return false;
}


unsigned long fetchTime = 0;
bool fetchStarted = false;

void handleFetchPatients() {

  static bool started = false;
  static unsigned long t = 0;

  if (!started) {
    started = true;
    t = millis();
    return;
  }

  if (millis() - t < 300) return;

  started = false;

  String url = String(scriptURL) + "?action=getPatients";
  String payload = httpGET(url);

  if (payload == "") {
    Serial.println("❌ fetch fail");
    appState = STATE_IDLE;
    displayPatientList();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.println("❌ JSON lỗi");
    appState = STATE_IDLE;
    return;
  }

  // 🔥 THIẾU CHECK NÀY → GÂY TREO LOGIC
  if (!doc["ok"]) {
    Serial.println("❌ API not OK");
    appState = STATE_IDLE;
    return;
  }

  patients.clear();

  for (JsonObject obj : doc["data"].as<JsonArray>()) {
    patients.push_back({
      obj["id"].as<String>(),
      obj["name"].as<String>()
    });
  }

  Serial.printf("✅ Loaded %d patients\n", patients.size());

  displayPatientList();
  appState = STATE_IDLE;
}


void handleSyncPending() {

  if (pendingMeasurements.empty()) {
  if (!printedNoPending) {
    Serial.println("✅ No pending");
    printedNoPending = true;
  }
  appState = STATE_IDLE;
  return;
}
printedNoPending = false;

  // gửi mỗi 3 giây
  if (millis() - lastSyncTime < 3000) return;
  lastSyncTime = millis();

  Pending &p = pendingMeasurements[0];

  String url = String(scriptURL) +
             "?action=addMeasurement" +
             "&id=" + p.id +
             "&scenario=" + String(p.scenIdx) +
             "&temp=" + String(p.temp,1) +
             "&bpm=" + String(p.bpm) +
             "&spo2=" + String(p.spo2);

  Serial.println("📡 Sync: " + url);

  bool ok = sendHTTP(url);

  if (ok) {
    Serial.println("✅ Sync OK");

    pendingMeasurements.erase(pendingMeasurements.begin());

if (!pendingMeasurements.empty()) {
  savePendingToFS();
} else {
  LittleFS.remove(PENDING_FILE);  // sạch luôn file
  Serial.println("🧹 Cleared pending.json");
}
  } else {
    Serial.println("❌ Sync fail");
  }
}

void handleButtons() {


  if (millis() - last < 200) return;

// ===== OK =====
if (digitalRead(BTN_OK) == LOW) {
  last = millis();


  // ===== nếu đang xem kết quả =====
  if (showingResult) {
    showingResult = false;

    inListMode = true;
    displayPatientList();

    appState = STATE_SYNC_PENDING; // sync sau khi user xác nhận

    return;
  }
  if (appState == STATE_MEASURING) return;
  if (appState != STATE_IDLE) return;

  if (inListMode) {

    if (patients.empty()) {
      refreshPatientList();
      return;
    }

    int idx = pageStart + cursor - 1;

    if (idx < patients.size()) {

      currentPatient = patients[idx];
      enqueueSound(SOUND_SELECTED);
enqueueSound(SOUND_START_MEASURING);

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(currentPatient.name);

      lcd.setCursor(0, 1);
      lcd.print("Dang doi ket qua...");

      uint8_t cmd = 1;
      esp_now_send(measureMac, &cmd, 1);

      appState = STATE_MEASURING;
      inListMode = false;
    }

  } else {
    inListMode = true;
    displayPatientList();
  }
}

  // ===== UP =====
  if (digitalRead(BTN_UP) == LOW && inListMode && appState == STATE_IDLE && !showingResult) {
    last = millis();

    if (patients.empty()) return;

    cursor--;

    if (cursor < 1) {
      cursor = 3;
      pageStart -= 3;

      if (pageStart < 0) {
        pageStart = ((patients.size() - 1) / 3) * 3;
      }
    }

    displayPatientList();
  }

  // ===== DOWN =====
  if (digitalRead(BTN_DOWN) == LOW && inListMode && appState == STATE_IDLE && !showingResult) {
    last = millis();

    if (patients.empty()) return;

    cursor++;

    if (cursor > 3 || pageStart + cursor - 1 >= patients.size()) {
      cursor = 1;
      pageStart += 3;

      if (pageStart >= patients.size()) {
        pageStart = 0;
      }
    }

    displayPatientList();
  }
}

void finishRefresh() {
  pageStart = 0;
  cursor = 1;

  if (patients.empty()) {
    enqueueSound(SOUND_NO_PATIENT);
  } else {
    enqueueSound(SOUND_PATIENT_LIST_READY);
  }

  displayPatientList();
  appState = STATE_IDLE;
}

// ====================== ESP-NOW (CORE 3.x) ======================
typedef struct struct_message {
  char type[8];
  float temp;
  int bpm;
  int spo2;
} struct_message;

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  struct_message msg;
  memcpy(&msg, data, sizeof(msg));

  if (strcmp(msg.type, "data") == 0) {

    lastMeasurement = {msg.temp, msg.bpm, msg.spo2, false};

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(currentPatient.name);

    lcd.setCursor(0, 1);
    lcd.print("Nhiet: " + String(msg.temp,1));

    lcd.setCursor(0, 2);
    lcd.print("Tim: " + String(msg.bpm));

    lcd.setCursor(0, 3);
    lcd.print("SpO2: " + String(msg.spo2));

    int scenIdx = determineScenario(msg.temp, msg.bpm, msg.spo2);
    enqueueSound(SCENARIO_BASE + scenIdx);
    bool ok = sendToSheet(currentPatient.id, scenIdx);

if (!ok) {
  Serial.println("Saved offline");
}
    

    showingResult = true;
    resultTime = millis();

    appState = STATE_MEASURE_DONE;   // 🔥 BẮT BUỘC PHẢI CÓ
  }
}

// ====================== LCD ======================
void displayPatientList() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("LIST BENH NHAN:");

  if (patients.empty()) {
    lcd.setCursor(0, 1);
    lcd.print("Khong co benh nhan");
    lcd.setCursor(0, 2);
    lcd.print("Nhan OK de tai lai");
    return;
  }

  for (int i = 0; i < 3; i++) {
    int idx = pageStart + i;
    if (idx >= patients.size()) break;
    lcd.setCursor(0, i + 1);
    String prefix = (i + 1 == cursor ? "> " : "  ");
    lcd.print(prefix + patients[idx].name.substring(0, 17));
  }
}

void handleDFPlayer() {

  if (myDFPlayer.available()) {

    uint8_t type = myDFPlayer.readType();
    int value = myDFPlayer.read();

    switch (type) {

      case DFPlayerPlayFinished:
        Serial.print("✅ Finished track: ");
        Serial.println(value);

        audioState = AUDIO_IDLE;
        currentTrack = -1;
        break;

      case DFPlayerError:
  Serial.print("❌ DFPlayer error: ");
  Serial.println(value);

  myDFPlayer.stop();   // reset player
  audioState = AUDIO_IDLE;
  break;


      default:
        break;
    }
  }
}

// ====================== THÊM HÀM REFRESH DANH SÁCH ======================
void refreshPatientList() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Dang tai danh sach...");

  appState = STATE_FETCH_PATIENTS;
  fetchStarted = false;
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Dang khoi dong...");

  // DFPlayer
  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  if (!myDFPlayer.begin(dfSerial, true)) {
  Serial.println("❌ DFPlayer NOT FOUND");
} else {
  Serial.println("✅ DFPlayer OK");
  myDFPlayer.volume(25);
}
  myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);

delay(1000);
enqueueSound(SOUND_START_SETUP);
littleFS_OK = LittleFS.begin(true);

if (!littleFS_OK) {
  Serial.println("LittleFS lỗi!");
} else {
  loadPendingFromFS();   
}


// ===== WIFI =====
WiFi.mode(WIFI_STA);
WiFi.begin(ssid, password);
Serial.print("Connecting WiFi");

unsigned long start = millis();

while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
  delay(500);
  Serial.print(".");
}

if (WiFi.status() == WL_CONNECTED) {
  Serial.println("\n✅ WiFi connected");
  refreshPatientList();
} else {
  Serial.println("\n❌ WiFi timeout (offline mode)");
  displayPatientList(); 
}


// 🔥 LOCK CHANNEL
int ch = WiFi.channel();
esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

Serial.print("WiFi channel locked: ");
Serial.println(ch);

// ===== ESP-NOW =====
if (esp_now_init() != ESP_OK) {
  Serial.println("ESP-NOW init failed!");
  return;
}

esp_now_register_recv_cb(OnDataRecv);

esp_now_peer_info_t peerInfo = {};
memcpy(peerInfo.peer_addr, measureMac, 6);
peerInfo.channel = ch;   
peerInfo.encrypt = false;

esp_now_add_peer(&peerInfo);

}

// ====================== LOOP ======================
void loop() {

  if (audioState == AUDIO_PLAYING && millis() - audioStartTime > AUDIO_TIMEOUT) {
  Serial.println("⚠️ Audio timeout → reset");
  audioState = AUDIO_IDLE;
}
  
  switch (appState) {

    case STATE_FETCH_PATIENTS:
      handleFetchPatients();
      break;

    case STATE_MEASURE_DONE:
      appState = STATE_SYNC_PENDING;
      break;

    case STATE_SYNC_PENDING:
      handleSyncPending();
      break;

    case STATE_IDLE:
      break;
  }

  handleButtons();
handleDFPlayer();
processSoundQueue();
}