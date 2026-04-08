/*
 * ESP32 WiFi-Stoppuhr — CLIENT (COM5, verbindet mit Master-AP)
 *
 * Pins:
 *   OLED  SDA=GPIO21  SCL=GPIO22
 *   OLED  VCC=GPIO26  (direkt am Pin, HIGH=an / LOW=aus)
 *   Taster GPIO27 → GND (INPUT_PULLUP)
 *   Akku  47kΩ/47kΩ Teiler → GPIO33 (ADC1_CH5)
 *
 * Verhalten:
 *   - Kurzdruck: Start → Stop → Reset
 *   - 4 Sek halten: Deep Sleep (auch ohne WiFi-Verbindung)
 *   - 10 Min Inaktivität: Deep Sleep (auch ohne WiFi-Verbindung)
 *   - Aufwachen: Tasterdruck
 *   - Kein Master: Offline-Modus, alle Sleep-Funktionen laufen trotzdem
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
#define DISP_PWR_PIN   26
#define BATT_PIN       35   // GPIO35 = eingebauter 100k/100k Teiler auf Lolin32
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define UDP_PORT     1234
#define DEBOUNCE_MS    60

#define DEEPSLEEP_MS    (10UL * 60UL * 1000UL)  // 10 Min → Deep Sleep
#define HOLD_OFF_MS     4000UL                   //  4 Sek halten → Deep Sleep
#define RECONNECT_MS   30000UL                   // 30 Sek zwischen Reconnect-Versuchen

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

void renderDisplay(unsigned long ms, const char* statusLine) {
    float vbat = readBattVoltage();
    int   pct  = battPercent(vbat);

    unsigned long m  = ms / 60000UL;
    unsigned long s  = (ms % 60000UL) / 1000UL;
    unsigned long cs = (ms % 1000UL) / 10UL;

    char timeBuf[10];
    snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu.%02lu", m, s, cs);

    char battBuf[20];
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    snprintf(battBuf, sizeof(battBuf), "%.2fV %3d%% [%s]",
             vbat, pct, wifiOk ? "C" : "!!");

    oled.clearDisplay();
    oled.setTextSize(3);
    oled.setCursor(1, 2);
    oled.print(timeBuf);

    oled.setTextSize(1);
    oled.setCursor(0, 30);
    oled.print(battBuf);

    oled.drawLine(0, 41, 127, 41, SSD1306_WHITE);

    oled.setCursor(0, 44);
    oled.print(statusLine);

    // Sleep-Countdown (letzte 60 Sekunden)
    if (state != RUNNING) {
        unsigned long idleSec = (millis() - lastActivityMs) / 1000UL;
        long secsLeft = (long)(DEEPSLEEP_MS / 1000UL) - (long)idleSec;
        if (secsLeft > 0 && secsLeft <= 60) {
            char hint[22];
            snprintf(hint, sizeof(hint), "Sleep in %lds", secsLeft);
            oled.setCursor(0, 56);
            oled.print(hint);
        }
    }

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
void doStart()           { state = RUNNING; startMs = millis(); lastActivityMs = millis(); }
void doStop(unsigned long el) { elapsedMs = el; state = STOPPED; lastActivityMs = millis(); }
void doReset()           { state = IDLE; elapsedMs = 0; lastActivityMs = millis(); }

// ── Deep Sleep ────────────────────────────────────────────────────
void enterDeepSleep() {
    Serial.println(F("[SLEEP] Deep Sleep"));
    sendUDP("DEEPSLEEP");     // kein Problem wenn nicht verbunden
    delay(100);

    // Warten bis Taste los → kein Sofort-Aufwachen
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(200);

    // Display aus
    oled.clearDisplay();
    oled.display();
    delay(20);

    // WiFi komplett abschalten
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    // GPIO26 (Display-VCC) LOW festhalten — verhindert Strom durch floating Pin
    digitalWrite(DISP_PWR_PIN, LOW);
    gpio_hold_en(GPIO_NUM_26);
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
    esp_deep_sleep_start();
}

// ── WiFi verbinden (non-blocking nach Timeout) ────────────────────
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
        // Zeile sauber überschreiben
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
        Serial.println(F("[WiFi] Kein Master — Offline-Modus"));
        oled.setCursor(0,  0); oled.print(F("Kein Master!"));
        oled.setCursor(0, 14); oled.print(F("Offline-Modus"));
        oled.setCursor(0, 28); oled.print(F("4s=AUS  10min=AUS"));
    }
    oled.display();
    delay(1200);
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
    // GPIO-Hold vom letzten Deep Sleep aufheben
    gpio_hold_dis(GPIO_NUM_26);
    gpio_deep_sleep_hold_dis();

    Serial.begin(115200);
    delay(100);
    Serial.println(F("\n[CLIENT] Boote..."));

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(DISP_PWR_PIN, OUTPUT);
    digitalWrite(DISP_PWR_PIN, HIGH);
    delay(80);

    // Warten bis Taste los
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(100);

    btStop();
    setCpuFrequencyMhz(80);
    analogSetPinAttenuation(BATT_PIN, ADC_11db);  // bis ~3.9V Eingang

    Wire.begin(21, 22);
    oledAddr = detectOLED();
    oled.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    oled.setTextColor(SSD1306_WHITE);

    tryConnectWiFi();

    lastActivityMs  = millis();
    lastReconnectMs = millis();
    renderDisplay(0, "Knopf=Start  4s=AUS");
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── Deep Sleep Timer ──────────────────────────────────────────
    if (state != RUNNING && (now - lastActivityMs) >= DEEPSLEEP_MS) {
        enterDeepSleep();
    }

    // ── WiFi Reconnect (alle 30 Sek, nicht blockierend) ───────────
    if (WiFi.status() != WL_CONNECTED && (now - lastReconnectMs) >= RECONNECT_MS) {
        lastReconnectMs = now;
        Serial.println(F("[WiFi] Reconnect..."));
        WiFi.reconnect();
        // Kurz warten ob es klappt
        unsigned long t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 3000) delay(100);
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setTxPower(WIFI_POWER_5dBm);
            WiFi.setSleep(true);
            udp.begin(UDP_PORT);
            Serial.println(F("[WiFi] Reconnect OK"));
        }
    }

    // ── Taster entprellen ─────────────────────────────────────────
    rawBtn = digitalRead(BUTTON_PIN);
    if (rawBtn != lastRawBtn) { debounceTimer = now; lastRawBtn = rawBtn; }
    if ((now - debounceTimer) >= DEBOUNCE_MS && rawBtn != debouncedBtn) {
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
        } else {
            btnHeld = false;
        }
    }

    // ── 4-Sek-Hold → Deep Sleep ───────────────────────────────────
    if (btnHeld && (now - btnPressMs) >= HOLD_OFF_MS) {
        btnHeld = false;
        enterDeepSleep();
    }

    // ── UDP empfangen ─────────────────────────────────────────────
    if (WiFi.status() == WL_CONNECTED) {
        int pkt = udp.parsePacket();
        if (pkt > 0) {
            char buf[64] = {};
            udp.read(buf, sizeof(buf) - 1);
            String msg(buf);
            Serial.printf("[UDP RX] %s\n", msg.c_str());
            if (msg == "START" && state == IDLE) {
                doStart();
            } else if (msg.startsWith("STOP:") && state == RUNNING) {
                doStop((unsigned long)msg.substring(5).toInt());
            } else if (msg == "RESET" && state == STOPPED) {
                doReset();
            } else if (msg == "DEEPSLEEP") {
                enterDeepSleep();
            }
        }
    }

    // ── Display ───────────────────────────────────────────────────
    if      (state == RUNNING) renderDisplay(now - startMs, "LAEUFT   Knopf=Stop");
    else if (state == STOPPED) renderDisplay(elapsedMs,     "STOPP    Knopf=Reset");
    else                       renderDisplay(0,              "Knopf=Start  4s=AUS");

    delay(16);
}
