/*
 * ESP32 WiFi-Stoppuhr — CLIENT (COM5, verbindet mit Master-AP)
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

#define LIGHT_SLEEP_MS  ( 1UL * 60UL * 1000UL)   //  1 Minute
#define DEEP_SLEEP_MS   (10UL * 60UL * 1000UL)   // 10 Minuten
#define HOLD_DEEP_MS    4000UL                    //  4 Sek halten

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
    int raw = analogRead(BATT_PIN);
    float v_adc = (raw / 4095.0f) * 3.3f;
    return v_adc * 2.0f;
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

    char timeBuf[10];
    snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu.%02lu", m, s, cs);

    char battBuf[20];
    snprintf(battBuf, sizeof(battBuf), "%.2fV  %3d%%  [C]", vbat, pct);

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
void doStart()           { state = RUNNING; startMs = millis(); resetActivity(); }
void doStop(unsigned long el) { elapsedMs = el; state = STOPPED; resetActivity(); }
void doReset()           { state = IDLE; elapsedMs = 0; resetActivity(); }

// ── Sleep ─────────────────────────────────────────────────────────
void enterLightSleep() {
    Serial.println(F("[SLEEP] Light Sleep"));
    oledPowerOff();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
    esp_light_sleep_start();
    // ─── Aufgewacht ───
    Serial.println(F("[SLEEP] Aufgewacht (Light)"));
    // Nach Light Sleep: WiFi-Reconnect prüfen
    if (WiFi.status() != WL_CONNECTED) {
        // Kurz warten, AP könnte gerade hochfahren
        delay(500);
    }
    oledPowerOn();
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
}

// ── WiFi verbinden ────────────────────────────────────────────────
void connectWiFi() {
    if (!dispOn) oledPowerOn();
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0,  0); oled.print(F("CLIENT - Verbinde"));
    oled.setCursor(0, 12); oled.print(STA_SSID);
    oled.display();

    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASS);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        tries++;
        oled.setCursor(0, 28);
        char buf[20];
        snprintf(buf, sizeof(buf), "Versuch %d/40...", tries);
        oled.print(buf);
        oled.display();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Verbunden! IP: %s\n",
                      WiFi.localIP().toString().c_str());
        oled.clearDisplay();
        oled.setCursor(0,  0); oled.print(F("Verbunden!"));
        oled.setCursor(0, 14); oled.print(WiFi.localIP());
        oled.display();
        delay(1000);
    } else {
        Serial.println(F("\n[FEHLER] WiFi fehlgeschlagen!"));
        oled.clearDisplay();
        oled.setCursor(0,  0); oled.print(F("KEIN WIFI!"));
        oled.setCursor(0, 14); oled.print(F("Master einschalten"));
        oled.display();
        delay(2000);
    }
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[CLIENT] Boote..."));

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(DISP_PWR_PIN, OUTPUT);
    digitalWrite(DISP_PWR_PIN, HIGH);
    delay(80);

    analogSetPinAttenuation(BATT_PIN, ADC_11db);

    Wire.begin(21, 22);
    oledAddr = detectOLED();
    Serial.printf("[I2C] OLED @ 0x%02X\n", oledAddr);

    if (!oled.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
        Serial.println(F("[FEHLER] OLED Init!"));
    }
    oled.setTextColor(SSD1306_WHITE);
    dispOn = true;

    connectWiFi();

    udp.begin(UDP_PORT);
    Serial.printf("[UDP] Port %d\n", UDP_PORT);

    lastActivityMs = millis();
    renderDisplay(0, "Knopf=Start  4s=AUS");
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── WiFi-Reconnect ────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[WiFi] Getrennt, reconnecte..."));
        connectWiFi();
        udp.begin(UDP_PORT);
        return;
    }

    // ── Sleep-Timer ───────────────────────────────────────────────
    if (state != RUNNING) {
        unsigned long idle = now - lastActivityMs;
        if (idle >= DEEP_SLEEP_MS) {
            enterDeepSleep();
        } else if (idle >= LIGHT_SLEEP_MS && !lightSlept) {
            lightSlept = true;
            enterLightSleep();
            now = millis();
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
            btnPressMs = now;
            btnHeld    = true;
            resetActivity();

            if (state == IDLE) {
                doStart(); sendUDP("START");
            } else if (state == RUNNING) {
                unsigned long el = now - startMs;
                doStop(el);
                char msg[32];
                snprintf(msg, sizeof(msg), "STOP:%lu", el);
                sendUDP(msg);
            } else if (state == STOPPED) {
                doReset(); sendUDP("RESET");
            }
        } else {
            btnHeld = false;
        }
    }

    // ── 4-Sek-Hold → Neustart ────────────────────────────────────
    if (btnHeld && (now - btnPressMs) >= HOLD_DEEP_MS) {
        sendUDP("RESET");
        delay(100);
        ESP.restart();
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
            unsigned long el = (unsigned long)msg.substring(5).toInt();
            doStop(el);
        } else if (msg == "RESET" && state == STOPPED) {
            doReset();
        } else if (msg == "DEEPSLEEP") {
            enterDeepSleep();
        }
    }

    // ── Display ───────────────────────────────────────────────────
    if      (state == RUNNING) renderDisplay(now - startMs, "LAEUFT   Knopf=Stop");
    else if (state == STOPPED) renderDisplay(elapsedMs,     "STOPP    Knopf=Reset");
    else                       renderDisplay(0,              "Knopf=Start  4s=AUS");

    delay(16);
}
