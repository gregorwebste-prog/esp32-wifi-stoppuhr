# ESP32 WiFi Stoppuhr

Synchronized stopwatch across two ESP32 devices via WiFi UDP. Press any button on either device to start or stop the stopwatch — both displays update simultaneously.

## Hardware

| Komponente | Details |
|---|---|
| Mikrocontroller | 2x ESP32 Lolin (Wemos Lolin32) |
| Display | 2x OLED 0.96" SSD1306, 128×64 Pixel, I2C |
| Taster | 1x pro ESP32, GPIO27 → GND |

### Verdrahtung (identisch auf beiden ESP32s)

```
OLED:
  VCC  → 3.3V
  GND  → GND
  SDA  → GPIO 21
  SCL  → GPIO 22

Taster:
  Pin 1 → GPIO 27
  Pin 2 → GND
  (Interner Pull-up wird per Software aktiviert)
```

## Funktionsweise

```
Master (COM5)                       Client (COM8)
┌─────────────────┐                ┌─────────────────┐
│  WiFi AP        │◄──────────────►│  WiFi Station   │
│  "StopwatchNet" │   UDP :1234    │  verbindet sich │
│                 │                │                 │
│  Taster GPIO27  │                │  Taster GPIO27  │
└─────────────────┘                └─────────────────┘
         │                                  │
         ▼                                  ▼
   Knopf drücken →  UDP "START:<ms>"  → beide starten
   Knopf drücken →  UDP "STOP:<ms>"   → beide stoppen
```

### WiFi-Protokoll (UDP)

| Nachricht | Format | Beschreibung |
|---|---|---|
| Start | `START:<millis>` | Stoppuhr starten |
| Stop | `STOP:<elapsed_ms>` | Stoppuhr stoppen, Zeit synchronisieren |

- **AP SSID:** `StopwatchNet`
- **Passwort:** `stopwatch123`
- **Master-IP:** `192.168.4.1`
- **UDP-Port:** `1234`
- **Broadcast:** `192.168.4.255`

## Display-Anzeige

```
┌────────────────────────┐
│   === STOPPUHR ===     │
│────────────────────────│
│     MM:SS.hh           │  ← große Schrift (2x)
│                        │
│ LAEUFT  [Knopf=Stop]  │  ← Status
│                [MASTER]│  ← Rolle
└────────────────────────┘
```

## Projektstruktur

```
esp32_stopwatch/
├── master/
│   ├── platformio.ini      # Board: lolin32, Port: COM5
│   └── src/main.cpp        # Master-Firmware (erstellt WiFi-AP)
├── client/
│   ├── platformio.ini      # Board: lolin32, Port: COM8
│   └── src/main.cpp        # Client-Firmware (verbindet mit AP)
└── README.md
```

## Flashen

Voraussetzung: [PlatformIO](https://platformio.org/) installiert (`pip install platformio`)

```bash
# Master flashen (COM5)
cd master
pio run --target upload

# Client flashen (COM8)
cd ../client
pio run --target upload
```

## Bibliotheken

- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306) `^2.5.7`
- [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) `^1.11.9`
- WiFi (ESP32 built-in)
- WiFiUdp (ESP32 built-in)

## Bedienung

1. **Master zuerst einschalten** — erscheint als WiFi-AP `StopwatchNet`
2. **Client einschalten** — verbindet sich automatisch mit dem Master
3. **Knopf auf einem der beiden ESP32s drücken** → Stoppuhr startet auf beiden
4. **Knopf erneut drücken** → Stoppuhr stoppt auf beiden, Zeit wird angezeigt
5. **Nochmal drücken** → Reset, bereit für nächste Messung

## Bekannte Einschränkungen

- Latenz zwischen den Displays: typisch < 5 ms (lokales WiFi-AP)
- Die `millis()`-Uhren der beiden ESP32s laufen nicht synchron — bei START vom Client wird die lokale `millis()` als Referenz verwendet. Dadurch kann es zu ~1–10 ms Abweichung kommen.
