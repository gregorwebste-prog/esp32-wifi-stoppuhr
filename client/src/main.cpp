/*
 * ESP32 WiFi-Stoppuhr — CLIENT (COM5, verbindet mit Master-AP)
 *
 * Pins:
 *   OLED  SDA=GPIO21  SCL=GPIO22
 *   OLED  VCC → Transistor (BC547) → GPIO26 schaltet
 *   Taster GPIO27 → GND (INPUT_PULLUP)
 *   Akku  GPIO35 (interner 100kΩ/100kΩ Teiler auf Lolin32, JST-Stecker)
 *
 * Verhalten:
 *   - Kurzdruck: Start → Stop → Reset
 *   - 4 Sek halten: Deep Sleep (auch ohne WiFi)
 *   - 10 Min Inaktivität: Deep Sleep (auch ohne WiFi)
 *   - Aufwachen: Tasterdruck
 *   - Kein Master: Offline-Modus, Sleep funktioniert trotzdem
 *   - Deep Sleep: GPIO26 per gpio_hold LOW gehalten → kein Leckverlust
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// ── Pins & Konstanten ─────────────────────────────────────────────
#define BUTTON_PIN     27
#define DISP_PWR_PIN   26   // Transistor-Basis: HIGH=Display an, LOW=aus
#define BATT_PIN       35   // interner 100kΩ/100kΩ Teiler auf Lolin32
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define UDP_PORT     1234
#define DEBOUNCE_MS    40

#define DEEPSLEEP_MS  (20UL * 60UL * 1000UL)
#define HOLD_OFF_MS    4000UL
#define RECONNECT_MS  30000UL

const char*     STA_SSID = "StopwatchNet";
const char*     STA_PASS = "stopwatch123";
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

// ── Taster ────────────────────────────────────────────────────────
bool          rawBtn        = HIGH;
bool          lastRawBtn    = HIGH;
bool          debouncedBtn  = HIGH;
unsigned long debounceTimer = 0;
unsigned long btnPressMs    = 0;
bool          btnHeld       = false;

// ── Timer ─────────────────────────────────────────────────────────
unsigned long lastActivityMs  = 0;
unsigned long lastReconnectMs = 0;
unsigned long lastElapsedMs   = 0;   // letzte abgeschlossene Zeit

// ── Batterie ──────────────────────────────────────────────────────
float readBattVoltage() {
    int raw = analogRead(BATT_PIN);
    return (raw / 4095.0f) * 3.3f * 2.0f;
}

int battPercent(float v) {
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

// Akku-Symbol: 28×10px Körper + 2px Pol, Balken mit 1px Abstand zum Rahmen
void drawBatteryIcon(int x, int y, int pct) {
    oled.drawRect(x, y, 28, 10, SSD1306_WHITE);
    oled.fillRect(x + 28, y + 3, 2, 4, SSD1306_WHITE);
    int bars = (pct >= 75) ? 4 : (pct >= 50) ? 3 : (pct >= 25) ? 2 : (pct >= 10) ? 1 : 0;
    for (int i = 0; i < bars; i++)
        oled.fillRect(x + 2 + i * 6, y + 2, 5, 6, SSD1306_WHITE);
}

void renderDisplay(unsigned long ms) {
    float vbat = readBattVoltage();
    int   pct  = battPercent(vbat);
    bool  wifiOk = (WiFi.status() == WL_CONNECTED);

    unsigned long m  = ms / 60000UL;
    unsigned long s  = (ms % 60000UL) / 1000UL;
    unsigned long cs = (ms % 1000UL) / 10UL;

    char timeBuf[10];
    snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu.%02lu", m, s, cs);

    oled.clearDisplay();

    // Zeit oben (textSize 3 = 24px hoch)
    oled.setTextSize(3);
    oled.setCursor(1, 0);
    oled.print(timeBuf);

    // Strich unter der Zeit
    oled.drawLine(0, 26, 127, 26, SSD1306_WHITE);

    // Letzte Zeit (klein, unter dem Strich)
    oled.setTextSize(1);
    if (lastElapsedMs > 0) {
        unsigned long lm  = lastElapsedMs / 60000UL;
        unsigned long ls  = (lastElapsedMs % 60000UL) / 1000UL;
        unsigned long lcs = (lastElapsedMs % 1000UL) / 10UL;
        char lastBuf[18];
        snprintf(lastBuf, sizeof(lastBuf), "L = %lu:%02lu.%02lu", lm, ls, lcs);
        oled.setCursor(0, 30);
        oled.print(lastBuf);
    }

    // Sleep-Countdown (nur letzte 60s, wenn nicht läuft)
    if (state != RUNNING) {
        unsigned long idleSec = (millis() - lastActivityMs) / 1000UL;
        long secsLeft = (long)(DEEPSLEEP_MS / 1000UL) - (long)idleSec;
        if (secsLeft > 0 && secsLeft <= 60) {
            char hint[18];
            snprintf(hint, sizeof(hint), "Sleep in %lds", secsLeft);
            oled.setCursor(0, 40);
            oled.print(hint);
        }
    }

    // Akku-Symbol (28×10) + % + Volt + [C/!] unten
    drawBatteryIcon(0, 49, pct);
    char battBuf[18];
    snprintf(battBuf, sizeof(battBuf), "%d%%  %.2fV [%s]", pct, vbat, wifiOk ? "C" : "!");
    oled.setCursor(33, 50);
    oled.print(battBuf);

    oled.display();
}

// ── UDP (nur wenn verbunden) ──────────────────────────────────────
void sendUDP(const char* msg) {
    if (WiFi.status() != WL_CONNECTED) return;
    udp.beginPacket(BROADCAST, UDP_PORT);
    udp.print(msg);
    udp.endPacket();
    Serial.printf("[UDP TX] %s\n", msg);
}

// ── Aktionen ──────────────────────────────────────────────────────
void doStart()                { state = RUNNING; startMs = millis(); lastActivityMs = millis(); }
void doStop(unsigned long el) { elapsedMs = el;  state = STOPPED;   lastActivityMs = millis(); }
void doReset()                { lastElapsedMs = elapsedMs; state = IDLE; elapsedMs = 0; lastActivityMs = millis(); }

// ── Deep Sleep ────────────────────────────────────────────────────
void enterDeepSleep() {
    Serial.println(F("[SLEEP] Deep Sleep"));
    sendUDP("DEEPSLEEP");
    delay(100);

    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(200);

    oled.clearDisplay();
    oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
    delay(10);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    // GPIO26 LOW halten im Deep Sleep → kein Strom durch floating Pin
    digitalWrite(DISP_PWR_PIN, LOW);
    gpio_hold_en(GPIO_NUM_26);
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
    esp_deep_sleep_start();
}

// ── WiFi verbinden ────────────────────────────────────────────────
void tryConnectWiFi() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0,  0); oled.print(F("Verbinde mit:"));
    oled.setCursor(0, 10); oled.print(STA_SSID);
    oled.display();

    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASS);

    for (int i = 1; i <= 20; i++) {
        delay(500);
        if (WiFi.status() == WL_CONNECTED) break;
        oled.fillRect(0, 22, 128, 10, SSD1306_BLACK);
        oled.setCursor(0, 22);
        char buf[22];
        snprintf(buf, sizeof(buf), "Versuch %d/20...", i);
        oled.print(buf);
        oled.display();
    }

    oled.clearDisplay();
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setTxPower(WIFI_POWER_5dBm);
        WiFi.setSleep(true);
        udp.begin(UDP_PORT);
        Serial.printf("[WiFi] Verbunden: %s\n", WiFi.localIP().toString().c_str());
        oled.setCursor(0,  0); oled.print(F("Verbunden!"));
        oled.setCursor(0, 14); oled.print(WiFi.localIP());
    } else {
        Serial.println(F("[WiFi] Kein Master — Offline"));
        oled.setCursor(0,  0); oled.print(F("Kein Master!"));
        oled.setCursor(0, 14); oled.print(F("Offline-Modus"));
        oled.setCursor(0, 28); oled.print(F("4s=AUS  10min=AUS"));
    }
    oled.display();
    delay(1200);
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
    // GPIO-Hold vom Deep Sleep aufheben
    gpio_hold_dis(GPIO_NUM_26);
    gpio_deep_sleep_hold_dis();

    Serial.begin(115200);
    delay(100);

    // Deep-Sleep-Nachweis: Aufwachgrund ausgeben
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT0)
        Serial.println(F("\n[CLIENT] Aufgewacht aus Deep Sleep (Taster)"));
    else if (cause == ESP_SLEEP_WAKEUP_UNDEFINED)
        Serial.println(F("\n[CLIENT] Boote... (Kaltstart / Reset)"));
    else
        Serial.printf("\n[CLIENT] Boote... (Wakeup cause: %d)\n", cause);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(DISP_PWR_PIN, OUTPUT);
    digitalWrite(DISP_PWR_PIN, HIGH);   // Display einschalten
    delay(80);

    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(100);

    btStop();
    setCpuFrequencyMhz(80);
    analogSetPinAttenuation(BATT_PIN, ADC_11db);

    Wire.begin(21, 22);
    oledAddr = detectOLED();
    oled.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    oled.setTextColor(SSD1306_WHITE);

    tryConnectWiFi();

    lastActivityMs  = millis();
    lastReconnectMs = millis();
    renderDisplay(0);
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    if (state != RUNNING && (now - lastActivityMs) >= DEEPSLEEP_MS)
        enterDeepSleep();

    // WiFi Reconnect alle 30s (nicht blockierend)
    if (WiFi.status() != WL_CONNECTED && (now - lastReconnectMs) >= RECONNECT_MS) {
        lastReconnectMs = now;
        WiFi.reconnect();
        unsigned long t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 3000) delay(100);
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setTxPower(WIFI_POWER_5dBm);
            WiFi.setSleep(true);
            udp.begin(UDP_PORT);
        }
    }

    rawBtn = digitalRead(BUTTON_PIN);
    if (rawBtn != lastRawBtn) {
        lastRawBtn = rawBtn;
        if ((now - debounceTimer) >= DEBOUNCE_MS) {
            debounceTimer = now;
            debouncedBtn = rawBtn;
            if (debouncedBtn == LOW) {
                btnPressMs = now; btnHeld = true;
                lastActivityMs = now;
                if      (state == IDLE)    { doStart(); sendUDP("START"); }
                else if (state == RUNNING) {
                    unsigned long el = now - startMs;
                    doStop(el);
                    char msg[32];
                    snprintf(msg, sizeof(msg), "STOP:%lu", el);
                    sendUDP(msg);
                }
                else if (state == STOPPED) { doReset(); sendUDP("RESET"); }
            } else { btnHeld = false; }
        }
    }

    if (btnHeld && (now - btnPressMs) >= HOLD_OFF_MS) {
        btnHeld = false;
        enterDeepSleep();
    }

    if (WiFi.status() == WL_CONNECTED) {
        int pkt = udp.parsePacket();
        if (pkt > 0) {
            char buf[64] = {};
            udp.read(buf, sizeof(buf) - 1);
            String msg(buf);
            Serial.printf("[UDP RX] %s\n", msg.c_str());
            if      (msg == "START" && state == IDLE)             doStart();
            else if (msg.startsWith("STOP:") && state == RUNNING) doStop((unsigned long)msg.substring(5).toInt());
            else if (msg == "RESET" && state == STOPPED)          doReset();
            else if (msg == "DEEPSLEEP")                          enterDeepSleep();
        }
    }

    if      (state == RUNNING) renderDisplay(now - startMs);
    else if (state == STOPPED) renderDisplay(elapsedMs);
    else                       renderDisplay(0);

    delay(16);
}
