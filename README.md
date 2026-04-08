# ESP32 WiFi-Stoppuhr

Zwei ESP32-Geräte synchronisieren eine Stoppuhr per WiFi (UDP-Broadcast).
Beide zeigen dieselbe Zeit auf einem OLED-Display — jeder kann Start/Stop/Reset auslösen.

---

## Hardware

### Benötigt (je Gerät)
- ESP32 DevKit (Lolin32 o.ä.)
- OLED SSD1306 128×64 I2C
- Taster (Momentary, NO)
- 2× 47 kΩ Widerstände (Spannungsteiler Akku)
- Li-Ion 18650 3500 mAh + Halter
- TP4056 Lademodul (empfohlen)

### Pin-Belegung

| Signal        | GPIO | Hinweis                         |
|---------------|------|---------------------------------|
| OLED SDA      | 21   |                                 |
| OLED SCL      | 22   |                                 |
| OLED VCC      | 26   | über NPN/N-MOSFET schalten      |
| Taster        | 27   | → GND, intern Pull-Up           |
| Akku ADC      | 33   | ADC1_CH5 (funktioniert mit WiFi)|

### Display-Abschalter (GPIO 26)

GPIO 26 schaltet die Display-Versorgung über einen NPN-Transistor oder N-MOSFET:

```
GPIO26 ──[1kΩ]── Basis NPN (z.B. 2N2222)
                 Collector → OLED VCC
                 Emitter   → GND

   oder N-MOSFET (z.B. IRLZ44N / BS170):
GPIO26 ──[10kΩ]── Gate
                  Drain → OLED VCC
                  Source → GND
```

> HIGH = Display ein | LOW = Display aus (kein Ruhestrom)

### Spannungsteiler Akku (GPIO 33)

```
LiIon+ ──[47kΩ]──┬──[47kΩ]── GND
                 └── GPIO33
```

Messbereich: 3.0–4.2 V → ADC 1.5–2.1 V (passt in 11 dB Attenuation, ADC1)

---

## Funktionsweise

| Aktion              | Verhalten                                  |
|---------------------|--------------------------------------------|
| Kurzdruck           | Start → Stop → Reset (zyklisch)           |
| 4 Sek halten        | Sofort Deep Sleep                          |
| 8 Min Inaktivität   | Display aus + Light Sleep                  |
| 18 Min Inaktivität  | Deep Sleep                                 |
| Aufwachen           | Beliebiger Tasterdruck                     |
| Deep Sleep Wakeup   | Vollständiger Neustart, WiFi reconnect     |
| `DEEPSLEEP` per UDP | Wenn ein Gerät schläft, schläft das andere auch |

Das Display zeigt während der Wartezeit einen Countdown bis zum nächsten Sleep an.

---

## Display-Layout

```
┌────────────────────────────┐
│ 0:00.00                    │  ← Textgröße 3, max 9:59.99 (7 Zeichen = 126px)
│ 3.87V   72%  [M]           │  ← Akku (M=Master / C=Client)
│────────────────────────────│
│ Knopf=Start  4s=AUS        │  ← Statuszeile
│ Sleep in 450s              │  ← nur wenn idle > 10s
└────────────────────────────┘
```

---

## Stromverbrauch & Akkulaufzeit

Gemessen/geschätzt bei 3.7 V (Li-Ion nominal), ESP32 + SSD1306:

| Zustand                           | Strom     | Laufzeit mit 3500 mAh |
|-----------------------------------|-----------|------------------------|
| Aktiv, Display an, WiFi aktiv     | ~150 mA   | ~23 Stunden            |
| Aktiv, Display aus, WiFi aktiv    | ~120 mA   | ~29 Stunden            |
| Light Sleep, Display aus, WiFi    | ~3–5 mA   | ~700–1160 Stunden      |
| Deep Sleep (nur RTC)              | ~15 µA    | ~27 000 Stunden        |

### Typisches Nutzungsprofil

Beispiel: täglich 10× je 5 Minuten messen, sonst Deep Sleep.

```
Pro Nutzung:
  5 Min aktiv (Display an)  = 5/60 × 150 mA  = 12.5 mAh
  8 Min idle bis Light Sleep = 8/60 × 120 mA  = 16.0 mAh
  ──────────────────────────────────────────────────────
  Pro Session total:                           ~ 28.5 mAh

  10 Sessions/Tag:                             ~285 mAh/Tag
  → 3500 mAh / 285 mAh ≈ 12 Tage ohne Laden
```

Deep Sleep zwischen den Sessions kostet praktisch nichts (~0.05 mAh/Std).

---

## WiFi-Protokoll (UDP Broadcast 192.168.4.255:1234)

| Nachricht      | Bedeutung                                        |
|----------------|--------------------------------------------------|
| `START`        | Stoppuhr starten                                 |
| `STOP:<ms>`    | Stoppuhr stoppen, `<ms>` = gemessene Millisekunden |
| `RESET`        | Zurücksetzen auf 0                               |
| `DEEPSLEEP`    | Beide Geräte gehen in Deep Sleep                 |

---

## Projektstruktur

```
esp32_stopwatch/
├── master/
│   ├── platformio.ini      # Board: esp32dev, Port: COM8
│   └── src/main.cpp        # Master-Firmware (erstellt WiFi-AP)
├── client/
│   ├── platformio.ini      # Board: esp32dev, Port: COM5
│   └── src/main.cpp        # Client-Firmware (verbindet mit AP)
└── README.md
```

---

## Bauen & Flashen

Benötigt: [PlatformIO](https://platformio.org/)

```bash
# Master flashen (COM8)
cd master
pio run --target upload

# Client flashen (COM5)
cd client
pio run --target upload
```

### platformio.ini

```ini
[env:esp32_master]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    adafruit/Adafruit SSD1306
    adafruit/Adafruit GFX Library
```

---

## Bedienung

1. **Master zuerst einschalten** — erstellt AP `StopwatchNet`
2. **Client einschalten** — verbindet sich automatisch
3. **Taster** auf beliebigem Gerät → Start / Stop / Reset
4. **4 Sek halten** → Deep Sleep (beide Geräte)

---

## Bekannte Einschränkungen

- Während Master im **Light Sleep** ist, verliert Client die WiFi-Verbindung.  
  Er reconnected automatisch sobald Master aufwacht.
- **Deep Sleep** setzt Zustand zurück (kein gespeicherter Zwischenstand).
- ESP32 ADC hat leichte Nichtlinearität — Akkuanzeige ±0.05 V Toleranz ist normal.
- Latenzt zwischen Displays: typisch < 5 ms.
