#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
// bluetooth low energy libs
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Identification
#define BUZZER_PIN 21
const char* WifiSsid      = "";
const char* WifiPassword  = "";

static const char* PreferenceKey = "";

// API HTTPS 
const char* BASE_SONG_URL = "https://iotjukebox.onrender.com/song";

// Queue
const char* SongList[] = { "harrypotter", "jigglypuffsong", "tetris", "gameofthrones" };
#define QUEUE_SIZE     5
#define MELODY_MAX     128

struct MelodyData {
  int Frequencies[MELODY_MAX];
  int Durations[MELODY_MAX];
  int Tempo;
  int NoteCount;
};

enum PlaybackActionType {// features
  PlaybackNone = 0,
  PlaybackToggle,
  PlaybackNext,
  PlaybackPrevious
};

// initial settings
String playlist[QUEUE_SIZE];
int QueueStartIndex = 0;
int QueueItemCount  = 0;
int CurrentQueuePosition = 0;

// default
MelodyData activeSong;
int  LastLoadedIndex = -1;
bool SongIsLoaded    = false;
bool IsPlaying       = true;

static const char* BLE_DEVICE_NAME = "ESP32_Jukebox_BLE";
static BLEServer* pServer = nullptr;
static BLECharacteristic* pTxChar = nullptr; // write to phone
static BLECharacteristic* pRxChar = nullptr; // phone writes to terminal

// UUIDs for ble
static BLEUUID UUID_SERVICE("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID UUID_RX     ("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"); // Write
static BLEUUID UUID_TX     ("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"); // Notify

// RX buffer from BLE writes
String g_rxBuffer;

String GetCurrentSongName() {
  if (QueueItemCount == 0) return "harrypotter"; // first song
  return playlist[(QueueStartIndex + CurrentQueuePosition) % QueueItemCount];
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT); // breadboard stuff

  InitializePlaylist();
  ConnectWiFi();
  InitBLE();

  Serial.println("Ready (BLE + Wi-Fi). Send P / N / B from iPhone app.");
}

void loop() {
  // Pull immediate commands from BLE buffer when idle too
  if (g_rxBuffer.length() > 0) {
    char cmd = g_rxBuffer[0];
    g_rxBuffer.remove(0, 1);
    if (cmd == 'P') TogglePlayPause(); // pause feature
    else if (cmd == 'N') NextTrack(); // next feature
    else if (cmd == 'B') PrevTrack(); // previous feature 
  }

  if (IsPlaying && WiFi.status() == WL_CONNECTED) { // connection 
    if (!SongIsLoaded || LastLoadedIndex != CurrentQueuePosition) {
      Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
      SongIsLoaded    = LoadSongByQueueIndex(CurrentQueuePosition, activeSong);
      LastLoadedIndex = SongIsLoaded ? CurrentQueuePosition : -1;
    }
    if (SongIsLoaded) {
      PlaybackActionType act = PlaySongAndListenControls(activeSong);
      if      (act == PlaybackToggle)   TogglePlayPause();
      else if (act == PlaybackNext)     NextTrack();
      else if (act == PlaybackPrevious) PrevTrack();
      else { // by default
        NextTrack();
      }
    }
  }

  delay(10); // BLE + WiFi friendly
}


void InitializePlaylist() {
  const int baseCount = sizeof(SongList) / sizeof(SongList[0]); // which song to play based on the queue status
  for (int i = 0; i < baseCount && i < QUEUE_SIZE; ++i) { // all the songs are stored and ready to play
    playlist[i] = SongList[i];
  }
  QueueItemCount = baseCount;
  CurrentQueuePosition = 0;
}

void TogglePlayPause() {
  IsPlaying = !IsPlaying;
  Serial.printf("Song: %s\n", IsPlaying ? "Toggled off" : "Toggled on");
  if (pTxChar) {
    String s = String("Play: ") + (IsPlaying ? "is playing" : "not playing");
    pTxChar->setValue((uint8_t*)s.c_str(), s.length());
    pTxChar->notify();
  }
}

void NextTrack() {
  CurrentQueuePosition = (CurrentQueuePosition + 1) % QueueItemCount; // moves up the queue
  SongIsLoaded = false;
  Serial.printf("Next -> %s\n", GetCurrentSongName().c_str());
  if (pTxChar) {
    String s = String("Next: ") + GetCurrentSongName(); // retrieve song
    pTxChar->setValue((uint8_t*)s.c_str(), s.length());
    pTxChar->notify();
  }
}

void PrevTrack() {
  CurrentQueuePosition = (CurrentQueuePosition - 1 + QueueItemCount) % QueueItemCount; // moves down the queue
  SongIsLoaded = false;
  Serial.printf("Prev -> %s\n", GetCurrentSongName().c_str());
  if (pTxChar) {
    String s = String("Prev: ") + GetCurrentSongName();// retrieve song
    pTxChar->setValue((uint8_t*)s.c_str(), s.length());
    pTxChar->notify();
  }
}

