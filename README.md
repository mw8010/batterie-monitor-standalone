# Batterie Monitor Standalone

Standalone-Batteriemonitor auf Basis eines ESP32 und eines INA226-Shunt-
Messwandlers. Der Monitor erfasst Spannung, Strom, Leistung und Ladezustand
und stellt die Werte über eine lokale Weboberfläche bereit.

## Funktionen

- Messung von Batteriespannung, Strom und Leistung mit dem INA226
- Ladezustandsberechnung per Coulomb Counting und Spannungskurve
- Anzeige von Ladezustand, Batteriestatus und Restkapazität
- Zeitprognose bis leer beziehungsweise bis voll
- Spannungswarnung und kritische Spannungserkennung
- Speicherung des Ladezustands im ESP32-NVS
- WLAN-Betrieb als Station oder Access Point
- WLAN-Konfiguration und Netzwerkscan über die Weboberfläche
- Responsive Weboberfläche mit den Bereichen Übersicht, Batterie, WLAN und Diagnose
- Automatische Aktualisierung der Messwerte im Sekundentakt

## Hardware

- ESP32 MiniKit, PlatformIO-Board `mhetesp32minikit`
- INA226 Strom-/Spannungsmessmodul
- Geeigneter Batterieshunt, im Projekt standardmäßig für 500 A / 75 mV konfiguriert
- 12-V-LiFePO4-Batterie, passend zur hinterlegten Spannungskurve

### I²C-Verbindung

| INA226 | ESP32 |
| --- | --- |
| SDA | GPIO 22 |
| SCL | GPIO 21 |
| GND | GND |
| VCC | Entsprechend dem verwendeten Modul |

Die INA226-Standardadresse ist `0x40`.

> **Sicherheit:** Die Batterie- und Shunt-Verkabelung kann hohe Ströme führen.
> Sicherungen, Leitungsquerschnitte, Polarität und Messbereich müssen zur
> konkreten Anlage passen. Arbeiten an Batteriesystemen nur fachgerecht und
> spannungsfrei durchführen.

## Voraussetzungen

- VS Code mit der PlatformIO-Erweiterung oder PlatformIO CLI
- USB-Datenkabel für den ESP32
- Lokales WLAN für den normalen Station-Betrieb

## Installation

1. Repository klonen und den Projektordner in VS Code öffnen.
2. ESP32 über USB anschließen.
3. Firmware und Dateisystem übertragen:

   ```bash
   pio run --target upload
   pio run --target uploadfs
   ```

4. Optional den seriellen Monitor öffnen:

   ```bash
   pio device monitor --baud 115200
   ```

Nach dem Start verbindet sich der ESP32 mit dem gespeicherten WLAN. Falls
keine Verbindung möglich ist, startet er den Access-Point-Modus. Die genaue
IP-Adresse wird im seriellen Monitor ausgegeben.

## Erstinbetriebnahme

1. Mit dem vom ESP32 bereitgestellten WLAN verbinden.
2. Die im seriellen Monitor angezeigte IP-Adresse im Browser öffnen.
3. Im Tab **WLAN** das lokale WLAN und Passwort eintragen.
4. **WLAN speichern** auswählen. Der ESP32 startet die WLAN-Verbindung neu.
5. Anschließend die neue IP-Adresse im Browser öffnen.

Der standardmäßige Access-Point-Name und das Access-Point-Passwort stehen in
`src/main.cpp`. Das Passwort sollte vor einem produktiven Einsatz geändert
werden.

## Konfiguration

Die wichtigsten Batterieparameter befinden sich am Anfang von
`src/main.cpp`:

- `BATTERY_CAPACITY_AH`: Nennkapazität der Batterie
- `MAX_CURRENT_A`: maximaler Shunt-Strom
- `SHUNT_MV`: Nennspannung des Shunts
- `CURRENT_DIRECTION`: Richtung von Laden und Entladen
- `LOW_VOLTAGE_WARNING`: Spannung für die Warnung
- `CRITICAL_VOLTAGE`: kritische Spannung
- `LIFEPO4_12V_CURVE`: Spannungskurve für die SoC-Diagnose

Nach Änderungen an Firmware oder Weboberfläche müssen Firmware und/oder
Dateisystem erneut übertragen werden.

## Projektstruktur

```text
.
├── data/index.html       Weboberfläche für SPIFFS
├── include/              Öffentliche Header
├── lib/                  Projektbibliotheken
├── src/main.cpp          ESP32-Firmware
├── test/                 Testbereich
└── platformio.ini        PlatformIO-Konfiguration
```

## Verwendete Bibliotheken

- ArduinoJson
- INA226 von Rob Tillaart
- ESPAsyncWebServer
- AsyncTCP
- Arduino WiFi, SPIFFS und Preferences

## API

Die Weboberfläche verwendet unter anderem folgende lokale Endpunkte:

- `GET /api/state`: aktueller Batterie- und WLAN-Status
- `POST /api/full`: Ladezustand auf 100 Prozent setzen
- `GET /api/wifi/scan`: WLAN-Scan starten
- `GET /api/wifi/scan/status`: Status und Ergebnis des WLAN-Scans
- `POST /api/wifi/save`: WLAN-Zugangsdaten speichern
- `POST /api/wifi/reset`: gespeicherte WLAN-Daten löschen

## Lizenzen

- **Software:** GNU General Public License v3.0, siehe [LICENSE](LICENSE).
- **Hardware-Design:** CERN Open Hardware Licence v2 - Strongly Reciprocal,
  siehe [LICENSE-HARDWARE](LICENSE-HARDWARE).

Die vollständigen Lizenzbedingungen sind in den jeweiligen Lizenzdateien
beziehungsweise auf den dort verlinkten offiziellen Seiten enthalten.