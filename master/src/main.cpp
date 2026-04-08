/*
 * ESP32 WiFi-Stoppuhr — MASTER (COM8, erstellt Access Point)
 *
 * Pins:
 *   OLED  SDA=GPIO21  SCL=GPIO22
 *   OLED  VCC=GPIO26  (HIGH=an, LOW=aus)
 *   Taster GPIO27 → GND (INPUT_PULLUP)
 *   Akku  47kΩ/47kΩ Teiler → GPIO33 (ADC1_CH5)
 *
 * Verhalten:
 *   - Kurzdruck: Start → Stop → Reset
 *   - 4 Sek halten: sofort Deep Sleep
 *   - 8 Min Inaktivität (IDLE/STOPPED): Display aus + Light Sleep
 *   - 18 Min Inaktivität: Deep Sleep
 *   - Aufwachen: beliebiger Tasterdruck
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_sleep.h"

// ── Pins & Konstanten ─────────────────────────────────────────────
#define BUTTON_PIN     27
#define DISP_PWR_PIN   26
#define BATT_PIN       33
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define UDP_PORT     1234
#define DEBOUNCE_MS    60

#define LIGHT_SLEEP_MS  ( 8UL * 60UL * 1000UL)   //  8 Minuten
#define DEEP_SLEEP_MS   (18UL * 60UL * 1000UL)   // 18 Minuten
#define HOLD_DEEP_MS    4000UL                    //  4 Sek halten

const char*     AP_SSID = "StopwatchNet";
const char*     AP_PASS = "stopwatch123";
const IPAddress BROADCAST(192, 168, 4, 255);

// ── Objekte ───────────────────────────────────────────────────────
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiUDP udp;

// ── Zustand ───────────────────────────────────────────────────────
enum State { IDLE, RUNNING, STOPPED };
State         state      = IDLE;
unsigned long startMs    = 0;
unsigned long elapsedMs  = 0;
uint8_t       oledAddr   = 0x3C;
bool          dispOn     = true;

// ── Taster ────────────────────────────────────────────────────────
bool          rawBtn        = HIGH;
bool          lastRawBtn    = HIGH;
bool          debouncedBtn  = HIGH;
unsigned long debounceTimer = 0;
unsigned long btnPressMs    = 0;
bool          btnHeld       = false;

// ── Aktivitäts-Timer ──────────────────────────────────────────────
unsigned long lastActivityMs = 0;
bool          lightSlept     = false;

// ── Batterie ──────────────────────────────────────────────────────
float readBattVoltage() {
    // GPIO33 = ADC1_CH5, 47k/47k Teiler → V_bat = V_adc × 2
    int raw = analogRead(BATT_PIN);
    float v_adc = (raw / 4095.0f) * 3.3f;
    return v_adc * 2.0f;
}

int battPercent(float v) {
    // Li-Ion: 4.2V = 100%, 3.0V = 0%
    if (v >= 4.2f) return 100;
    if (v <= 3.0f) return 0;
    return (int)((v - 3.0f) / 1.2f * 100.0f + 0.5f);
}

// ── OLED ──────────────────────────────────────────────────────────
uint8_t detectOLED() {
    for (uint8_t addr : {0x3C, 0x3D}) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) return addr;
    }
    return 0x3C;
}

void oledPowerOn() {
    digitalWrite(DISP_PWR_PIN, HIGH);
    delay(80);
    Wire.begin(21, 22);
    oled.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    oled.setTextColor(SSD1306_WHITE);
    dispOn = true;
}

void oledPowerOff() {
    oled.clearDisplay();
    oled.display();
    delay(10);
    digitalWrite(DISP_PWR_PIN, LOW);
    dispOn = false;
}

void renderDisplay(unsigned long ms, const char* statusLine) {
    if (!dispOn) return;

    float vbat = readBattVoltage();
    int   pct  = battPercent(vbat);

    unsigned long m  = ms / 60000UL;
    unsigned long s  = (ms % 60000UL) / 1000UL;
    unsigned long cs = (ms % 1000UL) / 10UL;

    // Maximal 7 Zeichen: "9:59.99" → 7 × 18px = 126px, passt ab x=1
    char timeBuf[10];
    snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu.%02lu", m, s, cs);

    char battBuf[20];
    snprintf(battBuf, sizeof(battBuf), "%.2fV  %3d%%  [M]", vbat, pct);

    oled.clearDisplay();

    // Zeit (gross)
    oled.setTextSize(3);
    oled.setCursor(1, 2);
    oled.print(timeBuf);

    // Akku
    oled.setTextSize(1);
    oled.setCursor(0, 30);
    oled.print(battBuf);

    // Trennlinie
    oled.drawLine(0, 41, 127, 41, SSD1306_WHITE);

    // Status
    oled.setCursor(0, 44);
    oled.print(statusLine);

    // Sleep-Hinweis (wann Display aus)
    unsigned long idleSec = (millis() - lastActivityMs) / 1000UL;
    if (idleSec > 10 && state != RUNNING) {
        char hintBuf[22];
        unsigned long secLeft = (LIGHT_SLEEP_MS / 1000UL) - idleSec;
        if (idleSec < LIGHT_SLEEP_MS / 1000UL) {
            snprintf(hintBuf, sizeof(hintBuf), "Sleep in %lus", secLeft);
        } else {
            snprintf(hintBuf, sizeof(hintBuf), "Deep in %lus",
                     (DEEP_SLEEP_MS / 1000UL) - idleSec);
        }
        oled.setCursor(0, 56);
        oled.print(hintBuf);
    }

    oled.display();
}

// ── UDP ───────────────────────────────────────────────────────────
void sendUDP(const char* msg) {
    udp.beginPacket(BROADCAST, UDP_PORT);
    udp.print(msg);
    udp.endPacket();
    Serial.printf("[UDP TX] %s\n", msg);
}

// ── Aktivität ─────────────────────────────────────────────────────
void resetActivity() {
    lastActivityMs = millis();
    lightSlept     = false;
    if (!dispOn) oledPowerOn();
}

// ── Stoppuhr-Aktionen ─────────────────────────────────────────────
void doStart() { state = RUNNING; startMs = millis(); resetActivity(); }
void doStop()  { elapsedMs = millis() - startMs; state = STOPPED; resetActivity(); }
void doReset() { state = IDLE; elapsedMs = 0; resetActivity(); }

// ── Sleep ─────────────────────────────────────────────────────────
void enterLightSleep() {
    Serial.println(F("[SLEEP] Light Sleep"));
    oledPowerOff();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
    esp_light_sleep_start();
    // ─── Aufgewacht ───
    Serial.println(F("[SLEEP] Aufgewacht (Light)"));
    oledPowerOn();
    // Debounce-Zustand zurücksetzen
    debounceTimer = millis();
    rawBtn = lastRawBtn = debouncedBtn = HIGH;
    btnHeld = false;
    resetActivity();
}

void enterDeepSleep() {
    Serial.println(F("[SLEEP] Deep Sleep"));
    sendUDP("DEEPSLEEP");
    delay(100);
    oledPowerOff();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
    esp_deep_sleep_start();
    // kein return
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[MASTER] Boote..."));

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(DISP_PWR_PIN, OUTPUT);
    digitalWrite(DISP_PWR_PIN, HIGH);
    delay(80);

    analogSetPinAttenuation(BATT_PIN, ADC_11db);  // 0–3.9V Messbereich

    Wire.begin(21, 22);
    oledAddr = detectOLED();
    Serial.printf("[I2C] OLED @ 0x%02X\n", oledAddr);

    if (!oled.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
        Serial.println(F("[FEHLER] OLED Init!"));
    }
    oled.setTextColor(SSD1306_WHITE);
    dispOn = true;

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0,  0); oled.print(F("MASTER - WiFi AP"));
    oled.setCursor(0, 12); oled.print(F("SSID: StopwatchNet"));
    oled.setCursor(0, 24); oled.print(F("Starte AP..."));
    oled.display();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(500);
    Serial.printf("[WiFi] AP: %s  IP: %s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());

    oled.setCursor(0, 36); oled.print(F("IP: ")); oled.print(WiFi.softAPIP());
    oled.setCursor(0, 50); oled.print(F("Warte auf Client..."));
    oled.display();

    udp.begin(UDP_PORT);
    Serial.printf("[UDP] Port %d\n", UDP_PORT);

    lastActivityMs = millis();
    delay(1500);
    renderDisplay(0, "Knopf=Start  4s=AUS");
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── Sleep-Timer (nur wenn nicht läuft) ───────────────────────
    if (state != RUNNING) {
        unsigned long idle = now - lastActivityMs;
        if (idle >= DEEP_SLEEP_MS) {
            enterDeepSleep();
        } else if (idle >= LIGHT_SLEEP_MS && !lightSlept) {
            lightSlept = true;
            enterLightSleep();
            now = millis();   // Zeit nach Aufwachen aktualisieren
        }
    }

    // ── Taster entprellen ─────────────────────────────────────────
    rawBtn = digitalRead(BUTTON_PIN);
    if (rawBtn != lastRawBtn) {
        debounceTimer = now;
        lastRawBtn    = rawBtn;
    }
    if ((now - debounceTimer) >= DEBOUNCE_MS && rawBtn != debouncedBtn) {
        debouncedBtn = rawBtn;

        if (debouncedBtn == LOW) {
            // Taste gedrückt → Aktion + Hold-Timer starten
            btnPressMs = now;
            btnHeld    = true;
            resetActivity();

            if (state == IDLE) {
                doStart(); sendUDP("START");
            } else if (state == RUNNING) {
                doStop();
                char msg[32];
                snprintf(msg, sizeof(msg), "STOP:%lu", elapsedMs);
                sendUDP(msg);
            } else if (state == STOPPED) {
                doReset(); sendUDP("RESET");
            }
        } else {
            // Taste losgelassen
            btnHeld = false;
        }
    }

    // ── 4-Sek-Hold → Deep Sleep ───────────────────────────────────
    if (btnHeld && (now - btnPressMs) >= HOLD_DEEP_MS) {
        enterDeepSleep();
    }

    // ── UDP empfangen ─────────────────────────────────────────────
    int pkt = udp.parsePacket();
    if (pkt > 0) {
        char buf[64] = {};
        udp.read(buf, sizeof(buf) - 1);
        String msg(buf);
        Serial.printf("[UDP RX] %s\n", msg.c_str());

        if (msg == "START" && state == IDLE) {
            doStart();
        } else if (msg.startsWith("STOP:") && state == RUNNING) {
            elapsedMs = (unsigned long)msg.substring(5).toInt();
            state = STOPPED; resetActivity();
        } else if (msg == "RESET" && state == STOPPED) {
            doReset();
        }
        // DEEPSLEEP vom Client: wir schlafen auch
        else if (msg == "DEEPSLEEP") {
            enterDeepSleep();
        }
    }

    // ── Display ───────────────────────────────────────────────────
    if      (state == RUNNING) renderDisplay(now - startMs, "LAEUFT   Knopf=Stop");
    else if (state == STOPPED) renderDisplay(elapsedMs,     "STOPP    Knopf=Reset");
    else                       renderDisplay(0,              "Knopf=Start  4s=AUS");

    delay(16);
}
