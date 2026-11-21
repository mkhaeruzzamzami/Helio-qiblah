/* === HELIO QIBLAH NAVIGATOR - KODE FINAL ARDUINO UNO (SERVO 0° + ANTI-JITTER) === */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <SoftwareSerial.h>

// --- KONFIGURASI PIN ---
#define PIN_BUTTON 2
#define PIN_VIB_R 5   
#define PIN_VIB_L 6   
#define PIN_LASER 8   
#define PIN_SERVO 9   

// --- PARAMETER UX & KEAMANAN ---
const unsigned long DEBOUNCE_DELAY = 50;
const unsigned long LONG_PRESS_MS = 3000;      // Tahan 3 detik untuk kalibrasi kompas
const unsigned long LASER_AUTO_OFF_MS = 30000; // Laser mati otomatis setelah 30 detik
const unsigned long SERVO_IDLE_DETACH_MS = 600;// Lepas PWM setelah 0,6 detik diam (anti-jitter)

// --- OBJEK GLOBAL ---
SoftwareSerial ESP_Serial(10, 11); 
LiquidCrystal_I2C lcd(0x27, 16, 2); 
Servo pointerServo;

// --- VARIABEL STATUS ---
int buttonState = HIGH;             
int lastButtonState = HIGH;         
unsigned long lastDebounceTime = 0; 
unsigned long buttonPressStart = 0;
bool longPressTriggered = false;

bool isOnline = false;
bool gpsReady = false;
bool isButtonLocked = false;

bool laserActive = false;
unsigned long laserOnTime = 0;

// --- VARIABEL KONTROL SERVO ANTI-JITTER ---
int currentServoAngle = 0;
bool servoAttached = false;
unsigned long lastServoMoveTime = 0;

// --- FUNGSI KONTROL SERVO ---
void moveServo(int targetAngle) {
  targetAngle = constrain(targetAngle, 0, 180);

  // Abaikan jika sudut tidak berubah agar servo tidak terus-terusan attach
  if (targetAngle == currentServoAngle && !servoAttached) return;

  if (!servoAttached) {
    pointerServo.attach(PIN_SERVO);
    servoAttached = true;
  }
  pointerServo.write(targetAngle);
  currentServoAngle = targetAngle;
  lastServoMoveTime = millis();
}

void handleServoIdle() {
  if (servoAttached && (millis() - lastServoMoveTime >= SERVO_IDLE_DETACH_MS)) {
    pointerServo.detach();
    servoAttached = false;
  }
}

// --- FUNGSI KONTROL AKTUATOR ---
void setVibration(bool left, bool right) {
  digitalWrite(PIN_VIB_L, left ? HIGH : LOW);
  digitalWrite(PIN_VIB_R, right ? HIGH : LOW);
}

void playHeartbeatHaptic() {
  for (uint8_t i = 0; i < 2; i++) {
    setVibration(true, true);
    delay(200);
    setVibration(false, false);
    delay(100);
  }
}

void setLaser(bool state) {
  digitalWrite(PIN_LASER, state ? HIGH : LOW);
  laserActive = state;
  if (state) laserOnTime = millis();
}

void handleLaserAutoOff() {
  if (laserActive && (millis() - laserOnTime >= LASER_AUTO_OFF_MS)) {
    setLaser(false);
    Serial.println(F("[INFO] Laser auto-off (30 detik)."));
  }
}

// --- FUNGSI CETAK LCD BERSIH (DUKUNG SIMBOL '°' & FILTER NOISE) ---
void printFormattedLCD(const String &text) {
  for (unsigned int i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text[i];

    if (c == 0xC2 && (i + 1 < text.length()) && (uint8_t)text[i + 1] == 0xB0) {
      lcd.write((uint8_t)223); // Simbol ° asli LCD
      i++;
    } 
    else if (isPrintable(c)) {
      lcd.print((char)c);
    }
  }
}