bool getHTTPRequest(const String& url, String& out, uint32_t timeoutMs = 10000) {
  WiFiClientSecure client;
  client.setInsecure();        // skip ssl cert      
  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  if (!http.begin(client, url)) {
    http.end();
    return false;
  }
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    out = http.getString();
    http.end();
    return true;
  }
  Serial.printf("HTTP GET failed: %d\n", code);
  http.end();
  return false;
}

bool ParseMelodyFromJson(const String& jsonPayload, MelodyData& out) {
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, jsonPayload);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    out.NoteCount = 0;
    return false;
  }
  // tempo can be string or number
  out.Tempo = doc["tempo"].as<int>();
  if (out.Tempo <= 0) out.Tempo = 120;

  JsonArray mel = doc["melody"].as<JsonArray>();
  int count = 0;
  // Expect melody as beats
  for (size_t i = 0; i + 1 < mel.size() && count < MELODY_MAX; i += 2) {
    out.Frequencies[count] = mel[i].as<int>();
    out.Durations[count]   = mel[i + 1].as<int>();
    ++count;
  }
  out.NoteCount = count;
  return (count > 0);
}

bool LoadSongByQueueIndex(int index, MelodyData& out) {
  const String name = GetCurrentSongName();
  String url = String(BASE_SONG_URL) + "?name=" + name;
  Serial.println("GET " + url);
  String payload;
  if (!getHTTPRequest(url, payload)) { // not able to retrieve song 
    Serial.println("HTTP failed");
    return false;
  }
  if (!ParseMelodyFromJson(payload, out)) {
    Serial.println("Song JSON invalid");
    return false;
  }
  return true;
}

// playback function to write to
PlaybackActionType PlaySongAndListenControls(MelodyData& song) {
  if (song.NoteCount <= 0 || song.Tempo <= 0) return PlaybackNone;

  int qn = 60000 / song.Tempo; // quarter-note ms
  Serial.printf("Playing '%s' (tempo=%d, notes=%d)\n",
                GetCurrentSongName().c_str(), song.Tempo, song.NoteCount);

  // optional notify
  if (pTxChar) {
    String s = String("Playing: ") + GetCurrentSongName();
    pTxChar->setValue((uint8_t*)s.c_str(), s.length());
    pTxChar->notify();
  }

  for (int i = 0; i < song.NoteCount; ++i) {
    int freq = song.Frequencies[i];
    int beat = song.Durations[i];
    int dur  = abs(beat) * qn / 4; // beat in quarter-note units

    if (freq > 0) tone(BUZZER_PIN, freq, dur);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)(dur * 1.3)) {
      // Check BLE RX buffer (commands)
      if (g_rxBuffer.length() > 0) {
        // read 1st char and consume line
        char cmd = g_rxBuffer[0];
        g_rxBuffer.remove(0, 1);  // consume single-char command

        if (cmd == 'P') { noTone(BUZZER_PIN); return PlaybackToggle; }
        if (cmd == 'N') { noTone(BUZZER_PIN); return PlaybackNext; }
        if (cmd == 'B') { noTone(BUZZER_PIN); return PlaybackPrevious; }
      }
      delay(5);
    }
    noTone(BUZZER_PIN);
  }
  return PlaybackNone;
}

// ble function to read from
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    if (v.length() == 0) return;
    // Append to buffer 
    for (uint8_t ch : v) {
      if (ch == '\r') continue;
      // accept first byte for simple commands; 
      g_rxBuffer += (char)ch;
    }
    Serial.print("BLE RX: ");
    for (size_t i = 0; i < v.length(); ++i) Serial.print((char)v[i]);
    Serial.println();

    // optional feedback
    if (pTxChar) {
      String s = "RX:" + String(v.c_str());
      pTxChar->setValue((uint8_t*)s.c_str(), s.length());
      pTxChar->notify();
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    Serial.println("BLE connected");
  }
  void onDisconnect(BLEServer* s) override {
    Serial.println("BLE disconnected, advertising...");
    s->getAdvertising()->start();
  }
};

void InitBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(UUID_SERVICE);

  // TX: notify (ESP32 -> phone)
  pTxChar = pService->createCharacteristic(
    UUID_TX, BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxChar->addDescriptor(new BLE2902());

  // RX: write (phone -> ESP32)
  pRxChar = pService->createCharacteristic(
    UUID_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(UUID_SERVICE);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  pAdv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("BLE UART ready. Open Terminal, write P/N/B to RX."); // part 2 task
}

// Wi-Fi
void ConnectWiFi() {
  Serial.printf("Connecting to %s", WifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WifiSsid, WifiPassword);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi Connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi FAILED (continuing; BLE still works).");
  }
}