#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include "INA226.h"

// ============================================================
// KONFIGURATION
// ============================================================

const char* HOSTNAME = "WohnwagenMonitor";

#define I2C_SDA 22
#define I2C_SCL 21

#define INA226_ADDRESS 0x40

// ------------------------------------------------------------
// Batterie
// ------------------------------------------------------------

const char* BATTERY_NAME = "Wohnwagen Batterie";

const float BATTERY_CAPACITY_AH = 100.0f;
const float MAX_CURRENT_A       = 500.0f;
const float SHUNT_MV            = 75.0f;

// 500 A / 75 mV -> 0.00015 Ohm = 150 µOhm
const float SHUNT_RESISTANCE_OHM =
    (SHUNT_MV / 1000.0f) / MAX_CURRENT_A;

// ------------------------------------------------------------
// Stromrichtung
// ------------------------------------------------------------
//
// +1:
//   positiver Strom = Entladung
//   negativer Strom = Ladung
//
// -1:
//   positiver Strom = Ladung
//   negativer Strom = Entladung
//

const int CURRENT_DIRECTION = -1;

// ------------------------------------------------------------
// Stromschwellen
// ------------------------------------------------------------

const float IDLE_CURRENT_A        = 0.20f;
const float CHARGING_CURRENT_A    = 0.50f;
const float DISCHARGING_CURRENT_A = 0.50f;

// Mindeststrom für die Zeitprognose, auch im Ruhezustand.
const float ESTIMATION_MIN_CURRENT_A = 0.01f;

// ------------------------------------------------------------
// Vollerkennung
// ------------------------------------------------------------

const float FULL_VOLTAGE        = 14.20f;
const float FULL_CHARGE_CURRENT = 2.00f;

const unsigned long FULL_CONFIRM_TIME_MS = 60000UL;

// Unter dieser Spannung wird der Full-Zustand wieder freigegeben.
const float FULL_RELEASE_VOLTAGE = 13.80f;

// ------------------------------------------------------------
// Spannungswarnungen
// ------------------------------------------------------------

const float LOW_VOLTAGE_WARNING = 12.00f;
const float CRITICAL_VOLTAGE    = 11.50f;

// ------------------------------------------------------------
// Spannungsbasierte SoC-Kurve
// ------------------------------------------------------------

struct CurvePoint
{
    float voltage;
    float soc;
};

const CurvePoint LIFEPO4_12V_CURVE[] =
{
    {10.0f,  0.0f},
    {11.4f, 10.0f},
    {11.6f, 20.0f},
    {11.8f, 30.0f},
    {12.0f, 40.0f},
    {12.4f, 50.0f},
    {12.8f, 60.0f},
    {13.2f, 70.0f},
    {13.6f, 80.0f},
    {14.0f, 90.0f},
    {14.6f, 100.0f}
};

const size_t LIFEPO4_CURVE_POINTS =
    sizeof(LIFEPO4_12V_CURVE) /
    sizeof(LIFEPO4_12V_CURVE[0]);

// ------------------------------------------------------------
// Messintervalle
// ------------------------------------------------------------

const unsigned long MEASUREMENT_INTERVAL_MS = 1000UL;
const unsigned long SAVE_INTERVAL_MS        = 300000UL;

// 5 Minuten bei 1 Messung/Sekunde
const size_t CURRENT_HISTORY_SIZE = 300;

// ============================================================
// OBJEKTE
// ============================================================

AsyncWebServer server(80);
Preferences preferences;
INA226 INA0(INA226_ADDRESS);

// ============================================================
// WIFI
// ============================================================

String wifiSSID;
String wifiPassword;

bool wifiAPMode = false;

const char* AP_SSID     = "WohnwagenMonitor";
const char* AP_PASSWORD = "12345678";

// ============================================================
// WLAN SCAN
// ============================================================
//
// Der Scan wird NICHT innerhalb eines AsyncWebServer-Callbacks
// ausgeführt.
//
// Der HTTP-Handler setzt lediglich wifiScanRequested.
// Der eigentliche Scan wird in loop() gestartet.
//
// ============================================================

volatile bool wifiScanRequested = false;

bool wifiScanRunning  = false;
bool wifiScanFinished = false;

int wifiScanResultCount = -1;

// Zeit des letzten Scanstarts.
// Verhindert, dass durch versehentliche mehrfache
// HTTP-Anfragen unmittelbar neue Scans gestartet werden.
unsigned long wifiScanLastFinish = 0;

const unsigned long WIFI_SCAN_COOLDOWN_MS = 3000UL;

