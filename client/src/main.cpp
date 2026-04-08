/*
 * ESP32 WiFi-Stoppuhr — CLIENT (COM8, verbindet sich mit Master-AP)
 *
 * Hardware:
 *   - OLED 0.96" SSD1306 128x64  → SDA=GPIO21, SCL=GPIO22
 *   - Taster                      → GPIO27 → GND (interner Pull-up aktiv)
 *
 * Protokoll:
 *   Verbindet sich mit AP "StopwatchNet" des Masters.
 *   UDP-Broadcast auf Port 1234: Nachrichten "START:<millis>" und "STOP:<elapsed>"
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

const char* STA_SSID = "StopwatchNet";
const char* STA_PASS = "stopwatch123";
const IPAddress BROADCAST(192, 168, 4, 255);

// ── Objekte ──────────────────────────────────────────────────────────────────
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiUDP           udp;

// ── Zustand ──────────────────────────────────────────────────────────────────
enum State { IDLE, RUNNING, STOPPED };
State         state        = IDLE;
unsigned long startRefMs   = 0;
unsigned long elapsedMs    = 0;

// Taster-Entprellung
bool          lastBtn      = HIGH;
unsigned long lastDebounce = 0;
const unsigned long DEBOUNCE = 50;

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────

void showDisplay(unsigned long ms, const char* statusLine) {
    unsigned long h   =  ms / 3600000UL;
    unsigned long m   = (ms % 3600000UL) / 60000UL;
    unsigned long s   = (ms %   60000UL) /  1000UL;
    unsigned long cs  = (ms %    1000UL) /    10UL;

    oled.clearDisplay();

    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(28, 0);
    oled.print("=== STOPPUHR ===");

    oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    char buf[16];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%02lu", h, m, s, cs);
    else
        snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", m, s, cs);

    oled.setTextSize(2);
    oled.setCursor(h > 0 ? 0 : 10, 20);
    oled.print(buf);

    oled.setTextSize(1);
    oled.setCursor(0, 50);
    oled.print(statusLine);

    oled.setCursor(90, 56);
    oled.print("[CLIENT]");

    oled.display();
}

void sendUDP(const char* msg) {
    udp.beginPacket(BROADCAST, UDP_PORT);
    udp.print(msg);
    udp.endPacket();
    Serial.printf("[UDP] Gesendet: %s\n", msg);
}

void handleStart(unsigned long refMs) {
    // Netzwerklatenz kompensieren: eigene millis() als Referenz nehmen
    // (beide starten quasi gleichzeitig, Latenz <5ms im lokalen AP)
    state      = RUNNING;
    startRefMs = refMs;
    Serial.printf("[START] refMs=%lu\n", refMs);
}

void handleStop(unsigned long elapsed) {
    state     = STOPPED;
    elapsedMs = elapsed;
    Serial.printf("[STOP] elapsed=%lu\n", elapsed);
}

void connectWiFi() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0); oled.print("Verbinde mit:");
    oled.setCursor(0, 12); oled.print(STA_SSID);
    oled.display();

    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        attempts++;
        oled.setCursor(0, 28);
        oled.printf("Versuch %d/40...", attempts);
        oled.display();
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Verbunden! IP: %s\n",
                      WiFi.localIP().toString().c_str());
        oled.clearDisplay();
        oled.setCursor(0, 0);  oled.print("Verbunden!");
        oled.setCursor(0, 12); oled.print("IP: "); oled.print(WiFi.localIP());
        oled.display();
        delay(1500);
    } else {
        Serial.println("\n[FEHLER] WiFi-Verbindung fehlgeschlagen!");
        oled.clearDisplay();
        oled.setCursor(0, 0);  oled.print("FEHLER: Kein WiFi!");
        oled.setCursor(0, 16); oled.print("Bitte Master");
        oled.setCursor(0, 26); oled.print("zuerst starten.");
        oled.display();
        delay(3000);
    }
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[CLIENT] Boote...");

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Wire.begin(21, 22);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[FEHLER] OLED nicht gefunden!");
        while (true) delay(500);
    }
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    connectWiFi();

    udp.begin(UDP_PORT);
    Serial.printf("[UDP] Hoere auf Port %d\n", UDP_PORT);

    showDisplay(0, "Bereit  [Knopf=Start]");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // WiFi-Reconnect falls getrennt
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Verbindung verloren, reconnecte...");
        connectWiFi();
        udp.begin(UDP_PORT);
    }

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
            // Verwende lokale millis() als Referenz (vermeidet millis()-Offset zwischen ESPs)
            handleStart(now - 0);  // beide starten von "jetzt" aus
            startRefMs = now;
        } else if (msg.startsWith("STOP:")) {
            unsigned long el = (unsigned long)msg.substring(5).toInt();
            handleStop(el);
        }
    }

    // ── Taster lesen + entprellen ──────────────────────────────────────────
    bool btnRaw = digitalRead(BUTTON_PIN);
    if (btnRaw != lastBtn) lastDebounce = now;
    if ((now - lastDebounce) > DEBOUNCE && btnRaw == LOW && lastBtn == HIGH) {
        if (state == IDLE || state == STOPPED) {
            unsigned long ref = now;
            handleStart(ref);
            startRefMs = now;
            char msg[32];
            snprintf(msg, sizeof(msg), "START:%lu", ref);
            sendUDP(msg);
        } else if (state == RUNNING) {
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
        showDisplay(now - startRefMs, "LAEUFT  [Knopf=Stop]");
    } else if (state == STOPPED) {
        showDisplay(elapsedMs, "GESTOPPT [Knopf=Reset]");
    } else {
        showDisplay(0, "Bereit  [Knopf=Start]");
    }

    delay(16);
}
