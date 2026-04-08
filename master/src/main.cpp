/*
 * ESP32 WiFi-Stoppuhr — MASTER (COM9, erstellt Access Point)
 *
 * Pins:
 *   OLED  SDA=GPIO21  SCL=GPIO22
 *   OLED  VCC → Transistor (BC547) → GPIO26 schaltet
 *   Taster GPIO27 → GND (INPUT_PULLUP)
 *   Akku  GPIO35 (interner 100kΩ/100kΩ Teiler auf Lolin32, JST-Stecker)
 *
 * Verhalten:
 *   - Kurzdruck: Start → Stop → Reset
 *   - 4 Sek halten: Deep Sleep
 *   - 10 Min Inaktivität: Deep Sleep
 *   - Aufwachen: Tasterdruck
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
#define DEBOUNCE_MS    60

#define DEEPSLEEP_MS (10UL * 60UL * 1000UL)
#define HOLD_OFF_MS   4000UL

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

// ── Taster ────────────────────────────────────────────────────────
bool          rawBtn        = HIGH;
bool          lastRawBtn    = HIGH;
bool          debouncedBtn  = HIGH;
unsigned long debounceTimer = 0;
unsigned long btnPressMs    = 0;
bool          btnHeld       = false;

// ── Aktivitäts-Timer ──────────────────────────────────────────────
unsigned long lastActivityMs = 0;

// ── Batterie ──────────────────────────────────────────────────────
float readBattVoltage() {
    // GPIO35: interner 100k/100k Teiler → V_bat = V_adc × 2
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

void oledOn() {
    digitalWrite(DISP_PWR_PIN, HIGH);
    delay(80);
    Wire.begin(21, 22);
    oled.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    oled.setTextColor(SSD1306_WHITE);
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
    snprintf(battBuf, sizeof(battBuf), "%.2fV  %3d%%  [M]", vbat, pct);

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

// ── UDP ───────────────────────────────────────────────────────────
void sendUDP(const char* msg) {
    udp.beginPacket(BROADCAST, UDP_PORT);
    udp.print(msg);
    udp.endPacket();
    Serial.printf("[UDP TX] %s\n", msg);
}

// ── Aktionen ──────────────────────────────────────────────────────
void doStart() { state = RUNNING; startMs = millis(); lastActivityMs = millis(); }
void doStop()  { elapsedMs = millis() - startMs; state = STOPPED; lastActivityMs = millis(); }
void doReset() { state = IDLE; elapsedMs = 0; lastActivityMs = millis(); }

// ── Deep Sleep ────────────────────────────────────────────────────
void enterDeepSleep() {
    Serial.println(F("[SLEEP] Deep Sleep"));
    sendUDP("DEEPSLEEP");
    delay(100);

    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    delay(200);

    // Display hardware ausschalten
    oled.clearDisplay();
    oled.display();
    delay(10);

    // WiFi komplett abschalten
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    // GPIO26 LOW halten im Deep Sleep → kein Strom durch floating Pin
    digitalWrite(DISP_PWR_PIN, LOW);
    gpio_hold_en(GPIO_NUM_26);
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
    esp_deep_sleep_start();
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
    // GPIO-Hold vom Deep Sleep aufheben bevor wir den Pin steuern
    gpio_hold_dis(GPIO_NUM_26);
    gpio_deep_sleep_hold_dis();

    Serial.begin(115200);
    delay(100);
    Serial.println(F("\n[MASTER] Boote..."));

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

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0,  0); oled.print(F("MASTER - WiFi AP"));
    oled.setCursor(0, 12); oled.print(F("SSID: StopwatchNet"));
    oled.setCursor(0, 24); oled.print(F("Starte AP..."));
    oled.display();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(500);
    WiFi.setTxPower(WIFI_POWER_5dBm);
    WiFi.setSleep(true);
    Serial.printf("[WiFi] AP: %s  IP: %s  CPU: %luMHz\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str(),
                  getCpuFrequencyMhz());

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0,  0); oled.print(F("MASTER - WiFi AP"));
    oled.setCursor(0, 12); oled.print(F("SSID: StopwatchNet"));
    oled.setCursor(0, 24); oled.print(F("IP: ")); oled.print(WiFi.softAPIP());
    oled.setCursor(0, 36); oled.print(F("Warte auf Client..."));
    oled.display();

    udp.begin(UDP_PORT);
    lastActivityMs = millis();
    delay(1500);
    renderDisplay(0, "Knopf=Start  4s=AUS");
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    if (state != RUNNING && (now - lastActivityMs) >= DEEPSLEEP_MS)
        enterDeepSleep();

    rawBtn = digitalRead(BUTTON_PIN);
    if (rawBtn != lastRawBtn) { debounceTimer = now; lastRawBtn = rawBtn; }
    if ((now - debounceTimer) >= DEBOUNCE_MS && rawBtn != debouncedBtn) {
        debouncedBtn = rawBtn;
        if (debouncedBtn == LOW) {
            btnPressMs = now; btnHeld = true;
            lastActivityMs = now;
            if      (state == IDLE)    { doStart(); sendUDP("START"); }
            else if (state == RUNNING) {
                doStop();
                char msg[32];
                snprintf(msg, sizeof(msg), "STOP:%lu", elapsedMs);
                sendUDP(msg);
            }
            else if (state == STOPPED) { doReset(); sendUDP("RESET"); }
        } else { btnHeld = false; }
    }

    if (btnHeld && (now - btnPressMs) >= HOLD_OFF_MS) {
        btnHeld = false;
        enterDeepSleep();
    }

    int pkt = udp.parsePacket();
    if (pkt > 0) {
        char buf[64] = {};
        udp.read(buf, sizeof(buf) - 1);
        String msg(buf);
        Serial.printf("[UDP RX] %s\n", msg.c_str());
        if      (msg == "START" && state == IDLE)            doStart();
        else if (msg.startsWith("STOP:") && state == RUNNING) {
            elapsedMs = (unsigned long)msg.substring(5).toInt();
            state = STOPPED; lastActivityMs = millis();
        }
        else if (msg == "RESET" && state == STOPPED)         doReset();
        else if (msg == "DEEPSLEEP")                         enterDeepSleep();
    }

    if      (state == RUNNING) renderDisplay(now - startMs, "LAEUFT   Knopf=Stop");
    else if (state == STOPPED) renderDisplay(elapsedMs,     "STOPP    Knopf=Reset");
    else                       renderDisplay(0,              "Knopf=Start  4s=AUS");

    delay(16);
}