// ============================================================
// MESSWERTE
// ============================================================

float batteryVoltage = 0.0f;
float batteryCurrent = 0.0f;
float batteryPower   = 0.0f;
float batteryShuntVoltage = 0.0f;

bool ina226Available = false;

// ============================================================
// SOC / AH
// ============================================================

float ampHoursUsed = 0.0f;

float socCoulomb = 100.0f;
float socVoltage = 0.0f;
float soc        = 100.0f;

// ============================================================
// STROMMITTELWERTE
// ============================================================

float currentHistory[CURRENT_HISTORY_SIZE];

size_t currentHistoryIndex = 0;
size_t currentHistoryCount = 0;

float averageDischargeCurrent = 0.0f;
float averageChargeCurrent    = 0.0f;

// ============================================================
// ZEITBERECHNUNG
// ============================================================

float timeToEmptyHours = -1.0f;
float timeToFullHours  = -1.0f;

// ============================================================
// BATTERIEZUSTAND
// ============================================================

enum BatteryState
{
    STATE_IDLE,
    STATE_CHARGING,
    STATE_DISCHARGING
};

BatteryState batteryState = STATE_IDLE;

// ============================================================
// FULL DETECTION
// ============================================================

bool fullDetected = false;

unsigned long fullConditionStart = 0;

// ============================================================
// TIMER
// ============================================================

unsigned long lastMeasurement = 0;
unsigned long lastSave        = 0;

unsigned long lastCoulombUpdate = 0;

// ============================================================
// RESTART-VERWALTUNG
// ============================================================

bool restartRequested = false;
unsigned long restartAt = 0;

// ============================================================
// FUNKTIONSDEKLARATIONEN
// ============================================================

float clampFloat(
    float value,
    float minimum,
    float maximum
);

bool isValidFloat(float value);

float calculateVoltageSOC(float voltage);

float calculateCoulombSOC();

void updateCurrentHistory(float current);

void calculateAverageCurrents();

void updateBatteryState();

void updateTimeEstimates();

void checkFullDetection();

void updateCoulombCounting();

void saveBatteryState();

void loadBatteryState();

void setBatteryFull();

bool setupINA226();

bool readINA226();

void setupWiFi();

void setupWebServer();

void createStateJSON(JsonDocument& doc);

String getBatteryStateString();

void requestRestart(
    unsigned long delayMs = 500
);

void processWiFiScan();

// ============================================================
// HILFSFUNKTIONEN
// ============================================================

float clampFloat(
    float value,
    float minimum,
    float maximum
)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}

// ------------------------------------------------------------

bool isValidFloat(float value)
{
    return isfinite(value);
}

// ============================================================
// SPANNUNGS-SOC
// ============================================================

float calculateVoltageSOC(float voltage)
{
    if (!isValidFloat(voltage))
        return 0.0f;

    if (
        voltage <=
        LIFEPO4_12V_CURVE[0].voltage
    )
    {
        return LIFEPO4_12V_CURVE[0].soc;
    }

    if (
        voltage >=
        LIFEPO4_12V_CURVE[
            LIFEPO4_CURVE_POINTS - 1
        ].voltage
    )
    {
        return LIFEPO4_12V_CURVE[
            LIFEPO4_CURVE_POINTS - 1
        ].soc;
    }

    for (
        size_t i = 1;
        i < LIFEPO4_CURVE_POINTS;
        i++
    )
    {
        const CurvePoint& p1 =
            LIFEPO4_12V_CURVE[i - 1];

        const CurvePoint& p2 =
            LIFEPO4_12V_CURVE[i];

        if (voltage <= p2.voltage)
        {
            float voltageRange =
                p2.voltage - p1.voltage;

            if (voltageRange <= 0.0f)
                return p1.soc;

            float factor =
                (voltage - p1.voltage) /
                voltageRange;

            return
                p1.soc +
                factor *
                (p2.soc - p1.soc);
        }
    }

    return 0.0f;
}

// ============================================================
// COULOMB SOC
// ============================================================

float calculateCoulombSOC()
{
    float remainingAh =
        BATTERY_CAPACITY_AH -
        ampHoursUsed;

    remainingAh =
        clampFloat(
            remainingAh,
            0.0f,
            BATTERY_CAPACITY_AH
        );

    return
        (remainingAh /
         BATTERY_CAPACITY_AH) *
        100.0f;
}

// ============================================================
// STROMHISTORIE
// ============================================================

