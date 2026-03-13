#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "HX711.h"
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- 1. WiFi and Firebase ---
#define WIFI_SSID "B4-15_2.4GHz"
#define WIFI_PASSWORD "appletv415"
#define API_KEY "AIzaSyDUBrzYarm-kq7x5r-En6VmZWk3Ia4qEcE"
#define FIREBASE_URL "smart-package-guard-default-rtdb.asia-southeast1.firebasedatabase.app"
#define BOT_TOKEN "8497710017:AAHrIXDJdlYNc1M_OtKpvDnbrzcxXC2V9r4"
#define CHAT_ID "8519935150"

// --- 2. Pin Config ---
#define RELAY_PIN D0
#define BUZZER_PIN D3         // ถ้า Upload ไม่เข้า ให้ถอดสายขานี้ออกก่อน
#define LOADCELL_DOUT_PIN D5
#define LOADCELL_SCK_PIN D6
#define VIB_SENSOR_PIN D8

// --- 3. Weight Config (Fixed as requested) ---
HX711 scale;
const float CALIBRATION_FACTOR = 1000.15; // ใช้ค่าคงที่ตามที่คุณกำหนด

LiquidCrystal_I2C lcd(0x27, 16, 2);
FirebaseData fbdo;
FirebaseData streamData;
FirebaseAuth auth;
FirebaseConfig config;

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);
String telegramChatId = CHAT_ID; 
unsigned long lastBotCheck = 0;
const unsigned long BOT_INTERVAL = 2000; 

// --- 4. Variables ---
unsigned long prevWeightMillis = 0;
bool isUnlocking = false;
unsigned long unlockStartTime = 0;
volatile bool newCommandReceived = false;
volatile bool targetLockState = true;

int vibeHitCount = 0;
unsigned long lastVibeTime = 0;
const int VIBE_HIT_THRESHOLD = 5;  // ลดจาก 20 เพื่อให้ตรวจจับง่ายขึ้น
const unsigned long VIB_COOLDOWN = 10000; // ป้องกันการแจ้งเตือนรัวข้ามกัน
unsigned long lastSirenTime = 0;
const unsigned long VIBE_TIME_WINDOW = 3000; // ขยายเวลาเป็น 3 วินาที


float lastWeightStable = 0.0;
int parcelCount = 0;
int triggerID = 0; 
bool scaleDetected = false; // Flag check hardware

const float STABILITY_MARGIN = 0.05;         
const unsigned long CHANGE_COOLDOWN = 4000;
unsigned long lastTriggerTime = 0;

// --- 5. Functions ---

