# Helio Qiblah Navigator

Perangkat bantu penentu arah kiblat berbasis **ESP32 (Master)** dan **Arduino UNO (Slave)** yang dirancang untuk memberikan panduan orientasi arah secara presisi melalui audio **DFPlayer Mini**, umpan balik getaran (*haptic*), layar **LCD 16x2 I2C**, penunjuk **Servo**, garis **Laser**, serta pemantauan *real-time* melalui **Dashboard Web Firebase**. Sistem ini dirancang ramah disabilitas sehingga dapat digunakan oleh penyandang tunanetra.

---

## Struktur Berkas Proyek

```text
Helio Qiblah Navigator/
├── audio/
│   ├── 0001.mp3                # Selamat datang di Helio Qiblah Navigator
│   ├── 0002.mp3                # Sedang mencari titik koordinat dan sinyal satelit
│   ├── 0003.mp3                # Titik koordinat berhasil dikunci
│   ├── 0004.mp3                # Arah kiblat berada di sebelah kiri
│   ├── 0005.mp3                # Arah kiblat berada di sebelah kanan
│   ├── 0006.mp3                # Posisi hampir tepat
│   ├── 0007.mp3                # Arah kiblat telah ditetapkan
│   ├── 0008.mp3                # Garis saf salat telah diproyeksikan
│   └── 0009.mp3                # Peringatan gangguan medan magnet
└── kode program/
    ├── esp32-Helioqiblat/
    │   └── esp32-Helioqiblat.ino        # Firmware Master (ESP32)
    ├── arduinouno-Helioqiblat/
    │   └── arduinouno-Helioqiblat.ino   # Firmware Slave (Arduino UNO)
    └── index.html                       # Dashboard Web Real-Time (Firebase + Tailwind CSS)
```

> **Catatan Penting MicroSD Audio:** DFPlayer Mini mewajibkan seluruh file audio dalam format **.mp3** dan disimpan di dalam folder bernama `mp3` (huruf kecil semua) pada kartu MicroSD berformat **FAT32** (`0001.mp3` s/d `0009.mp3`). Jika masih ada file berformat `.m4a`, konversikan ke `.mp3` terlebih dahulu sebelum disalin ke MicroSD.

---

## Fitur Utama Sistem

* **Arsitektur Dual-MCU (Master-Slave):** ESP32 menangani pembacaan sensor, GPS, audio, dan komunikasi cloud, sedangkan Arduino UNO menangani layar LCD, tombol fisik, serta aktuator (servo, vibrator, laser).
* **Mode Hybrid Online & Offline Otomatis:** Mencoba koneksi Wi-Fi selama 5 detik saat booting untuk sinkronisasi ke Firebase Realtime Database (`[ON]`). Jika jaringan tidak tersedia, Wi-Fi dinonaktifkan otomatis agar hemat daya dan alat berjalan mandiri (`[OFF]`).
* **Kompensasi Kemiringan (Auto-Zero MPU6050) & Smoothing Filter:** Mengkalibrasi titik datar awal saat booting dan mengoreksi pembacaan kompas **QMC5883P** menggunakan data akselerometer serta filter vektor (`sin`/`cos`) anti-*jitter*.
* **Kalibrasi Kompas Permanen (NVS):** Menyimpan hasil kalibrasi kompas (gerakan angka 8) ke memori non-volatil ESP32 agar tetap tersimpan saat daya dimatikan.
* **Audio Lock & Antrean Non-Blocking:** Menggunakan pin `BUSY` DFPlayer Mini agar instruksi suara tidak saling bertabrakan saat perangkat diputar.
* **Live Tracking & Auto Re-Track:** Memandu arah secara langsung; mengunci target saat selisih sudut $\le 1{,}5^\circ$ (mengaktifkan getaran ganda *heartbeat* dan laser saf selama 30 detik), serta otomatis membuka kunci jika alat tergeser kembali $> 5^\circ$.

---

## Wiring & Sambungan Perangkat Keras

> **Aturan Keselamatan Kelistrikan:**
> * Satukan seluruh jalur ground (**Common Ground**) antara regulator, ESP32, Arduino UNO, sensor, dan aktuator.
> * Jangan menyambungkan jalur daya 5V ke pin GPIO atau sensor 3,3V secara langsung.
> * Jauhkan modul kompas **QMC5883P** dari motor servo, vibrator, speaker, dan regulator **LM2596** untuk mencegah interferensi magnetik.

