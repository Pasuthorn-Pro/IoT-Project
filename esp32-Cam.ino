#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Firebase_ESP_Client.h>

// --- Firebase Addons ---
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// 1. WiFi Settings
#define WIFI_SSID "B4-15_2.4GHz"
#define WIFI_PASSWORD "appletv415"

// 2. ImgBB API Key
String IMGBB_API_KEY = "4d8eb22f918bf309a4b0d713c0f1e0fc"; 

// 3. Firebase Settings
#define API_KEY "AIzaSyDUBrzYarm-kq7x5r-En6VmZWk3Ia4qEcE" 
#define FIREBASE_URL "smart-package-guard-default-rtdb.asia-southeast1.firebasedatabase.app"

// 4. AI Thinker Camera Pins
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

FirebaseData fbdo;
FirebaseData streamData;
FirebaseAuth auth;
FirebaseConfig config;

volatile bool captureRequested = false;
int lastTriggerID = 0;

void startCamera() {
  camera_config_t configCam;
  configCam.ledc_channel = LEDC_CHANNEL_0;
  configCam.ledc_timer   = LEDC_TIMER_0;
  configCam.pin_d0 = Y2_GPIO_NUM;
  configCam.pin_d1 = Y3_GPIO_NUM;
  configCam.pin_d2 = Y4_GPIO_NUM;
  configCam.pin_d3 = Y5_GPIO_NUM;
  configCam.pin_d4 = Y6_GPIO_NUM;
  configCam.pin_d5 = Y7_GPIO_NUM;
  configCam.pin_d6 = Y8_GPIO_NUM;
  configCam.pin_d7 = Y9_GPIO_NUM;
  configCam.pin_xclk = XCLK_GPIO_NUM;
  configCam.pin_pclk = PCLK_GPIO_NUM;
  configCam.pin_vsync = VSYNC_GPIO_NUM;
  configCam.pin_href = HREF_GPIO_NUM;
  configCam.pin_sscb_sda = SIOD_GPIO_NUM;
  configCam.pin_sscb_scl = SIOC_GPIO_NUM;
  configCam.pin_pwdn = PWDN_GPIO_NUM;
  configCam.pin_reset = RESET_GPIO_NUM;
  configCam.xclk_freq_hz = 20000000;
  configCam.pixel_format = PIXFORMAT_JPEG;

  if(psramFound()){
    configCam.frame_size = FRAMESIZE_VGA;
    configCam.jpeg_quality = 10;
    configCam.fb_count = 2;
  } else {
    configCam.frame_size = FRAMESIZE_SVGA;
    configCam.jpeg_quality = 12;
    configCam.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&configCam);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
}

bool uploadToImgBB(camera_fb_t *fb, String &returnedUrl) {
  WiFiClientSecure client;
  client.setInsecure();

  Serial.println("Connecting to ImgBB API...");
  if (!client.connect("api.imgbb.com", 443)) {
    Serial.println("Connection failed!");
    return false;
  }

  String head = "--MyBoundary\r\nContent-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--MyBoundary--\r\n";
  uint32_t totalLen = head.length() + fb->len + tail.length();

  client.println("POST /1/upload?key=" + IMGBB_API_KEY + " HTTP/1.1");
  client.println("Host: api.imgbb.com");
  client.println("Content-Length: " + String(totalLen));
  client.println("Content-Type: multipart/form-data; boundary=MyBoundary");
  client.println();
  client.print(head);

  uint8_t *fbBuf = fb->buf;
  size_t fbLen = fb->len;
  for (size_t n = 0; n < fbLen; n = n + 1024) {
    size_t chunk = (n + 1024 < fbLen) ? 1024 : (fbLen - n);
    client.write(fbBuf + n, chunk);
  }
  client.print(tail);

  String response = "";
  unsigned long startTime = millis();
  while (millis() - startTime < 10000) { 
    while (client.available()) {
      char c = client.read();
      response += c;
    }
    if (response.indexOf("}") != -1) break;
    delay(10);
  }
  client.stop();

  int urlStart = response.indexOf("\"display_url\":\"");
  if (urlStart != -1) {
    urlStart += 15;
    int urlEnd = response.indexOf("\"", urlStart);
    returnedUrl = response.substring(urlStart, urlEnd);
    returnedUrl.replace("\\/", "/");
    return true;
  }
  
  Serial.println("Upload failed. Response: " + response);
  return false;
}

void captureAndUpload() {
  Serial.println("Taking photo...");
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  String imageUrl = "";
  if (uploadToImgBB(fb, imageUrl)) {
    Serial.printf("Upload success! URL: %s\n", imageUrl.c_str());
    Firebase.RTDB.setStringAsync(&fbdo, "/smartbox/camera/latest_imageUrl", imageUrl);

    FirebaseJson json;
    json.set("type", "camera_captured");
    json.set("timestamp", millis());
    Firebase.RTDB.pushJSONAsync(&fbdo, "/smartbox/logs", &json);
  }

  esp_camera_fb_return(fb);
  // ไม่ต้องสั่ง Reset ค่า triggerID เพราะเราใช้การเช็คเลขที่เปลี่ยนไป
}

void streamCallback(FirebaseStream data) {
  if (data.dataType() == "int") {
    int currentID = data.intData();
    // ถ้าเลข ID ใหม่มากกว่าเลขล่าสุด แสดงว่ามีคำสั่งถ่ายรูปใหม่
    if (currentID > lastTriggerID) {
      lastTriggerID = currentID;
      captureRequested = true;
      Serial.printf("New Trigger ID received: %d\n", currentID);
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout...");
}

void setup() {
  Serial.begin(115200);
  startCamera();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected");

  config.api_key = API_KEY;
  config.database_url = FIREBASE_URL;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.RTDB.beginStream(&streamData, "/smartbox/camera/triggerID")) {
    Firebase.RTDB.setStreamCallback(&streamData, streamCallback, streamTimeoutCallback);
  }
}

void loop() {
  if (captureRequested) {
    captureRequested = false;
    captureAndUpload();
  }
}
