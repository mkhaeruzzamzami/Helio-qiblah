/* === HELIO QIBLAH NAVIGATOR - KODE MASTER ESP32 (AUTO-ZERO MPU6050 + AUDIO QUEUE) === */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <FirebaseESP32.h>
#include <TinyGPS++.h> 
#include <Adafruit_Sensor.h>
#include <Adafruit_MPU6050.h>
#include <DFRobotDFPlayerMini.h> 
#include <SoftwareSerial.h> 

// --- PENGATURAN MODE ONLINE / OFFLINE ---
#define ENABLE_WIFI true
#define WIFI_TIMEOUT_MS 5000           // Dipercepat jadi 5 detik agar inisiasi lebih sigap

// --- KONFIGURASI KREDENSIAL ---
#define WIFI_SSID "miskin"
#define WIFI_PASSWORD "00000000"
#define FIREBASE_HOST "helioqiblah-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "kbJ0Ta1aOQ7w2bBcyvTaiXOvHatzslzqhbRGBHPb"

// --- KONFIGURASI PIN ---
#define RX_GPS 16 
#define TX_GPS 17 
#define RX_UNO 32
#define TX_UNO 33
#define RX_DFP 14 
#define TX_DFP 13 
#define BUSY_PIN 25 
#define I2C1_SDA 21
#define I2C1_SCL 22

// --- ALAMAT I2C KOMPAS QMC5883P ---
#define QMC5883P_ADDR 0x2C

// --- PARAMETER KALKULASI & TOLERANSI ---
const float KAABAH_LAT = 21.422487;
const float KAABAH_LNG = 39.826206;
const float DEKLINASI_MAGNETIK = 0.8;  // Koreksi True North
const float TOLERANSI_TEPAT = 1.5;     // Batas mengunci arah kiblat (derajat)
const float TOLERANSI_HAMPIR = 5.0;    // Batas hampir tepat (derajat)
const float BATAS_GESER = 5.0;         // Jika tergeser > 5 derajat, otomatis navigasi ulang
const float BATAS_MIRING = 15.0;       // Batas kemiringan relatif dari posisi datar awal

const unsigned long INTERVAL_KIRIM_UNO = 3000;
const unsigned long INTERVAL_NAVIGASI = 250;
const unsigned long AUDIO_COOLDOWN = 4500;
const float ALPHA_SMOOTH = 0.2;

// --- OBJEK GLOBAL ---
HardwareSerial GPS_Serial(2); 
HardwareSerial UNO_Serial(1); 
SoftwareSerial DFP_Serial(RX_DFP, TX_DFP); 

TinyGPSPlus gps;
Adafruit_MPU6050 mpu;
DFRobotDFPlayerMini myDFPlayer;
Preferences prefs;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// --- VARIABEL STATUS ---
bool isOnline = false;
bool mpuOK = false;
bool koordinatTerkirim = false;
bool navigasiAktif = false;
bool targetTerkunci = false;

float targetKiblat = 0;
float currentHeading = 0;
float smoothSin = 0, smoothCos = 1;

// Variabel Auto-Zero (Tare) MPU6050
float baseRoll = 0, basePitch = 0;
float relRoll = 0, relPitch = 0;

float magOffsetX = 0, magOffsetY = 0, magOffsetZ = 0;

// --- VARIABEL ANTREAN AUDIO ---
int nextAudioTrack = 0;
unsigned long lastAudioTime = 0;
unsigned long lastKirimUno = 0;
unsigned long lastNavUpdate = 0;
unsigned long lastSatUpdate = 0;
String lastStatusLCD = "";

// --- NORMALISASI SELISIH SUDUT (-180 s/d +180) ---
float normalizeAngleDiff(float diff) {
  while (diff > 180.0) diff -= 360.0;
  while (diff < -180.0) diff += 360.0;
  return diff;
}

// --- AUTO-ZERO (TARE) MPU6050 SAAT BOOTING ---
void calibrateMPUZero() {
  float sumRoll = 0, sumPitch = 0;
  const int samples = 10;
  for (int i = 0; i < samples; i++) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    sumRoll  += atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
    sumPitch += atan2(-a.acceleration.x, sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 180.0 / PI;
    delay(20);
  }
  baseRoll  = sumRoll / samples;
  basePitch = sumPitch / samples;
  Serial.printf("MPU6050 Auto-Zero OK | Base Roll: %.1f° | Base Pitch: %.1f°\n", baseRoll, basePitch);
}