void updateCurrentHistory(float current)
{
    if (!isValidFloat(current))
        return;

    currentHistory[currentHistoryIndex] =
        current;

    currentHistoryIndex++;

    if (
        currentHistoryIndex >=
        CURRENT_HISTORY_SIZE
    )
    {
        currentHistoryIndex = 0;
    }

    if (
        currentHistoryCount <
        CURRENT_HISTORY_SIZE
    )
    {
        currentHistoryCount++;
    }
}

// ============================================================
// DURCHSCHNITTSSTRÖME
// ============================================================

void calculateAverageCurrents()
{
    float dischargeSum = 0.0f;
    size_t dischargeCount = 0;

    float chargeSum = 0.0f;
    size_t chargeCount = 0;

    for (
        size_t i = 0;
        i < currentHistoryCount;
        i++
    )
    {
        float current =
            currentHistory[i];

        if (
            current >
            DISCHARGING_CURRENT_A
        )
        {
            dischargeSum += current;
            dischargeCount++;
        }
        else if (
            current <
            -CHARGING_CURRENT_A
        )
        {
            chargeSum += fabs(current);
            chargeCount++;
        }
    }

    if (dischargeCount > 0)
    {
        averageDischargeCurrent =
            dischargeSum /
            dischargeCount;
    }
    else
    {
        averageDischargeCurrent =
            0.0f;
    }

    if (chargeCount > 0)
    {
        averageChargeCurrent =
            chargeSum /
            chargeCount;
    }
    else
    {
        averageChargeCurrent =
            0.0f;
    }
}

// ============================================================
// BATTERIEZUSTAND
// ============================================================

void updateBatteryState()
{
    if (
        batteryCurrent >
        DISCHARGING_CURRENT_A
    )
    {
        batteryState =
            STATE_DISCHARGING;
    }
    else if (
        batteryCurrent <
        -CHARGING_CURRENT_A
    )
    {
        batteryState =
            STATE_CHARGING;
    }
    else
    {
        batteryState =
            STATE_IDLE;
    }
}

// ============================================================
// ZEITBERECHNUNG
// ============================================================

void updateTimeEstimates()
{
    float remainingAh =
        BATTERY_CAPACITY_AH -
        ampHoursUsed;

    remainingAh =
        clampFloat(
            remainingAh,
            0.0f,
            BATTERY_CAPACITY_AH
        );

    // --------------------------------------------------------
    // Zeit bis leer
    // --------------------------------------------------------

    if (
        batteryCurrent >
            ESTIMATION_MIN_CURRENT_A &&
        remainingAh > 0.0f
    )
    {
        timeToEmptyHours =
            remainingAh /
            batteryCurrent;
    }
    else
    {
        timeToEmptyHours =
            -1.0f;
    }

    // --------------------------------------------------------
    // Zeit bis voll
    // --------------------------------------------------------

    if (
        batteryCurrent <
            -ESTIMATION_MIN_CURRENT_A &&
        ampHoursUsed > 0.0f
    )
    {
        timeToFullHours =
            ampHoursUsed /
            fabs(batteryCurrent);
    }
    else
    {
        timeToFullHours =
            -1.0f;
    }
}

// ============================================================
// FULL DETECTION
// ============================================================

void checkFullDetection()
{
    // --------------------------------------------------------
    // Bereits voll erkannt
    // --------------------------------------------------------

    if (fullDetected)
    {
        if (
            batteryVoltage <
            FULL_RELEASE_VOLTAGE
        )
        {
            fullDetected = false;
        }

        return;
    }

    // --------------------------------------------------------
    // Voraussetzungen
    // --------------------------------------------------------

    bool voltageOK =
        batteryVoltage >=
        FULL_VOLTAGE;

    bool charging =
        batteryCurrent <
        -CHARGING_CURRENT_A;

    bool currentLow =
        fabs(batteryCurrent) <=
        FULL_CHARGE_CURRENT;

    bool fullCondition =
        voltageOK &&
        charging &&
        currentLow;

    // --------------------------------------------------------
    // Bedingung erstmals erkannt
    // --------------------------------------------------------

    if (fullCondition)
    {
        if (fullConditionStart == 0)
        {
            fullConditionStart =
                millis();
        }

        unsigned long elapsed =
            millis() -
            fullConditionStart;

        if (
            elapsed >=
            FULL_CONFIRM_TIME_MS
        )
        {
            setBatteryFull();
        }
    }
    else
    {
        fullConditionStart = 0;
    }
}

// ============================================================
// COULOMB COUNTING
// ============================================================