### 1. Sistem Catu Daya (Baterai 3S / Adaptor + LM2596)
* Kalibrasi tegangan output **LM2596** ke **5,00V** menggunakan multimeter sebelum dihubungkan ke komponen.
* **Rel 5V:** Mencatu `VIN`/`5V` ESP32, `5V` Arduino UNO, DFPlayer Mini, modul GPS, LCD I2C, Motor Servo, Vibrator, dan Laser.
* **Rel 3,3V:** Diambil dari pin `3V3` ESP32 khusus untuk sensor I2C (**MPU6050** dan **QMC5883P**).

### 2. Peta Pin ESP32 (Master)
| Komponen / Jalur | Pin ESP32 | Pin Tujuan | Keterangan |
| :--- | :--- | :--- | :--- |
| **Komunikasi Serial UNO** | `GPIO 32` (RX1) | Pin `11` (TX) Arduino UNO | Gunakan pembagi tegangan (`1kΩ` seri + `2kΩ` ke GND) dari TX UNO (5V) ke RX ESP32 (3,3V) |
| **Komunikasi Serial UNO** | `GPIO 33` (TX1) | Pin `10` (RX) Arduino UNO | Langsung (logika 3,3V terbaca HIGH oleh UNO) |
| **Modul GPS (UART2)** | `GPIO 16` (RX2) | `TX` Modul GPS | Baud rate `38400` bps |
| **Modul GPS (UART2)** | `GPIO 17` (TX2) | `RX` Modul GPS | Baud rate `38400` bps |
| **DFPlayer Mini** | `GPIO 14` (RX) | `TX` DFPlayer Mini | SoftwareSerial `9600` bps |
| **DFPlayer Mini** | `GPIO 13` (TX) | `RX` DFPlayer Mini | Pasang resistor seri `1kΩ` untuk meredam *noise* |
| **DFPlayer Mini (BUSY)** | `GPIO 25` | `BUSY` DFPlayer Mini | `INPUT_PULLUP` (`LOW` = audio sedang diputar) |
| **Bus I2C Sensor (3,3V)** | `GPIO 21` (SDA) | `SDA` MPU6050 & QMC5883P | Alamat MPU6050: `0x68`, QMC5883P: `0x2C` |
| **Bus I2C Sensor (3,3V)** | `GPIO 22` (SCL) | `SCL` MPU6050 & QMC5883P | Daya `VCC` dari pin `3V3` ESP32 |

### 3. Peta Pin Arduino UNO (Slave)
| Komponen / Jalur | Pin Arduino UNO | Pin Tujuan | Keterangan |
| :--- | :--- | :--- | :--- |
| **Komunikasi Serial ESP32** | Pin `10` (RX) | `GPIO 33` (TX1) ESP32 | SoftwareSerial `9600` bps |
| **Komunikasi Serial ESP32** | Pin `11` (TX) | `GPIO 32` (RX1) ESP32 | Melalui pembagi tegangan resistor 5V ke 3,3V |
| **LCD 16x2 I2C** | `A4` (SDA), `A5` (SCL) | `SDA`, `SCL` LCD I2C | Alamat I2C `0x27`, daya `5V` & `GND` |
| **Push Button Utama** | Pin `2` | Kaki 1 Push Button | Kaki 2 ke `GND` (`INPUT_PULLUP`) |
| **Motor Vibrator Kanan** | Pin `5` | Input Driver / Gate MOSFET | Aktif `HIGH` saat arah kurang ke kanan |
| **Motor Vibrator Kiri** | Pin `6` | Input Driver / Gate MOSFET | Aktif `HIGH` saat arah kurang ke kiri |
| **Laser Penunjuk Saf** | Pin `8` | Gate MOSFET / Sinyal Modul | Padam otomatis setelah 30 detik |
| **Motor Servo Penunjuk** | Pin `9` | Kabel Sinyal PWM Servo | Daya `5V` & `GND` eksternal dari LM2596 |

---

## Cara Menjalankan & Instalasi