// --- FUNGSI AUDIO & ANTREAN NON-BLOCKING ---
void playAudio(int trackNumber, bool forceInterrupt = false, int queuedNext = 0) {
  bool isBusy = (digitalRead(BUSY_PIN) == LOW);
  if (forceInterrupt) {
    myDFPlayer.playMp3Folder(trackNumber);
    lastAudioTime = millis();
    nextAudioTrack = queuedNext;
    return;
  }
  if (isBusy || (millis() - lastAudioTime < AUDIO_COOLDOWN)) return;
  myDFPlayer.playMp3Folder(trackNumber);
  lastAudioTime = millis();
  nextAudioTrack = queuedNext;
}

void handleAudioQueue() {
  if (nextAudioTrack > 0 && (millis() - lastAudioTime > 600)) {
    if (digitalRead(BUSY_PIN) == HIGH) {
      int trackToPlay = nextAudioTrack;
      nextAudioTrack = 0;
      myDFPlayer.playMp3Folder(trackToPlay);
      lastAudioTime = millis();
      Serial.printf("[AUDIO] Memutar antrean track: %04d.mp3\n", trackToPlay);
    }
  }
}

// --- FUNGSI KOMPAS QMC5883P (0x2C) ---
void initQMC5883P() {
  Wire.beginTransmission(QMC5883P_ADDR);
  Wire.write(0x0B); Wire.write(0x08);
  Wire.endTransmission();

  Wire.beginTransmission(QMC5883P_ADDR);
  Wire.write(0x0A); Wire.write(0x0D);
  Wire.endTransmission();

  prefs.begin("qiblah_cal", true);
  magOffsetX = prefs.getFloat("offX", 0.0);
  magOffsetY = prefs.getFloat("offY", 0.0);
  magOffsetZ = prefs.getFloat("offZ", 0.0);
  prefs.end();

  Serial.printf("QMC5883P OK | Offset: X=%.1f Y=%.1f Z=%.1f\n", magOffsetX, magOffsetY, magOffsetZ);
}

bool readQMC5883PRaw(int16_t &x, int16_t &y, int16_t &z) {
  Wire.beginTransmission(QMC5883P_ADDR);
  Wire.write(0x01);
  Wire.endTransmission(false);
  Wire.requestFrom(QMC5883P_ADDR, 6);

  if (Wire.available() == 6) {
    x = Wire.read() | (Wire.read() << 8);
    y = Wire.read() | (Wire.read() << 8);
    z = Wire.read() | (Wire.read() << 8);
    return true;
  }
  return false;
}

// --- KALIBRASI KOMPAS 10 DETIK ---
void jalankanKalibrasiKompas() {
  Serial.println("[CAL] Memulai kalibrasi 10 detik...");
  UNO_Serial.println("LCD,Putar Angka 8...");
  
  int16_t minX = 32767, maxX = -32768;
  int16_t minY = 32767, maxY = -32768;
  int16_t minZ = 32767, maxZ = -32768;

  unsigned long startCal = millis();
  while (millis() - startCal < 10000) {
    int16_t x, y, z;
    if (readQMC5883PRaw(x, y, z)) {
      if (x < minX) minX = x; if (x > maxX) maxX = x;
      if (y < minY) minY = y; if (y > maxY) maxY = y;
      if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
    }
    delay(20);
  }

  magOffsetX = (maxX + minX) / 2.0;
  magOffsetY = (maxY + minY) / 2.0;
  magOffsetZ = (maxZ + minZ) / 2.0;

  prefs.begin("qiblah_cal", false);
  prefs.putFloat("offX", magOffsetX);
  prefs.putFloat("offY", magOffsetY);
  prefs.putFloat("offZ", magOffsetZ);
  prefs.end();

  UNO_Serial.println("LCD,Kalibrasi OK!");
  UNO_Serial.println("CMD,F");
  delay(1500);
}

// --- BACA KOMPAS + TILT COMPENSATION (RELATIF TERHADAP AUTO-ZERO) + SMOOTHING ---
void updateSensorArah() {
  int16_t rx, ry, rz;
  if (!readQMC5883PRaw(rx, ry, rz)) return;

  float mx = (float)rx - magOffsetX;
  float my = (float)ry - magOffsetY;
  float mz = (float)rz - magOffsetZ;

  float headingRad;
  if (mpuOK) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    float rawRoll  = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
    float rawPitch = atan2(-a.acceleration.x, sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 180.0 / PI;

    // Hitung kemiringan relatif terhadap posisi awal (aman jika sensor terbalik)
    relRoll  = normalizeAngleDiff(rawRoll - baseRoll);
    relPitch = normalizeAngleDiff(rawPitch - basePitch);

    float rollRad  = radians(relRoll);
    float pitchRad = radians(relPitch);

    float xh = mx * cos(pitchRad) + mz * sin(pitchRad);
    float yh = mx * sin(rollRad) * sin(pitchRad) + my * cos(rollRad) - mz * sin(rollRad) * cos(pitchRad);
    headingRad = atan2(yh, xh);
  } else {
    headingRad = atan2(my, mx);
  }

  smoothSin = (ALPHA_SMOOTH * sin(headingRad)) + ((1.0 - ALPHA_SMOOTH) * smoothSin);
  smoothCos = (ALPHA_SMOOTH * cos(headingRad)) + ((1.0 - ALPHA_SMOOTH) * smoothCos);

  float headingDeg = atan2(smoothSin, smoothCos) * 180.0 / PI;
  headingDeg += DEKLINASI_MAGNETIK;
  if (headingDeg < 0) headingDeg += 360.0;
  if (headingDeg >= 360.0) headingDeg -= 360.0;

  currentHeading = headingDeg;
}

