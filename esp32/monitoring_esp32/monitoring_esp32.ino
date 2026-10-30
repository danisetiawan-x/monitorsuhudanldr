/*
  Monitoring Suhu & Kelembapan + Kontrol Lampu/Kipas Manual/Otomatis (ESP32)

  Library (Arduino IDE > Library Manager):
    - PubSubClient (Nick O'Leary)
    - ArduinoJson (Benoit Blanchon) versi 7
    - DHT sensor library (Adafruit) + Adafruit Unified Sensor

  Mode:
    - OTOMATIS : lampu menyala saat gelap, kipas menyala saat suhu tinggi.
    - MANUAL   : lampu & kipas hanya mengikuti tombol ON/OFF di HP.
    Suhu, kelembapan, dan buzzer SELALU otomatis di kedua mode.
    Mode & keadaan terakhir disimpan di flash, jadi tetap sama setelah ESP32 restart.

  Perintah dari HP (topik kontrol, format JSON). HP selalu mengirim keadaan
  yang diinginkan (true = nyala, false = mati), bukan perintah "balikkan":
    {"mode":"manual"}  {"mode":"otomatis"}  {"lampu":true}  {"kipas":false}
    {"mode":"manual","lampu":true}   <- tombol ON ditekan saat mode otomatis

  ===== JIKA LAMPU/KIPAS TERBALIK (ON jadi mati, OFF jadi nyala) =====
  Ubah LAMPU_AKTIF_LOW / KIPAS_AKTIF_LOW di bawah (true <-> false), lalu upload ulang.
  Cara cek: buka Serial Monitor (115200). Setiap perubahan dicetak, misalnya
  "Lampu -> NYALA (pin 26 = LOW)". Jika tulisan NYALA tetapi lampu mati,
  balik nilai pengaturannya. Penyebab umum:
    - modul relay jenis aktif HIGH (ada jumper H/L di modulnya), atau
    - kabel lampu dipasang di terminal NC; seharusnya di COM + NO.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DHT.h>

// ===== Isi sesuai milikmu =====
const char* WIFI_SSID = "NAMA_WIFI";
const char* WIFI_PASS = "PASSWORD_WIFI";

const char* MQTT_HOST = "i317fd82.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT = 8883;                // MQTT over TLS
const char* MQTT_USER = "USER_ESP32";        // pakai akun berbeda dari akun web
const char* MQTT_PASS = "PASSWORD_ESP32";

const char* TOPIK_DATA    = "alfian-x7k2/monitoring/data";
const char* TOPIK_STATUS  = "alfian-x7k2/monitoring/status";
const char* TOPIK_KONTROL = "alfian-x7k2/monitoring/kontrol";

// Pin (sesuaikan dengan rangkaianmu)
#define PIN_DHT     4
#define TIPE_DHT    DHT22        // ganti DHT11 jika pakai DHT11
#define PIN_LDR     34           // pin DO modul LDR
#define PIN_LAMPU   26           // relay lampu
#define PIN_KIPAS   27           // relay kipas
#define PIN_BUZZER  25

const bool LDR_GELAP_HIGH = true;    // kebanyakan modul LDR: DO = HIGH saat gelap

// Logika relay, diatur terpisah untuk tiap alat.
// true  = relay menyala saat pin LOW  (aktif LOW)
// false = relay menyala saat pin HIGH (aktif HIGH)
// Lampu sebelumnya terbalik dengan aktif LOW, jadi sekarang diatur aktif HIGH.
const bool LAMPU_AKTIF_LOW = false;
const bool KIPAS_AKTIF_LOW = true;

// Batas otomatis
const float SUHU_KIPAS_ON  = 30.0;   // kipas nyala di atas ini (mode otomatis)
const float SUHU_KIPAS_OFF = 29.0;   // kipas mati di bawah ini (jeda agar tidak kedip)
const float SUHU_BUZZER    = 35.0;   // buzzer bunyi di atas ini (selalu otomatis)

const unsigned long INTERVAL_KIRIM = 2000;  // ms
// ==============================

WiFiClientSecure net;
PubSubClient mqtt(net);
DHT dht(PIN_DHT, TIPE_DHT);
Preferences pref;

bool modeManual = false;
bool lampuOn = false, kipasOn = false, buzzerOn = false;
float suhu = NAN, lembab = NAN;
bool gelap = false;
bool perluKirim = true;
unsigned long terakhirKirim = 0;

// Level pin untuk keadaan nyala/mati, sesuai logika relay masing-masing
int levelRelay(bool nyala, bool aktifLow) {
  if (aktifLow) return nyala ? LOW : HIGH;
  return nyala ? HIGH : LOW;
}

void tulisRelay(const char* nama, int pin, bool aktifLow, bool nyala) {
  int level = levelRelay(nyala, aktifLow);
  if (digitalRead(pin) != level) {
    Serial.printf("%s -> %s (pin %d = %s)\n", nama, nyala ? "NYALA" : "MATI", pin, level == HIGH ? "HIGH" : "LOW");
  }
  digitalWrite(pin, level);
}

void terapkanOutput() {
  tulisRelay("Lampu", PIN_LAMPU, LAMPU_AKTIF_LOW, lampuOn);
  tulisRelay("Kipas", PIN_KIPAS, KIPAS_AKTIF_LOW, kipasOn);
  digitalWrite(PIN_BUZZER, buzzerOn ? HIGH : LOW);
}

void simpanPengaturan() {
  // Hanya tulis jika berubah, agar flash tidak cepat aus
  if (pref.getBool("manual", false) != modeManual) pref.putBool("manual", modeManual);
  if (pref.getBool("lampu", false) != lampuOn)     pref.putBool("lampu", lampuOn);
  if (pref.getBool("kipas", false) != kipasOn)     pref.putBool("kipas", kipasOn);
}

void bacaSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) suhu = t;
  if (!isnan(h)) lembab = h;
  gelap = (digitalRead(PIN_LDR) == HIGH) == LDR_GELAP_HIGH;
}

void logikaOtomatis() {
  if (!modeManual) {
    lampuOn = gelap;
    if (!isnan(suhu)) {
      if (suhu >= SUHU_KIPAS_ON) kipasOn = true;
      else if (suhu <= SUHU_KIPAS_OFF) kipasOn = false;
    }
  }
  // Buzzer selalu otomatis
  buzzerOn = !isnan(suhu) && suhu >= SUHU_BUZZER;
}

void kirimData() {
  if (isnan(suhu) || isnan(lembab)) return;  // dashboard butuh angka valid
  JsonDocument doc;
  doc["suhu"]   = suhu;
  doc["lembab"] = lembab;
  doc["gelap"]  = gelap;
  doc["lampu"]  = lampuOn;
  doc["kipas"]  = kipasOn;
  doc["buzzer"] = buzzerOn;
  doc["mode"]   = modeManual ? "manual" : "otomatis";
  char buf[200];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(TOPIK_DATA, (const uint8_t*)buf, n, true);  // retained: HP langsung dapat data terakhir
}

void terimaPesan(char* topik, byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;  // abaikan pesan rusak

  if (doc["mode"].is<const char*>()) {
    String m = doc["mode"].as<String>();
    if (m == "manual")   modeManual = true;
    if (m == "otomatis") modeManual = false;
  }
  // Perintah lampu/kipas hanya berlaku di mode manual.
  // Nilainya langsung dipakai sebagai keadaan (true = nyala), tidak dibalik.
  if (modeManual) {
    if (doc["lampu"].is<bool>()) lampuOn = doc["lampu"].as<bool>();
    if (doc["kipas"].is<bool>()) kipasOn = doc["kipas"].as<bool>();
  }
  logikaOtomatis();
  terapkanOutput();
  simpanPengaturan();
  perluKirim = true;  // konfirmasi ke HP secepatnya
}

void sambungWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(" tersambung");
}

void sambungMQTT() {
  while (!mqtt.connected()) {
    String id = "esp32-monitoring-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    // Last Will: broker mengirim "offline" otomatis jika ESP32 mati/putus
    if (mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, TOPIK_STATUS, 1, true, "offline")) {
      Serial.println("MQTT tersambung");
      mqtt.publish(TOPIK_STATUS, "online", true);
      mqtt.subscribe(TOPIK_KONTROL, 1);
      perluKirim = true;
    } else {
      Serial.printf("MQTT gagal (%d), coba lagi...\n", mqtt.state());
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  pref.begin("monitoring", false);
  modeManual = pref.getBool("manual", false);
  lampuOn    = pref.getBool("lampu", false);
  kipasOn    = pref.getBool("kipas", false);

  // Atur level pin dulu sebelum jadi OUTPUT, agar relay tidak sempat berkedip saat menyala
  digitalWrite(PIN_LAMPU, levelRelay(lampuOn, LAMPU_AKTIF_LOW));
  digitalWrite(PIN_KIPAS, levelRelay(kipasOn, KIPAS_AKTIF_LOW));
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LAMPU, OUTPUT);
  pinMode(PIN_KIPAS, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LDR, INPUT);
  dht.begin();
  terapkanOutput();

  sambungWiFi();
  net.setInsecure();  // TLS tanpa verifikasi sertifikat; untuk lebih aman pakai net.setCACert(...) dari EMQX
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(terimaPesan);
  mqtt.setBufferSize(512);
}

void loop() {
  sambungWiFi();
  sambungMQTT();
  mqtt.loop();

  if (perluKirim || millis() - terakhirKirim >= INTERVAL_KIRIM) {
    bacaSensor();
    logikaOtomatis();
    terapkanOutput();
    kirimData();
    terakhirKirim = millis();
    perluKirim = false;
  }
}