### Langkah 1: Persiapan MicroSD DFPlayer Mini
1. Format kartu MicroSD menggunakan sistem file **FAT32**.
2. Pastikan seluruh file audio (`0001.mp3` hingga `0009.mp3`) sudah berformat **.mp3**.
3. Buat folder bernama `mp3` di direktori utama (*root*) MicroSD dan salin kesembilan file audio tersebut ke dalamnya.
4. Pasang kartu MicroSD ke modul DFPlayer Mini.

### Langkah 2: Instalasi Library & Upload Firmware
Buka **Arduino IDE** dan instal pustaka berikut melalui **Library Manager**:
* **ESP32 (`esp32-Helioqiblat.ino`):**
  * `Firebase ESP32 Client` (Mobizt)
  * `TinyGPSPlus` (Mikal Hart)
  * `Adafruit MPU6050` & `Adafruit Unified Sensor`
  * `DFRobotDFPlayerMini`
  * `EspSoftwareSerial`
* **Arduino UNO (`arduinouno-Helioqiblat.ino`):**
  * `LiquidCrystal I2C` (Frank de Brabander)
  * `Servo` (bawaan Arduino IDE)

**Langkah Unggah Kode:**
1. **ESP32:** Buka `esp32-Helioqiblat.ino`, sesuaikan `WIFI_SSID` dan `WIFI_PASSWORD` jika diperlukan. Pilih board **DOIT ESP32 DEVKIT V1** (atau **ESP32 Dev Module**), pilih port COM ESP32, lalu klik **Upload** (tahan tombol **BOOT** jika muncul status *Connecting...*).
2. **Arduino UNO:** Buka `arduinouno-Helioqiblat.ino`, pilih board **Arduino Uno**, pilih port COM UNO, lalu klik **Upload**. *(Lepaskan suplai 5V eksternal saat mencolokkan kabel USB ke komputer).*

### Langkah 3: Pengoperasian Perangkat
1. **Nyalakan Daya:** Letakkan perangkat pada bidang datar lalu hidupkan saklar daya utama agar sensor **MPU6050** melakukan kalibrasi titik datar awal (*Auto-Zero*).
2. **Inisialisasi:** LCD menampilkan `Inisiasi...` lalu `Selamat Datang di Helio Qiblah`, diikuti status koneksi Wi-Fi `HelioQiblah [ON]` atau `HelioQiblah [OFF]`.
3. **Kunci Sinyal GPS:** Bawa perangkat ke area terbuka. LCD menampilkan jumlah satelit (`GPS: X Satelit`). Setelah koordinat terkunci, LCD menampilkan Lintang dan Bujur dalam format Derajat-Menit-Detik ($D^\circ M'S''$) dan memutar `0003.mp3`.
4. **Kalibrasi Kompas (Bila Diperlukan):** Saat posisi siaga (GPS sudah terkunci), tekan dan tahan tombol **Pin 2** selama **3 detik** hingga muncul `Putar Angka 8...`, lalu putar perangkat membentuk angka 8 selama 10 detik untuk menyimpan kalibrasi ke memori NVS.
5. **Mulai Pencarian Kiblat:**
   * Tekan tombol **Pin 2** sebanyak **1 kali** untuk mengaktifkan *Live Tracking*.
   * Putar perangkat mengikuti getaran motor (**Kiri** = Pin 6, **Kanan** = Pin 5), penunjuk servo, dan panduan suara (`0004.mp3` / `0005.mp3`), sambil menjaga kemiringan alat di bawah $15^\circ$.
   * Saat selisih sudut masuk toleransi $\le 1{,}5^\circ$, perangkat memberikan getaran konfirmasi ganda (*heartbeat*), mengunci teks `Arah: Sesuai!`, memutar suara konfirmasi (`0007.mp3` & `0008.mp3`), dan menyalakan **Laser** garis saf selama 30 detik.

### Langkah 4: Memantau Dashboard Web (`index.html`)
1. Pastikan perangkat aktif pada mode `HelioQiblah [ON]`.
2. Buka file `index.html` melalui peramban (Chrome/Edge/Safari) di laptop atau ponsel.
3. Indikator status di pojok kanan atas berubah menjadi **Online (Live)** dan menampilkan nilai **Latitude**, **Longitude**, **Target Sudut Kiblat**, serta **Status Arah** secara *real-time*.