void streamCallback(FirebaseStream data) {
  if (data.dataType() == "boolean") {
    targetLockState = data.boolData();
    newCommandReceived = true;
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout...");
}

void triggerCamera() {
  triggerID++; 
  Firebase.RTDB.setIntAsync(&fbdo, "/smartbox/camera/triggerID", triggerID);
  lcd.setCursor(0, 1);
  lcd.print("Capturing.....  "); 
  Serial.printf(">> CAMERA TRIGGERED: #%d\n", triggerID); 
}

void sirenAlarm() {
  if (millis() - lastSirenTime < VIB_COOLDOWN) return; // ติด Cooldown ค่อยแจ้งเตือนใหม่
  lastSirenTime = millis();

  Serial.println("!!! SIREN ACTIVATED !!!");
  
  // Send Telegram Alert if Chat ID is available
  if (telegramChatId != "") {
    Serial.print(">> Sending Vibration Alert to Telegram: " + telegramChatId + " ... ");
    if (bot.sendMessage(telegramChatId, "⚠️ Alert: Box vibration detected (Theft Attempt!)", "")) {
      Serial.println("Success!");
    } else {
      Serial.println("Failed!");
    }
  } else {
    Serial.println("!! Cannot send Telegram: No Chat ID. Please send /status to the bot first.");
  }

  // เสียงไซเรน
  for (int i = 0; i < 5; i++) { 
    Serial.println(">> Buzzer Sounding...");
    tone(BUZZER_PIN, 1500); 
    delay(300);
    yield(); 
    tone(BUZZER_PIN, 800);  
    delay(300);
    yield();
  }
  noTone(BUZZER_PIN);
}


void handleTelegramMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    if (telegramChatId == "" || telegramChatId != chat_id) {
      telegramChatId = chat_id;
      Firebase.RTDB.setStringAsync(&fbdo, "/smartbox/telegram/chatId", telegramChatId);
    }

    if (text == "/status") {
      String statusMsg = "📦 SmartBox Status Update\n";
      statusMsg += "--------------------------\n";
      
      if (scaleDetected) {
        float weight = scale.get_units(5);
        if (weight < 0) weight = 0.0;
        float weightKg = weight / 1000.0;
        statusMsg += "⚖️ Weight: " + String(weight, 1) + "g (" + String(weightKg, 2) + "kg)\n";
      } else {
        statusMsg += "⚖️ Weight: [Sensor Not Connected]\n";
      }
      
      statusMsg += "🔢 Parcels: " + String(parcelCount) + "\n";
      statusMsg += "🔒 Lock: " + String(targetLockState ? "LOCKED" : "UNLOCKED") + "\n";
      statusMsg += "⚙️ Config:\n";
      statusMsg += " - Calibration: " + String(CALIBRATION_FACTOR) + "\n";
      statusMsg += " - Trigger ID: " + String(triggerID) + "\n";
      statusMsg += " - Hardware: " + String(scaleDetected ? "Scale OK" : "Scale ERROR") + "\n";
      
      bot.sendMessage(chat_id, statusMsg, "");
    }
    
    if (text == "/start") {
      String welcome = "Welcome to SmartBox Bot, " + from_name + ".\n";
      welcome += "Use /status to get current data.";
      bot.sendMessage(chat_id, welcome, "");
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(VIB_SENSOR_PIN, INPUT);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.print("SYSTEM STARTING");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n>> WiFi Connected");

  // --- NTP Time Sync (Required for Telegram SSL on ESP8266) ---
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\n>> Time Synced");

  client.setInsecure(); // Moved up for stability
  client.setBufferSizes(1024, 1024); // Increase buffer slightly for Telegram headers
  client.setTimeout(10000); // 10 seconds timeout
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  if (scale.wait_ready_timeout(1000)) { // รอมันพร้อมแค่ 1 วินาที
    scale.set_scale(CALIBRATION_FACTOR);
    scale.tare(); 
    scaleDetected = true;
    Serial.println(">> Scale detected and tared.");
  } else {
    scaleDetected = false;
    Serial.println("!! Scale NOT detected. Running without weight features.");
  }

  config.api_key = API_KEY;
  config.database_url = FIREBASE_URL;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Sync Telegram Chat ID with Firebase
  if (Firebase.RTDB.getString(&fbdo, "/smartbox/telegram/chatId")) {
    if (fbdo.dataType() == "string" && fbdo.stringData() != "") {
      telegramChatId = fbdo.stringData();
      Serial.println(">> Recovered Chat ID: " + telegramChatId);
    } else {
      Firebase.RTDB.setStringAsync(&fbdo, "/smartbox/telegram/chatId", CHAT_ID);
      Serial.println(">> Setting default Chat ID to Firebase");
    }
  }

  if (Firebase.RTDB.beginStream(&streamData, "/smartbox/lock/status")) {
    Firebase.RTDB.setStreamCallback(&streamData, streamCallback, streamTimeoutCallback);
  }

  Firebase.RTDB.setIntAsync(&fbdo, "/smartbox/parcel/count", 0);
  Firebase.RTDB.setIntAsync(&fbdo, "/smartbox/camera/triggerID", 0);

  lcd.clear();
  lcd.print("SmartBox Online");

  // Send Online Message to Telegram with Debug
  if (telegramChatId != "") {
    delay(1000); // Wait a bit for system to settle
    Serial.print(">> Sending Welcome to Telegram... ");
    if (bot.sendMessage(telegramChatId, "✅ SmartBox System is Online!", "")) {
      Serial.println("Success!");
    } else {
      Serial.println("Failed!");
    }
  }
}

void loop() {
  // --- 1. Serial Command (Non-blocking) ---
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n'); 
    command.trim();
    if (command == "tare" && scaleDetected) {
      scale.tare();
      Serial.println(">> Tared.");
    } else if (command == "tare") {
      Serial.println("!! Cannot tare: Scale not detected.");
    }
  }

  // --- 2. Lock & Relay ---
  if (newCommandReceived) {
    newCommandReceived = false;
    if (targetLockState == false) {
      digitalWrite(RELAY_PIN, HIGH);
      isUnlocking = true;
      unlockStartTime = millis();
      FirebaseJson json;
      json.set("type", "remote_unlock");
      Firebase.RTDB.pushJSONAsync(&fbdo, "/smartbox/logs", &json);
    } else {
      digitalWrite(RELAY_PIN, LOW);
      isUnlocking = false;
    }
  }

  if (isUnlocking && (millis() - unlockStartTime >= 10000)) {
    digitalWrite(RELAY_PIN, LOW);
    isUnlocking = false;
    Firebase.RTDB.setBoolAsync(&fbdo, "/smartbox/lock/status", true);
  }

  // --- 3. Weight & Parcel Counting (Using Fixed Scale Logic) ---
  if (millis() - prevWeightMillis > 500) {
    prevWeightMillis = millis();
    
    if (scaleDetected && scale.is_ready()) {
      float weight = scale.get_units(5); 
      if (weight < 0) weight = 0.0;     
      float weightKg = weight / 1000.0;

      static float lastSentW = -1.0;
      if (abs(weightKg - lastSentW) > 0.01) {
          Firebase.RTDB.setFloatAsync(&fbdo, "/smartbox/weight/totalKg", weightKg);
          lastSentW = weightKg;
      }

      unsigned long now = millis();
      if (now - lastTriggerTime > CHANGE_COOLDOWN) {
        float delta = weight - lastWeightStable;
        if (abs(delta) >= 500.0) { 
          if (delta > 0) parcelCount++;
          else parcelCount = max(0, parcelCount - 1);
          
          lastWeightStable = weight;
          lastTriggerTime = now;
          Firebase.RTDB.setIntAsync(&fbdo, "/smartbox/parcel/count", parcelCount);
          triggerCamera();
        }
      }

      lcd.setCursor(0, 0);
      lcd.print("W:"); 
      if (weight < 1000) {
        lcd.print(weight, 1); lcd.print("g ");
      } else {
        lcd.print(weightKg, 2); lcd.print("kg ");
      }
      lcd.print("P:"); lcd.print(parcelCount); lcd.print("  ");
    } else if (!scaleDetected) {
      lcd.setCursor(0, 0);
      lcd.print("Weight: NO SENSOR");
    }
    
    lcd.setCursor(0, 1);
    lcd.print(isUnlocking ? ">> UNLOCKING << " : "Status: Locked  ");
  }

  // --- 4. Vibration & Siren ---
  // --- 4. Vibration & Siren ---
  int vibReading = digitalRead(VIB_SENSOR_PIN);
  if (vibReading == HIGH) { // **ถ้าเขย่าแล้วเงียบ ให้ลองเปลี่ยน HIGH เป็น LOW**
    unsigned long nowV = millis();
    if (nowV - lastVibeTime > VIBE_TIME_WINDOW) {
      vibeHitCount = 0;
    }
    
    vibeHitCount++;
    lastVibeTime = nowV;
    Serial.printf(">> Vibe Hit Detected! (Count: %d/%d)\n", vibeHitCount, VIBE_HIT_THRESHOLD);

    if (vibeHitCount >= VIBE_HIT_THRESHOLD) {
      vibeHitCount = 0; 
      sirenAlarm();
      triggerCamera();
      
      FirebaseJson json;
      json.set("type", "vibration_alert");
      json.set("timestamp", String(nowV));
      Firebase.RTDB.pushJSONAsync(&fbdo, "/smartbox/logs", &json);
    }
    delay(20); // หน่วงนิดเดียวเพื่อให้เก็บ pulse ได้ต่อเนื่อง
  }


  // --- 5. Telegram Bot ---
  if (millis() - lastBotCheck > BOT_INTERVAL) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleTelegramMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotCheck = millis();
  }
}