void updateCoulombCounting()
{
    unsigned long now =
        millis();

    if (lastCoulombUpdate == 0)
    {
        lastCoulombUpdate =
            now;

        return;
    }

    unsigned long deltaMs =
        now -
        lastCoulombUpdate;

    lastCoulombUpdate =
        now;

    // Sicherheitsgrenze
    if (deltaMs > 10000UL)
        deltaMs = 10000UL;

    float deltaSeconds =
        deltaMs /
        1000.0f;

    // + Strom = Entladung
    // - Strom = Ladung

    float deltaAh =
        batteryCurrent *
        (deltaSeconds /
         3600.0f);

    ampHoursUsed +=
        deltaAh;

    ampHoursUsed =
        clampFloat(
            ampHoursUsed,
            0.0f,
            BATTERY_CAPACITY_AH
        );

    socCoulomb =
        calculateCoulombSOC();

    soc =
        socCoulomb;
}

// ============================================================
// BATTERIE AUF 100 % SETZEN
// ============================================================

void setBatteryFull()
{
    ampHoursUsed = 0.0f;

    socCoulomb = 100.0f;
    soc        = 100.0f;

    fullDetected = true;

    fullConditionStart = 0;

    saveBatteryState();
}

// ============================================================
// PERSISTENZ
// ============================================================

void saveBatteryState()
{
    preferences.begin(
        "battery",
        false
    );

    preferences.putFloat(
        "ah_used",
        ampHoursUsed
    );

    preferences.putBool(
        "full",
        fullDetected
    );

    preferences.end();
}

// ------------------------------------------------------------

void loadBatteryState()
{
    preferences.begin(
        "battery",
        true
    );

    ampHoursUsed =
        preferences.getFloat(
            "ah_used",
            0.0f
        );

    fullDetected =
        preferences.getBool(
            "full",
            false
        );

    preferences.end();

    ampHoursUsed =
        clampFloat(
            ampHoursUsed,
            0.0f,
            BATTERY_CAPACITY_AH
        );

    socCoulomb =
        calculateCoulombSOC();

    soc =
        socCoulomb;
}

// ============================================================
// INA226
// ============================================================

bool setupINA226()
{
    Serial.println();
    Serial.println(
        "Initialisiere INA226..."
    );

    if (!INA0.begin())
    {
        Serial.println(
            "FEHLER: INA226 nicht gefunden."
        );

        return false;
    }

    // 64 Messungen mitteln
    INA0.setAverage(
        INA226_64_SAMPLES
    );

    // 1.1 ms Conversion Time
    INA0.setBusVoltageConversionTime(
        INA226_1100_us
    );

    INA0.setShuntVoltageConversionTime(
        INA226_1100_us
    );

    // Continuous shunt + bus
    INA0.setMode(7);

    Serial.println(
        "INA226 erfolgreich initialisiert."
    );

    Serial.print(
        "Shunt-Widerstand: "
    );

    Serial.print(
        SHUNT_RESISTANCE_OHM *
        1000000.0f,
        2
    );

    Serial.println(
        " µOhm"
    );

    return true;
}

// ============================================================
// INA226 MESSUNG
// ============================================================

bool readINA226()
{
    if (!ina226Available)
        return false;

    float voltage =
        INA0.getBusVoltage();

    float shuntVoltage =
        INA0.getShuntVoltage();

    if (
        !isValidFloat(voltage) ||
        !isValidFloat(shuntVoltage)
    )
    {
        Serial.println(
            "FEHLER: Ungültige INA226-Messwerte."
        );

        return false;
    }

    float current =
        (shuntVoltage /
         SHUNT_RESISTANCE_OHM) *
        CURRENT_DIRECTION;

    batteryVoltage =
        voltage;

    batteryCurrent =
        current;

    batteryShuntVoltage =
        shuntVoltage;

    batteryPower =
        batteryVoltage *
        batteryCurrent;

    socVoltage =
        calculateVoltageSOC(
            batteryVoltage
        );

    updateCurrentHistory(
        batteryCurrent
    );

    calculateAverageCurrents();

    updateBatteryState();

    Serial.print("INA226 | Spannung: ");
    Serial.print(batteryVoltage, 3);
    Serial.print(" V | Strom: ");
    Serial.print(batteryCurrent, 3);
    Serial.print(" A | Leistung: ");
    Serial.print(batteryPower, 3);
    Serial.print(" W | Zustand: ");
    Serial.println(getBatteryStateString());

    updateCoulombCounting();

    checkFullDetection();

    updateTimeEstimates();

    return true;
}

