/*
 * ESP32 WiFi-Stoppuhr — MASTER (COM5, erstellt Access Point)
 *
 * Hardware:
 *   - OLED 0.96" SSD1306 128x64  → SDA=GPIO21, SCL=GPIO22
 *   - Taster                      → GPIO27 → GND (interner Pull-up aktiv)
 *
 * Protokoll:
 *   Master erstellt WiFi-AP "StopwatchNet" / PW "stopwatch123"
 *   Client verbindet sich damit.
 *   UDP-Broadcast auf Port 1234: Nachrichten "START:<millis>" und "STOP:<elapsed>"
 *   Beide ESPs synchronisieren Start-Zeitstempel, damit die Anzeigen übereinstimmen.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Konfiguration ────────────────────────────────────────────────────────────
#define BUTTON_PIN    27
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define UDP_PORT      1234

const char* AP_SSID = "StopwatchNet";
const char* AP_PASS = "stopwatch123";
const IPAddress BROADCAST(192, 168, 4, 255);

// ── Objekte ──────────────────────────────────────────────────────────────────
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiUDP           udp;

// ── Zustand ──────────────────────────────────────────────────────────────────
enum State { IDLE, RUNNING, STOPPED };
State         state        = IDLE;
unsigned long startRefMs   = 0;   // millis()-Zeitpunkt des Starts
unsigned long elapsedMs    = 0;   // gespeicherte Zeit nach Stop

// Taster-Entprellung
bool          lastBtn      = HIGH;
unsigned long lastDebounce = 0;
const unsigned long DEBOUNCE = 50;

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────

void showDisplay(unsigned long ms, const char* statusLine) {
    unsigned long h   =  ms / 3600000UL;
    unsigned long m   = (ms % 3600000UL) / 60000UL;
    unsigned long s   = (ms %   60000UL) /  1000UL;
    unsigned long cs  = (ms %    1000UL) /    10UL;   // Hundertstel

    oled.clearDisplay();

    // Titel
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(28, 0);
    oled.print("=== STOPPUHR ===");

    // Trennlinie
    oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    // Zeit gross
    char buf[16];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%02lu", h, m, s, cs);
    else
        snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", m, s, cs);

    oled.setTextSize(2);
    oled.setCursor(h > 0 ? 0 : 10, 20);
    oled.print(buf);

    // Status
    oled.setTextSize(1);
    oled.setCursor(0, 50);
    oled.print(statusLine);

    // Rolle
    oled.setCursor(90, 56);
    oled.print("[MASTER]");

    oled.display();
}

void sendUDP(const char* msg) {
    udp.beginPacket(BROADCAST, UDP_PORT);
    udp.print(msg);
    udp.endPacket();
    Serial.printf("[UDP] Gesendet: %s\n", msg);
}

void handleStart(unsigned long refMs) {
    state      = RUNNING;
    startRefMs = refMs;
    Serial.printf("[START] refMs=%lu\n", refMs);
}

void handleStop(unsigned long elapsed) {
    state     = STOPPED;
    elapsedMs = elapsed;
    Serial.printf("[STOP] elapsed=%lu\n", elapsed);
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[MASTER] Boote...");

    // Button
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // OLED
    Wire.begin(21, 22);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[FEHLER] OLED nicht gefunden!");
        while (true) delay(500);
    }
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    // Startscreen
    oled.setTextSize(1);
    oled.setCursor(10, 10); oled.print("WiFi AP starten...");
    oled.display();

    // WiFi Access Point
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[WiFi] AP: %s  IP: %s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());

    // UDP
    udp.begin(UDP_PORT);
    Serial.printf("[UDP] Hoere auf Port %d\n", UDP_PORT);

    oled.clearDisplay();
    oled.setCursor(0, 0);  oled.print("AP: "); oled.print(AP_SSID);
    oled.setCursor(0, 12); oled.print("IP: "); oled.print(WiFi.softAPIP());
    oled.setCursor(0, 28); oled.setTextSize(1);
    oled.print("Warte auf Client...");
    oled.setCursor(0, 50); oled.print("Knopf = Start/Stop");
    oled.display();

    delay(2000);
    showDisplay(0, "Bereit — Knopf druecken");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── UDP empfangen ──────────────────────────────────────────────────────
    int pktSize = udp.parsePacket();
    if (pktSize > 0) {
        char buf[64];
        int len = udp.read(buf, sizeof(buf) - 1);
        buf[len] = '\0';
        String msg(buf);
        Serial.printf("[UDP] Empfangen: %s\n", msg.c_str());

        if (msg.startsWith("START:")) {
            unsigned long ref = (unsigned long)msg.substring(6).toInt();
            handleStart(ref);
        } else if (msg.startsWith("STOP:")) {
            unsigned long el = (unsigned long)msg.substring(5).toInt();
            handleStop(el);
        }
    }

    // ── Taster lesen + entprellen ──────────────────────────────────────────
    bool btnRaw = digitalRead(BUTTON_PIN);
    if (btnRaw != lastBtn) lastDebounce = now;
    if ((now - lastDebounce) > DEBOUNCE && btnRaw == LOW && lastBtn == HIGH) {
        // Flanke: gedrückt
        if (state == IDLE || state == STOPPED) {
            // START
            unsigned long ref = now;
            handleStart(ref);
            char msg[32];
            snprintf(msg, sizeof(msg), "START:%lu", ref);
            sendUDP(msg);
        } else if (state == RUNNING) {
            // STOP
            unsigned long el = now - startRefMs;
            handleStop(el);
            char msg[32];
            snprintf(msg, sizeof(msg), "STOP:%lu", el);
            sendUDP(msg);
        }
    }
    lastBtn = btnRaw;

    // ── Display aktualisieren ──────────────────────────────────────────────
    if (state == RUNNING) {
        unsigned long elapsed = now - startRefMs;
        showDisplay(elapsed, "LAEUFT  [Knopf=Stop]");
    } else if (state == STOPPED) {
        showDisplay(elapsedMs, "GESTOPPT [Knopf=Reset]");
    } else {
        showDisplay(0, "Bereit  [Knopf=Start]");
    }

    delay(16);  // ~60 FPS
}