// --- FUNGSI UTILITAS ---
String toDMS(float desimal, boolean isLat) {
  char arah = isLat ? ((desimal >= 0) ? 'U' : 'S') : ((desimal >= 0) ? 'T' : 'B');
  float val = abs(desimal);
  int d = (int)val;
  float m_float = (val - d) * 60.0;
  int m = (int)m_float;
  int s = (int)((m_float - m) * 60.0);
  return String(d) + "°" + String(m) + "'" + String(s) + "\"" + arah;
}

String azimutToDMS(float desimal) {
  int d = (int)desimal;
  float m_float = (desimal - d) * 60.0;
  int m = (int)m_float;
  int s = (int)((m_float - m) * 60.0);
  return String(d) + "°" + String(m) + "'" + String(s) + "\"";
}

float hitungKiblat(float latUser, float lngUser) {
  float latU_rad = radians(latUser);
  float lngU_rad = radians(lngUser);
  float latK_rad = radians(KAABAH_LAT);
  float lngK_rad = radians(KAABAH_LNG);
  float deltaLng = lngK_rad - lngU_rad;
  float y = sin(deltaLng);
  float x = cos(latU_rad) * tan(latK_rad) - sin(latU_rad) * cos(deltaLng);
  float qibla = degrees(atan2(y, x));
  return (qibla < 0) ? (qibla + 360) : qibla;
}

void kirimLCDUnik(String teks) {
  if (teks != lastStatusLCD) {
    UNO_Serial.print("LCD,");
    UNO_Serial.println(teks);
    lastStatusLCD = teks;
  }
}

// --- EKSEKUSI LIVE TRACKING & AUTO RE-TRACK ---
void jalankanNavigasiLive() {
  float selisihSudut = normalizeAngleDiff(targetKiblat - currentHeading);

  // 1. Jika sedang TERKUNCI, pantau apakah alat tergeser melebihi BATAS_GESER (5°)
  if (targetTerkunci) {
    if (abs(selisihSudut) > BATAS_GESER) {
      Serial.println("[NAV] Alat tergeser! Membuka kunci & navigasi ulang...");
      targetTerkunci = false;
      nextAudioTrack = 0;
      UNO_Serial.println("LASER,0");
      lastStatusLCD = "";
    } else {
      return;
    }
  }

  // 2. Cek kemiringan relatif terhadap posisi datar awal (Auto-Zero)
  if (mpuOK && (abs(relRoll) > BATAS_MIRING || abs(relPitch) > BATAS_MIRING)) {
    UNO_Serial.println("CMD,OFF");
    kirimLCDUnik("Ratakan Alat!");
    return;
  }

  int servoAngle = constrain(90 + (int)selisihSudut, 0, 180);
  UNO_Serial.print("SERVO,");
  UNO_Serial.println(servoAngle);

  String statusArah = "";

  if (abs(selisihSudut) <= TOLERANSI_TEPAT) {
    statusArah = "Sudah Sesuai";
    targetTerkunci = true;

    kirimLCDUnik("Arah: Sesuai!");
    UNO_Serial.println("LASER,1");
    UNO_Serial.println("CMD,F");
    playAudio(7, true, 8);

    if (isOnline && WiFi.status() == WL_CONNECTED) {
      Firebase.setString(fbdo, "/Navigator/Status", statusArah);
    }
  } 
  else if (abs(selisihSudut) <= TOLERANSI_HAMPIR) {
    statusArah = "Hampir Tepat";
    UNO_Serial.println("CMD,OFF");
    kirimLCDUnik("Hampir Tepat...");
    playAudio(6, false);
  } 
  else {
    UNO_Serial.println("LASER,0");
    kirimLCDUnik("Kiblat:" + azimutToDMS(targetKiblat));

    if (selisihSudut > TOLERANSI_HAMPIR) {
      statusArah = "Kurang Kanan";
      UNO_Serial.println("CMD,R");
      playAudio(5, false);
    } else {
      statusArah = "Kurang Kiri";
      UNO_Serial.println("CMD,L");
      playAudio(4, false);
    }
  }
}