// ============================================================
// BATTERIEZUSTAND ALS TEXT
// ============================================================

String getBatteryStateString()
{
    switch (batteryState)
    {
        case STATE_CHARGING:
            return "charging";

        case STATE_DISCHARGING:
            return "discharging";

        default:
            return "idle";
    }
}

// ============================================================
// WIFI SETUP
// ============================================================

void setupWiFi()
{
    preferences.begin(
        "wifi",
        true
    );

    wifiSSID =
        preferences.getString(
            "ssid",
            ""
        );

    wifiPassword =
        preferences.getString(
            "password",
            ""
        );

    preferences.end();

    // --------------------------------------------------------
    // Keine Zugangsdaten -> Access Point + Station
    // --------------------------------------------------------

    if (wifiSSID.length() == 0)
    {
        Serial.println(
            "Keine WLAN-Zugangsdaten gespeichert."
        );

        // AP + STA:
        // Der ESP stellt seinen eigenen AP bereit
        // und kann gleichzeitig WLANs scannen.
        WiFi.mode(
            WIFI_AP_STA
        );

        WiFi.softAP(
            AP_SSID,
            AP_PASSWORD
        );

        wifiAPMode = true;

        Serial.println(
            "Access Point gestartet."
        );

        Serial.print(
            "SSID: "
        );

        Serial.println(
            AP_SSID
        );

        Serial.print(
            "IP: "
        );

        Serial.println(
            WiFi.softAPIP()
        );

        return;
    }

    // --------------------------------------------------------
    // Station Mode
    // --------------------------------------------------------

    Serial.println();

    Serial.print(
        "Verbinde mit WLAN: "
    );

    Serial.println(
        wifiSSID
    );

    WiFi.mode(
        WIFI_STA
    );

    WiFi.setHostname(
        HOSTNAME
    );

    WiFi.begin(
        wifiSSID.c_str(),
        wifiPassword.c_str()
    );

    unsigned long startTime =
        millis();

    while (
        WiFi.status() !=
            WL_CONNECTED &&
        millis() - startTime <
            8000UL
    )
    {
        delay(250);

        Serial.print(
            "."
        );
    }

    Serial.println();

    if (
        WiFi.status() ==
        WL_CONNECTED
    )
    {
        wifiAPMode = false;

        Serial.println(
            "WLAN erfolgreich verbunden."
        );

        Serial.print(
            "IP: "
        );

        Serial.println(
            WiFi.localIP()
        );
    }
    else
    {
        Serial.println(
            "WLAN-Verbindung fehlgeschlagen."
        );

        WiFi.disconnect(
            true
        );

        delay(100);

        // Auch im Fallback AP + STA,
        // damit WLAN-Scans funktionieren.
        WiFi.mode(
            WIFI_AP_STA
        );

        WiFi.softAP(
            AP_SSID,
            AP_PASSWORD
        );

        wifiAPMode = true;

        Serial.println(
            "Fallback Access Point gestartet."
        );

        Serial.print(
            "SSID: "
        );

        Serial.println(
            AP_SSID
        );

        Serial.print(
            "IP: "
        );

        Serial.println(
            WiFi.softAPIP()
        );
    }
}

// ============================================================
// WLAN SCAN VERARBEITEN
// ============================================================

void processWiFiScan()
{
    if (!wifiScanRequested)
        return;

    wifiScanRequested = false;

    Serial.println();
    Serial.println("========================================");
    Serial.println(" STARTE SYNCHRONEN WLAN-SCAN");
    Serial.println("========================================");

    WiFi.scanDelete();

    int result = WiFi.scanNetworks(
        false,   // synchron
        true     // versteckte Netzwerke
    );

    Serial.print("scanNetworks() Rückgabe: ");
    Serial.println(result);

    if (result < 0)
    {
        Serial.println("WLAN-SCAN FEHLGESCHLAGEN.");

        wifiScanRunning = false;
        wifiScanFinished = true;
        wifiScanResultCount = -1;

        wifiScanLastFinish = millis();

        return;
    }

    Serial.print("WLAN-Scan abgeschlossen: ");
    Serial.print(result);
    Serial.println(" Netzwerke gefunden.");

    for (int i = 0; i < result; i++)
    {
        Serial.print(i);
        Serial.print(": SSID=");
        Serial.print(WiFi.SSID(i));

        Serial.print(" RSSI=");
        Serial.print(WiFi.RSSI(i));

        Serial.print(" Kanal=");
        Serial.print(WiFi.channel(i));

        Serial.print(" Verschlüsselung=");
        Serial.println(WiFi.encryptionType(i));
    }

    wifiScanResultCount = result;

    wifiScanRunning = false;
    wifiScanFinished = true;

    wifiScanLastFinish = millis();
}

