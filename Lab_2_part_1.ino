#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Arduino_JSON.h>
#include <BluetoothSerial.h>

// ---- Wi-Fi ----
const char* ssid     = "OGsMobile";
const char* password = "Aout0014";

// ---- API ----
const char* basePrefURL = "https://iotjukebox.onrender.com/preference";
const char* baseSongURL = "https://iotjukebox.onrender.com/song";

// ---- IDs / BT ----
BluetoothSerial SerialBT;
String studentID = "40226489";
#define BT_DISCOVER_TIME 10000
#define BUZZER_PIN 21

// timing
unsigned long lastTime   = 0;
unsigned long timerDelay = 15000;

// ---- fwd decl ----
void connectToWiFi();
void scanBluetooth();
void querySong(const String& deviceName);
bool getSongData(const String& songName, int& tempoOut, int* melodyOut, int& noteCountOut);
void playSong(const int* melody /*pairs: freq,beat*/, int notePairs, int tempo);
String httpGETRequest(const String& url);
int httpPOSTPreference(const String& id, const String& key, const String& value);

static String urlEncode(const String& s) {
  String out; out.reserve(s.length()*3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i=0;i<s.length();++i) {
    uint8_t c = (uint8_t)s[i];
    bool unreserved = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~';
    if (unreserved) out += char(c);
    else {
      out += '%'; out += hex[(c>>4)&0xF]; out += hex[c&0xF];
    }
  }
  return out;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  SerialBT.begin("ESP32_Jukebox");
  Serial.println("Bluetooth initialized!");

  connectToWiFi();
}

void loop() {
  if ((millis() - lastTime) > timerDelay) {
    scanBluetooth();
    lastTime = millis();
  }
}

// ---- Wi-Fi ----
void connectToWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.printf("\nConnected to WiFi! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ---- Bluetooth scan ----
void scanBluetooth() {
  Serial.println("\nScanning for Bluetooth devices...");
  BTScanResults* results = SerialBT.discover(BT_DISCOVER_TIME);

  if (!results) { Serial.println("No Bluetooth devices found!"); return; }

  int count = results->getCount();
  Serial.printf("Found %d devices\n", count);

  for (int i = 0; i < count; i++) {
    BTAdvertisedDevice* d = results->getDevice(i);
    if (!d) continue;
    if (d->haveName()) {
      String raw = String(d->getName().c_str());
      Serial.print("Found device: "); Serial.println(raw);

      // normalize to your saved key(s) if needed
      if (raw == "AirPods") raw = "Sassans AirPods";

      querySong(raw); // try to resolve a song for any seen device
    }
  }
}

void querySong(const String& deviceName) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected."); return; }

  String keyEnc = urlEncode(deviceName);
  String idEnc  = urlEncode(studentID);

  String url = String(basePrefURL) + "?id=" + idEnc + "&key=" + keyEnc;
  Serial.println("Requesting from: " + url);

  String response = httpGETRequest(url);
  Serial.println("Raw response: " + response);

  JSONVar json = JSON.parse(response);
  if (JSON.typeof(json) == "undefined") {
    Serial.println("Failed to parse preference JSON.");
    return;
  }

  if (!json.hasOwnProperty("name")) {
    Serial.println("No 'name' in preference response.");
    return;
  }

  String songName = (const char*) json["name"];
  Serial.println("Song preference for " + deviceName + ": " + songName);

  int tempo=0; int notePairs=0;
  // melodyPairs: freq,beat,freq,beat,...
  int melodyPairs[256]; // up to 128 pairs (fits lab API)
  if (getSongData(songName, tempo, melodyPairs, notePairs)) {
    playSong(melodyPairs, notePairs, tempo);
  }
}

bool getSongData(const String& songName, int& tempoOut, int* melodyOut, int& noteCountOut) {
  tempoOut = 0; noteCountOut = 0;

  String nameEnc = urlEncode(songName);
  String url = String(baseSongURL) + "?name=" + nameEnc;
  Serial.println("Fetching song data from: " + url);

  String response = httpGETRequest(url);
  if (response.length() == 0) { Serial.println("Empty /song response."); return false; }

  JSONVar json = JSON.parse(response);
  if (JSON.typeof(json) == "undefined") { Serial.println("Failed to parse song JSON."); return false; }

  // tempo can arrive as string or number in this API; coerce to int
  if (json.hasOwnProperty("tempo")) {
    const char* tstr = (const char*)json["tempo"];
    tempoOut = tstr ? atoi(tstr) : (int) json["tempo"];
  }
  if (tempoOut <= 0) tempoOut = 120;

  if (!json.hasOwnProperty("melody")) { Serial.println("No melody field."); return false; }

  JSONVar mel = json["melody"];
  int L = mel.length();
  if (L < 2) { Serial.println("Melody too short."); return false; }

  // Interpret as pairs
  int pairs = 0;
  for (int i = 0; i+1 < L && pairs < 128; i += 2) {
    int f = atoi((const char*) mel[i]);
    int b = atoi((const char*) mel[i+1]);
    melodyOut[2*pairs]   = f;
    melodyOut[2*pairs+1] = b;
    pairs++;
  }
  noteCountOut = pairs;
  Serial.printf("Parsed %d note pairs, tempo=%d\n", pairs, tempoOut);
  return pairs > 0;
}

// ---- Buzzer playback using tempo + (freq,beat) ----
void playSong(const int* melody, int notePairs, int tempo) {
  const int qn = 60000 / tempo; // ms per quarter note
  for (int i = 0; i < notePairs; ++i) {
    int freq = melody[2*i];
    int beat = melody[2*i+1];             // e.g., 4=quarter, 8=eighth, etc.
    int dur  = abs(beat) * qn / 4;        // scale to quarter note base

    if (freq > 0) tone(BUZZER_PIN, freq, dur);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)(dur * 1.3)) { delay(2); }
    noTone(BUZZER_PIN);
  }
}

// ---- GET helper (HTTPS, no cert) ----
String httpGETRequest(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(10000);
  if (!http.begin(client, url)) { http.end(); return String(); }

  int code = http.GET();
  String payload;
  if (code > 0) {
    Serial.print("HTTP code: "); Serial.println(code);
    payload = http.getString();
  } else {
    Serial.print("HTTP error: "); Serial.println(code);
  }
  http.end();
  return payload;
}