// --- FUNGSI PEMROSESAN PERINTAH UNO ---
void prosesPerintahUno(String msg) {
  if (msg == "PING") {
    UNO_Serial.println(isOnline ? "PONG,ON" : "PONG,OFF");
    return;
  }
  if (msg == "CAL_START") {
    navigasiAktif = false;
    targetTerkunci = false;
    jalankanKalibrasiKompas();
    return;
  }
  if (msg == "BTN_PRESSED" && koordinatTerkirim) {
    navigasiAktif = true;
    targetTerkunci = false;
    lastStatusLCD = "";
    Serial.println("[NAV] Mode Live Tracking AKTIF!");
  }
}

// --- SETUP UTAMA ---
void setup() {
  Serial.begin(115200); 
  delay(500); 
  Serial.println("\n--- Memulai Sistem Helio Qiblah PRO ---");

  UNO_Serial.begin(9600, SERIAL_8N1, RX_UNO, TX_UNO);
  GPS_Serial.begin(38400, SERIAL_8N1, RX_GPS, TX_GPS);
  DFP_Serial.begin(9600);
  pinMode(BUSY_PIN, INPUT_PULLUP);

  // Mode Online / Offline
  if (ENABLE_WIFI) {
    Serial.print("Menyambungkan Wi-Fi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt < WIFI_TIMEOUT_MS)) {
      delay(500); yield(); Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      isOnline = true;
      Serial.println("\n[MODE ONLINE] Wi-Fi Terhubung!");
      config.database_url = FIREBASE_HOST;
      config.signer.tokens.legacy_token = FIREBASE_AUTH;
      Firebase.begin(&config, &auth);
      Firebase.reconnectWiFi(true);
    } else {
      isOnline = false;
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("\n[MODE OFFLINE] Melanjutkan eksekusi lokal...");
    }
  } else {
    isOnline = false;
    WiFi.mode(WIFI_OFF);
  }

  // Inisialisasi Sensor
  Wire.begin(I2C1_SDA, I2C1_SCL);
  delay(300);
  mpuOK = mpu.begin(0x68, &Wire);
  if (mpuOK) {
    calibrateMPUZero(); // Ambil posisi awal sebagai titik datar (0°)
  } else {
    Serial.println("MPU6050 Gagal!");
  }
  
  delay(200);
  initQMC5883P();
  
  // Inisialisasi DFPlayer
  delay(500);
  if (myDFPlayer.begin(DFP_Serial, false, true)) {
    myDFPlayer.volume(30); 
    Serial.println("MP3-TF-16P OK"); 
  } else {
    Serial.println("MP3-TF-16P GAGAL");
  }
  
  playAudio(1, true, 2);
  Serial.println("Menunggu data satelit GPS...");
}

// --- LOOP UTAMA ---
void loop() {
  handleAudioQueue();

  while (GPS_Serial.available() > 0) gps.encode(GPS_Serial.read());

  if (!koordinatTerkirim && (millis() - lastSatUpdate > 1500)) {
    UNO_Serial.print("SAT,");
    UNO_Serial.println(gps.satellites.value());
    lastSatUpdate = millis();
  }

  if (gps.location.isValid()) {
    float latAktif = gps.location.lat();
    float lngAktif = gps.location.lng();
    targetKiblat = hitungKiblat(latAktif, lngAktif);

    if (!koordinatTerkirim) {
      Serial.printf("Koordinat Terkunci! Lat: %.6f Lng: %.6f | Kiblat: %.2f\n", latAktif, lngAktif, targetKiblat);
      if (isOnline && WiFi.status() == WL_CONNECTED) {
        Firebase.setFloat(fbdo, "/Navigator/Koordinat/Lat", latAktif);
        Firebase.setFloat(fbdo, "/Navigator/Koordinat/Lng", lngAktif);
        Firebase.setFloat(fbdo, "/Navigator/Sensor/Kiblat", targetKiblat);
      }
      playAudio(3, true, 0);
      koordinatTerkirim = true;
    }

    if (!navigasiAktif && !targetTerkunci && (millis() - lastKirimUno > INTERVAL_KIRIM_UNO)) {
      UNO_Serial.print("COORD,Lat:");
      UNO_Serial.print(toDMS(latAktif, true));
      UNO_Serial.print(",Lng:");
      UNO_Serial.println(toDMS(lngAktif, false));
      lastKirimUno = millis();
    }
  }

  updateSensorArah();

  if (navigasiAktif && (millis() - lastNavUpdate >= INTERVAL_NAVIGASI)) {
    lastNavUpdate = millis();
    jalankanNavigasiLive();
  }

  if (UNO_Serial.available()) {
    String msg = UNO_Serial.readStringUntil('\n');
    msg.trim();
    prosesPerintahUno(msg);
  }
}