// ============================================================
// RESTART ANFORDERN
// ============================================================

void requestRestart(
    unsigned long delayMs
)
{
    restartRequested =
        true;

    restartAt =
        millis() +
        delayMs;
}

// ============================================================
// JSON STATE
// ============================================================

void createStateJSON(
    JsonDocument& doc
)
{
    doc.clear();

    doc["name"] =
        BATTERY_NAME;

    doc["voltage"] =
        batteryVoltage;

    doc["current"] =
        batteryCurrent;

    doc["power"] =
        batteryPower;

    doc["shuntVoltage"] =
        batteryShuntVoltage;

    doc["capacityAh"] =
        BATTERY_CAPACITY_AH;

    doc["usedAh"] =
        ampHoursUsed;

    doc["remainingAh"] =
        BATTERY_CAPACITY_AH -
        ampHoursUsed;

    doc["soc"] =
        soc;

    doc["socCoulomb"] =
        socCoulomb;

    doc["socVoltage"] =
        socVoltage;

    doc["state"] =
        getBatteryStateString();

    doc["full"] =
        fullDetected;

    doc["averageDischargeCurrent"] =
        averageDischargeCurrent;

    doc["averageChargeCurrent"] =
        averageChargeCurrent;

    doc["timeToEmptyHours"] =
        timeToEmptyHours;

    doc["timeToFullHours"] =
        timeToFullHours;

    doc["lowVoltage"] =
        batteryVoltage <=
        LOW_VOLTAGE_WARNING;

    doc["criticalVoltage"] =
        batteryVoltage <=
        CRITICAL_VOLTAGE;

    doc["shuntResistanceOhm"] =
        SHUNT_RESISTANCE_OHM;

    // Status des INA226
    doc["ina226Available"] =
        ina226Available;

    // --------------------------------------------------------
    // WLAN
    // --------------------------------------------------------

    JsonObject wifi =
        doc["wifi"]
            .to<JsonObject>();

    wifi["apMode"] =
        wifiAPMode;

    if (wifiAPMode)
    {
        wifi["ssid"] =
            AP_SSID;

        wifi["ip"] =
            WiFi.softAPIP()
                .toString();

        wifi["rssi"] =
            0;
    }
    else
    {
        wifi["ssid"] =
            WiFi.SSID();

        wifi["ip"] =
            WiFi.localIP()
                .toString();

        wifi["rssi"] =
            WiFi.RSSI();
    }
}

// ============================================================
// WEB SERVER
// ============================================================

