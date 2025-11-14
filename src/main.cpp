#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BH1750.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ====== Temperature Sensor ======
#define ONE_WIRE_BUS 27
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature temp_sensors(&oneWire);

// ====== Water Level Sensor ======
#define LEVEL_PIN 32
int readWaterLevel() {
  int rawValue = analogRead(LEVEL_PIN);  // ESP32: 0~4095
  int levelPercentage = map(rawValue, 0, 4095, 0, 100);  // 0% ~ 100% transform
  return levelPercentage;
}

// ====== User Settings ======
const char* WIFI_SSID     = "USER SSID";
const char* WIFI_PASSWORD = "USER PW";

// ★ HTTPS EndPoint — CN/SAN / IP
const char* MOBIUS_URL = "https://userIP:443";

// oneM2M Resource
const char* CSEBASE   = "Mobius";
const char* AE_RN     = "AE-Sensor";
const char* CNT_LIGHT = "light";
const char* CNT_TEMP  = "temp";
const char* CNT_WLEVEL= "wlevel";

// upload period(ms)
const unsigned long UPLOAD_INTERVAL_MS = 30000;

// ====== Variable ======
BH1750 lightMeter;
unsigned long lastUpload = 0;
unsigned long reqId = 10000;
WiFiClientSecure secureClient;

// Certification
static const char root_ca_pem[] =R"EOF(
-----BEGIN CERTIFICATE-----
Local Certification by Mobius4 Server
-----END CERTIFICATE-----
)EOF";

// HTTPS URL
String url(const String& path) {
  String base = MOBIUS_URL;
  if (base.endsWith("/")) base.remove(base.length() - 1);
  return base + "/" + path;
}

// oneM2M common header
void setCommonHeaders(HTTPClient& http, bool isPost, int ty) {
  http.addHeader("Accept", "application/json");
  if (isPost) http.addHeader("Content-Type", "application/json; ty=" + String(ty));
  http.addHeader("X-M2M-Origin", "S-Sensor");
  http.addHeader("X-M2M-RI", String(reqId++));
  http.addHeader("X-M2M-RVI", "4");
}

// common CIN upload: Mobius/<AE>/<CNT> value transfer
bool createCINAt(const char* ae, const char* cnt, float value) {
  HTTPClient http;
  String target = url(String(CSEBASE) + "/" + String(ae) + "/" + String(cnt));
  Serial.printf("[CIN] target: %s\n", target.c_str());

  if (!http.begin(secureClient, target)) {
    Serial.println("[CIN] http.begin failed");
    return false;
  }

  setCommonHeaders(http, true, 4);

  // only contain data ({"m2m:cin":{"con":12.34}})
  String body = String("{\"m2m:cin\":{\"con\":") + String(value, 2) + "}}";

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("[CIN] HTTP %d value=%.2f\n", code, value);
  if (code == 201) return true;

  Serial.printf("[CIN] Resp: %s\n", resp.c_str());
  return false;
}

// NTP time sync
bool syncTimeWithNTP(uint32_t timeout_ms = 10000) {
  // KST=UTC+9.
  configTime(9 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  uint32_t start = millis();
  time_t now;
  do {
    delay(200);
    time(&now);
    if (now > 1700000000) {
      Serial.printf("Time synced: %ld\n", now);
      return true;
    }
  } while (millis() - start < timeout_ms);

  Serial.println("NTP sync timeout");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());

  // ★ TLS time sync
  syncTimeWithNTP();

  // ★ root CA setting
  secureClient.setCACert(root_ca_pem);

  // I2C + BH1750
  Wire.begin(); // SDA=21, SCL=22
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 init failed. Check wiring/address (0x23 or 0x5C).");
  } else {
    Serial.println("BH1750 OK");
  }

  // temp sensor
  temp_sensors.begin();

  // pin mapping for water level
  pinMode(LEVEL_PIN, INPUT);

}

void loop() {
  if (millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = millis();

    // 1) lux
    float lux = lightMeter.readLightLevel();
    if (lux < 0) {
      Serial.println("BH1750 read error");
    } else {
      if (!createCINAt(AE_RN, CNT_LIGHT, lux)) {
        Serial.println("CIN upload failed: light");
      }
    }

    // 2) temp
    temp_sensors.requestTemperatures();
    float tempC = temp_sensors.getTempCByIndex(0); // DS18B20
    if (tempC == DEVICE_DISCONNECTED_C) {
      Serial.println("Temp sensor read error");
    } else {
      if (!createCINAt(AE_RN, CNT_TEMP, tempC)) {
        Serial.println("CIN upload failed: temp");
      }
    }

    // 3) wlevel
    int wlevel = readWaterLevel(); // 0~4095
    if (!createCINAt(AE_RN, CNT_WLEVEL, (float)wlevel)) {
      Serial.println("CIN upload failed: wlevel");
    }
  }
}