// --- FUNGSI INISIASI & HANDSHAKE LATAR BELAKANG ---
void runSystemInit() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("  HELIO QIBLAH  "));
  lcd.setCursor(0, 1);
  lcd.print(F("Inisiasi        "));
  
  Serial.println(F("[INIT] Pengecekan koneksi ESP32..."));

  bool espConnected = false;
  unsigned long pingTimer = 0;
  unsigned long animTimer = 0;
  uint8_t dotCount = 0;

  while (!espConnected) {
    if (millis() - animTimer > 400) {
      animTimer = millis();
      dotCount = (dotCount + 1) % 4;
      lcd.setCursor(8, 1);
      for (uint8_t i = 0; i < 3; i++) {
        lcd.print(i < dotCount ? '.' : ' ');
      }
    }

    if (millis() - pingTimer > 1000) {
      ESP_Serial.println(F("PING"));
      pingTimer = millis();
    }

    if (ESP_Serial.available()) {
      String resp = ESP_Serial.readStringUntil('\n');
      resp.trim();
      if (resp.startsWith(F("PONG"))) {
        isOnline = resp.endsWith(F("ON"));
        espConnected = true;
        Serial.println(isOnline ? F("[OK] ESP32 Online") : F("[OK] ESP32 Offline"));
      }
    }
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F(" Selamat Datang "));
  lcd.setCursor(0, 1);
  lcd.print(F("di Helio Qiblah "));
  delay(2500);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(isOnline ? F("HelioQiblah [ON]") : F("HelioQiblah[OFF]"));
  lcd.setCursor(0, 1);
  lcd.print(F("GPS: 0 Satelit  "));
}

// --- FUNGSI PEMBACAAN TOMBOL (1X TEKAN + TAHAN 3 DETIK KALIBRASI) ---
void handleButton() {
  int reading = digitalRead(PIN_BUTTON);

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) {
        buttonPressStart = millis();
        longPressTriggered = false;
      } else {
        if (!longPressTriggered) {
          if (!gpsReady) {
            Serial.println(F("[INFO] GPS belum terkunci."));
          } else if (!isButtonLocked) {
            isButtonLocked = true;
            Serial.println(F("[OK] Live Tracking dimulai & tombol dikunci."));
            ESP_Serial.println(F("BTN_PRESSED"));
          } else {
            Serial.println(F("[LIMIT] Tombol sudah dikunci."));
          }
        }
      }
    }

    if (buttonState == LOW && !longPressTriggered && !isButtonLocked) {
      if (millis() - buttonPressStart >= LONG_PRESS_MS) {
        longPressTriggered = true;
        Serial.println(F("[CAL] Memulai kalibrasi kompas..."));
        ESP_Serial.println(F("CAL_START"));
      }
    }
  }
  lastButtonState = reading;
}

// --- FUNGSI PEMROSESAN PESAN ESP32 ---
void handleCommunication() {
  if (ESP_Serial.available()) {
    String msg = ESP_Serial.readStringUntil('\n');
    msg.trim(); 
    
    if (msg.startsWith(F("SAT,")) && !gpsReady) {
      lcd.setCursor(0, 1);
      lcd.print(F("GPS: "));
      lcd.print(msg.substring(4));
      lcd.print(F(" Satelit   "));
    }
    else if (msg.startsWith(F("COORD,"))) {
      gpsReady = true;
      int idxLat = msg.indexOf(F("Lat:"));
      int idxLng = msg.indexOf(F(",Lng:"));
      if (idxLat != -1 && idxLng != -1) {
        lcd.clear();
        lcd.setCursor(0, 0);
        printFormattedLCD(msg.substring(idxLat + 4, idxLng));
        lcd.setCursor(0, 1);
        printFormattedLCD(msg.substring(idxLng + 5));
      }
    } 
    else if (msg.startsWith(F("LCD,"))) {
      lcd.setCursor(0, 1);
      lcd.print(F("                "));
      lcd.setCursor(0, 1);
      printFormattedLCD(msg.substring(4));
    }
    else if (msg.startsWith(F("SERVO,"))) {
      moveServo(msg.substring(6).toInt());
    }
    else if (msg.startsWith(F("LASER,"))) {
      setLaser(msg.substring(6) == "1");
    }
    else if (msg.startsWith(F("CMD,"))) {
      String cmd = msg.substring(4);
      if (cmd == "L")        setVibration(true, false);
      else if (cmd == "R")   setVibration(false, true);
      else if (cmd == "F")   playHeartbeatHaptic();
      else if (cmd == "OFF") setVibration(false, false);
    }
  }
}

// --- SETUP UTAMA ---
void setup() {
  Serial.begin(9600);
  ESP_Serial.begin(9600);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LASER, OUTPUT);
  pinMode(PIN_VIB_L, OUTPUT);
  pinMode(PIN_VIB_R, OUTPUT);
  
  setLaser(false);
  setVibration(false, false);

  // Set servo ke titik 0° lalu langsung detach agar diam total
  pointerServo.write(0);
  pointerServo.attach(PIN_SERVO);
  delay(600);
  pointerServo.detach();
  servoAttached = false;
  currentServoAngle = 0;

  delay(300);
  lcd.init();
  lcd.backlight();
  
  runSystemInit();
}

// --- LOOP UTAMA ---
void loop() {
  handleButton();
  handleCommunication();
  handleLaserAutoOff();
  handleServoIdle();
}