void setupWebServer()
{
    // --------------------------------------------------------
    // Hauptseite
    // --------------------------------------------------------

    server.on(
    "/",
    HTTP_GET,
    [](AsyncWebServerRequest* request)
    {
        AsyncWebServerResponse* response =
            request->beginResponse(
                SPIFFS,
                "/index.html",
                "text/html"
            );

        response->addHeader(
            "Cache-Control",
            "no-store, no-cache, must-revalidate"
        );

        response->addHeader(
            "Pragma",
            "no-cache"
        );

        response->addHeader(
            "Expires",
            "0"
        );

        request->send(response);
    }
);

// --------------------------------------------------------
// TEST
// --------------------------------------------------------

server.on(
    "/api/test",
    HTTP_GET,
    [](AsyncWebServerRequest* request)
    {
        request->send(
            200,
            "text/plain",
            "WohnwagenMonitor TEST 2026-09-15"
        );
    }
);

    // --------------------------------------------------------
    // API STATE
    // --------------------------------------------------------

    server.on(
        "/api/state",
        HTTP_GET,
        [](AsyncWebServerRequest* request)
        {
            JsonDocument doc;

            createStateJSON(
                doc
            );

            String response;

            serializeJson(
                doc,
                response
            );

            AsyncWebServerResponse*
                httpResponse =
                    request->beginResponse(
                        200,
                        "application/json",
                        response
                    );

            httpResponse->addHeader(
                "Cache-Control",
                "no-cache"
            );

            request->send(
                httpResponse
            );
        }
    );

    // --------------------------------------------------------
    // WIFI SCAN STATUS
    // --------------------------------------------------------

    server.on(
    "/api/wifi/scan/status",
    HTTP_GET,
    [](AsyncWebServerRequest* request)
    {   
        Serial.println();
        Serial.println(">>> STATUS HANDLER");
        Serial.print("    request->url() = ");
        Serial.println(request->url());
        Serial.println(">>> /api/wifi/scan/status aufgerufen");
        JsonDocument doc;

        doc["TEST_VERSION"] = "STATUS-2026-09-15";
        doc["running"] = wifiScanRunning;
        doc["finished"] = wifiScanFinished;
        doc["count"] = wifiScanResultCount;

        // ------------------------------------------------
        // Nur abgeschlossene Scans liefern Ergebnisse
        // ------------------------------------------------

        if (
            wifiScanFinished &&
            !wifiScanRunning &&
            wifiScanResultCount >= 0
        )
        {
            JsonArray networks =
                doc["networks"]
                    .to<JsonArray>();

            for (
                int i = 0;
                i < wifiScanResultCount;
                i++
            )
            {
                JsonObject network =
                    networks.add<JsonObject>();

                network["ssid"] =
                    WiFi.SSID(i);

                network["rssi"] =
                    WiFi.RSSI(i);

                network["channel"] =
                    WiFi.channel(i);

                network["encryption"] =
                    (
                        WiFi.encryptionType(i) !=
                        WIFI_AUTH_OPEN
                    );
            }
        }

        String response;

        serializeJson(
            doc,
            response
        );

        request->send(
            200,
            "application/json",
            response
        );
    }
);


    // --------------------------------------------------------
    // WIFI SCAN STARTEN
    // --------------------------------------------------------

    server.on(
    "/api/wifi/scan",
    HTTP_GET,
    [](AsyncWebServerRequest* request)
    {
        Serial.println();
        Serial.println(">>> SCAN HANDLER");
        Serial.print("    request->url() = ");
        Serial.println(request->url());

        static unsigned long scanRequestCounter = 0;
        scanRequestCounter++;

        Serial.println(">>> /api/wifi/scan aufgerufen #");
        Serial.println(scanRequestCounter);

        // ------------------------------------------------
        // Bereits laufender Scan
        // ------------------------------------------------

        if (wifiScanRunning)
        {
            Serial.println("    -> Scan läuft bereits.");

            request->send(
                200,
                "application/json",
                "{\"started\":false,\"running\":true}"
            );

            return;
        }

        // ------------------------------------------------
        // Cooldown prüfen
        // ------------------------------------------------

        unsigned long now = millis();

        if (
            wifiScanLastFinish != 0 &&
            now - wifiScanLastFinish < WIFI_SCAN_COOLDOWN_MS
        )
        {
            Serial.println("    -> Scan wegen Cooldown abgelehnt.");

            request->send(
                200,
                "application/json",
                "{\"started\":false,\"running\":false,\"cooldown\":true}"
            );

            return;
        }

        // ------------------------------------------------
        // Scan starten
        // ------------------------------------------------

        Serial.println("    -> Scan wird angefordert.");

        wifiScanRunning = true;

        wifiScanRequested = true;

        wifiScanFinished = false;

        wifiScanResultCount = -1;

        request->send(
            200,
            "application/json",
            "{\"started\":true,\"running\":true}"
        );
    }
);

    
    // --------------------------------------------------------
    // WIFI SPEICHERN
    // --------------------------------------------------------

    server.on(
        "/api/wifi/save",
        HTTP_POST,
        [](AsyncWebServerRequest* request)
        {
            if (
                !request->hasParam(
                    "ssid",
                    true
                )
            )
            {
                request->send(
                    400,
                    "application/json",
                    "{\"error\":\"SSID fehlt\"}"
                );

                return;
            }

            String newSSID =
                request->getParam(
                    "ssid",
                    true
                )->value();

            String newPassword;

            if (
                request->hasParam(
                    "password",
                    true
                )
            )
            {
                newPassword =
                    request->getParam(
                        "password",
                        true
                    )->value();
            }

            if (
                newSSID.length() == 0
            )
            {
                request->send(
                    400,
                    "application/json",
                    "{\"error\":\"SSID darf nicht leer sein\"}"
                );

                return;
            }

            preferences.begin(
                "wifi",
                false
            );

            preferences.putString(
                "ssid",
                newSSID
            );

            preferences.putString(
                "password",
                newPassword
            );

            preferences.end();

            request->send(
                200,
                "application/json",
                "{\"success\":true,\"restart\":true}"
            );

            Serial.println(
                "Neue WLAN-Daten gespeichert."
            );

            requestRestart(
                1000
            );
        }
    );

    // --------------------------------------------------------
    // WIFI ZURÜCKSETZEN
    // --------------------------------------------------------

    server.on(
        "/api/wifi/reset",
        HTTP_POST,
        [](AsyncWebServerRequest* request)
        {
            preferences.begin(
                "wifi",
                false
            );

            preferences.clear();

            preferences.end();

            request->send(
                200,
                "application/json",
                "{\"success\":true,\"restart\":true}"
            );

            Serial.println(
                "WLAN-Zugangsdaten gelöscht."
            );

            requestRestart(
                1000
            );
        }
    );

    // --------------------------------------------------------
    // BATTERIE AUF 100 %
    // --------------------------------------------------------

    server.on(
        "/api/full",
        HTTP_POST,
        [](AsyncWebServerRequest* request)
        {
            setBatteryFull();

            request->send(
                200,
                "application/json",
                "{\"success\":true}"
            );
        }
    );

    // --------------------------------------------------------
    // STATISCHE DATEIEN
    // --------------------------------------------------------

    server.serveStatic(
        "/",
        SPIFFS,
        "/"
    );

    // --------------------------------------------------------
    // SERVER START
    // --------------------------------------------------------

    server.begin();

    Serial.println(
        "Webserver gestartet."
    );
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );

    delay(500);

    Serial.println();

    Serial.println(
        "========================================"
    );

    Serial.println(
        " WohnwagenMonitor"
    );

    Serial.println(
        " ESP32 Batterie Shunt Monitor"
    );

    Serial.println(
        "========================================"
    );

    // --------------------------------------------------------
    // I2C
    // --------------------------------------------------------

    Wire.begin(
        I2C_SDA,
        I2C_SCL
    );

    // --------------------------------------------------------
    // SPIFFS
    // --------------------------------------------------------

    if (!SPIFFS.begin(true))
    {
        Serial.println(
            "FEHLER: SPIFFS konnte nicht gestartet werden."
        );
    }
    else
    {
        Serial.println(
            "SPIFFS erfolgreich gestartet."
        );

        if (
            !SPIFFS.exists(
                "/index.html"
            )
        )
        {
            Serial.println(
                "WARNUNG: /index.html nicht gefunden!"
            );
        }
    }

    // --------------------------------------------------------
    // Batterie-Zustand laden
    // --------------------------------------------------------

    loadBatteryState();

    // --------------------------------------------------------
    // INA226
    // --------------------------------------------------------

    ina226Available =
        setupINA226();

    // --------------------------------------------------------
    // WLAN
    // --------------------------------------------------------

    setupWiFi();

    // --------------------------------------------------------
    // Webserver
    // --------------------------------------------------------

    setupWebServer();

    // --------------------------------------------------------
    // Timer
    // --------------------------------------------------------

    lastMeasurement =
        millis();

    lastSave =
        millis();

    lastCoulombUpdate =
        millis();

    // --------------------------------------------------------
    // Status
    // --------------------------------------------------------

    Serial.println();

    Serial.println(
        "WohnwagenMonitor bereit."
    );

    if (wifiAPMode)
    {
        Serial.print(
            "Weboberfläche: http://"
        );

        Serial.print(
            WiFi.softAPIP()
        );

        Serial.println(
            "/"
        );
    }
    else
    {
        Serial.print(
            "Weboberfläche: http://"
        );

        Serial.print(
            WiFi.localIP()
        );

        Serial.println(
            "/"
        );
    }
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    unsigned long now =
        millis();

    // --------------------------------------------------------
    // WLAN Scan
    // --------------------------------------------------------

    processWiFiScan();

    // --------------------------------------------------------
    // Messung
    // --------------------------------------------------------

    if (
        now - lastMeasurement >=
        MEASUREMENT_INTERVAL_MS
    )
    {
        lastMeasurement =
            now;

        if (!readINA226())
        {
            // Sensor nicht verfügbar:
            // keine Coulombzählung mit
            // ungültigen Daten.
        }
    }

    // --------------------------------------------------------
    // Batterie-Zustand speichern
    // --------------------------------------------------------

    if (
        now - lastSave >=
        SAVE_INTERVAL_MS
    )
    {
        lastSave =
            now;

        saveBatteryState();
    }

    // --------------------------------------------------------
    // Angeforderten Neustart ausführen
    // --------------------------------------------------------

    if (
        restartRequested &&
        (long)(
            now - restartAt
        ) >= 0
    )
    {
        Serial.println(
            "ESP32 wird neu gestartet..."
        );

        delay(100);

        ESP.restart();
    }

    delay(5);
}