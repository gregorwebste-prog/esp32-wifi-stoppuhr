# ESP32 WiFi-Stoppuhr

Zwei ESP32 Wemos Lolin32 synchronisieren eine Stoppuhr per WiFi (UDP-Broadcast).
Beide zeigen dieselbe Zeit auf einem OLED-Display — jeder kann Start/Stop/Reset auslösen.

---

## Hardware

### Benötigt (je Gerät)
- ESP32 Wemos Lolin32
- OLED SSD1306 128×64 I2C
- Taster (Momentary, NO)
- Li-Ion 18650 3500 mAh + Halter
- JST-PH 2.0mm Kabel für Batterieanschluss

### Pin-Belegung

| Signal        | GPIO | Hinweis                                      |
|---------------|------|----------------------------------------------|
| OLED SDA      | 21   |                                              |
| OLED SCL      | 22   |                                              |
| OLED VCC      | 26   | Direkt am Pin (HIGH=an, LOW=aus)             |
| Taster        | 27   | → GND, intern Pull-Up                        |
| Akku ADC      | 35   | Eingebauter 100kΩ/100kΩ Teiler auf Lolin32   |

### Batterie-Anschluss

Den **JST-PH 2.0mm Stecker** auf dem Lolin32 verwenden — **nicht** den 5V-Pin!

```
18650 ──JST──► Lolin32 Lade-IC (TP4056)
                    │
                    ▼
              3.3V Regler (für LiPo ausgelegt, 3.0–4.2V Eingang)
                    │
                    ▼
                  ESP32
```

Beim JST-Stecker Polarität prüfen: **Rot = +, Schwarz = −**
Wenn USB eingesteckt ist, wird der Akku automatisch geladen.

### Akkuspannung messen

Der Lolin32 hat einen eingebauten Spannungsteiler (100kΩ/100kΩ) auf **GPIO35** — kein externer Teiler nötig.

```
Akku+ ──[100kΩ]──┬──[100kΩ]── GND
                 └── GPIO35 (ADC1_CH7)
```

Formel: `V_bat = ADC_Spannung × 2`

### Display-Abschalter (GPIO 26)

GPIO 26 schaltet OLED-VCC direkt (kein Transistor nötig bei SSD1306).
Im Deep Sleep wird GPIO 26 per `gpio_hold_en()` LOW gehalten → kein Ruhestrom.

---

## Ladezeit

Der Lolin32 lädt über den eingebauten **TP4056** Lade-IC (500 mA via USB 2.0):

| Akkustand | Ladezeit bei 500 mA |
|-----------|---------------------|
| 0 → 100%  | ~8–10 Stunden       |
| 20 → 80%  | ~4–5 Stunden        |
| 80 → 100% | ~2–3 Stunden (CV-Phase, Strom nimmt ab) |

> Bei USB 3.0 oder Schnellladegerät bleibt der TP4056 trotzdem bei 500 mA — das ist sein Hardware-Limit.

---

## Funktionsweise

| Aktion              | Verhalten                                  |
|---------------------|--------------------------------------------|
| Kurzdruck           | Start → Stop → Reset (zyklisch)           |
| 4 Sek halten        | Deep Sleep (auch ohne WiFi)                |
| 10 Min Inaktivität  | Deep Sleep                                 |
| Aufwachen           | Tasterdruck                                |
| Kein Master         | Client läuft im Offline-Modus, Sleep aktiv |

---

## Display-Layout

```
┌────────────────────────────┐
│ 0:00.00                    │  ← Textgröße 3, max 9:59.99
│ 3.87V   72%  [M]           │  ← Akku (M=Master / C=Client, !!= kein WiFi)
│────────────────────────────│
│ Knopf=Start  4s=AUS        │  ← Statuszeile
│ Sleep in 45s               │  ← letzte 60s vor Deep Sleep
└────────────────────────────┘
```

---

## Stromverbrauch & Akkulaufzeit

| Zustand                           | Strom     | Laufzeit mit 3500 mAh |
|-----------------------------------|-----------|------------------------|
| Master aktiv (WiFi AP + Display)  | ~115 mA   | ~30 Stunden            |
| Client aktiv (WiFi STA + Display) | ~35 mA    | ~100 Stunden           |
| Deep Sleep (gpio_hold aktiv)      | ~15–25 µA | ~16 000 Stunden        |

### Typisches Nutzungsprofil (10× 5 min/Tag)

```
Pro Session (Master):   5/60 × 115 mA ≈ 9.6 mAh
Pro Tag (10 Sessions):  ~96 mAh
→ 3500 mAh / 96 mAh ≈  36 Tage ohne Laden
```

---

## WiFi-Protokoll (UDP Broadcast 192.168.4.255:1234)

| Nachricht      | Bedeutung                                  |
|----------------|--------------------------------------------|
| `START`        | Stoppuhr starten                           |
| `STOP:<ms>`    | Stoppuhr stoppen, `<ms>` = Millisekunden   |
| `RESET`        | Zurücksetzen                               |
| `DEEPSLEEP`    | Beide Geräte gehen in Deep Sleep           |

---

## Energiespar-Maßnahmen

| Maßnahme              | Einsparung |
|-----------------------|------------|
| CPU 80 MHz (statt 240)| ~40 mA     |
| WiFi Modem Sleep      | ~60 mA     |
| TX-Power 5 dBm        | ~15 mA     |
| Bluetooth deaktiviert | ~5 mA      |
| GPIO26 hold im Sleep  | verhindert ~20 mA Leckverlust |

---

## Projektstruktur

```
esp32_stopwatch/
├── master/
│   ├── platformio.ini      # Board: lolin32, Port: COM9
│   └── src/main.cpp        # Master-Firmware (erstellt WiFi-AP)
├── client/
│   ├── platformio.ini      # Board: lolin32, Port: COM5
│   └── src/main.cpp        # Client-Firmware (verbindet mit AP)
└── README.md
```

---

## Bauen & Flashen

```bash
cd master && pio run --target upload   # COM9
cd client && pio run --target upload   # COM5
```

---

## Bedienung

1. **Master zuerst einschalten** → erstellt AP `StopwatchNet`
2. **Client einschalten** → verbindet automatisch (~10 Sek)
3. **Taster** auf beliebigem Gerät → Start / Stop / Reset
4. **4 Sek halten** → beide Geräte in Deep Sleep
5. **USB einstecken** → Akku lädt automatisch (LED auf Lolin32 zeigt Ladestatus)
