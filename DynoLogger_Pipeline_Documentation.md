# Dyno Data Logger Pipeline Documentation

**Project:** Agriculture EV Truck Dyno Data Logger  
**Author:** Aiman Harith Bin Abdul Hamid  
**Organisation:** MiND Mechatronic Intelligent Design Sdn Bhd, Malaysian Palm Oil Board Sdn Bhd (MPOB)  
**Version:** 1.0 — July 2026  

---

## 1. Project Overview

The **Agriculture EV Truck Dyno Data Logger** is an embedded IoT system that measures and records electrical parameters such as **Voltage, Current, Power, and Energy**  from a DC load under test on a Dynamometer & Normal use. The **PZEM-017 DC energy meter** communicates with the ESP32 built-in on the KC868-A2 V2 board. Measurement data is logged to internal flash (SPIFFS), visualised on a live web dashboard, and downloadable as a CSV file. This Project does support dual way communication setup for easy device setup stationary or no the go.

### Key Features
* **Real-time measurement:** Voltage, Current, Power, and Energy via PZEM-017
* **RS485 Modbus RTU:** Communication at 9600 baud, 8N2
* **Local CSV logging:** Persistent storage to ESP32 SPIFFS flash — no SD card required
* **Live Web Dashboard:** Trend charts auto-refreshing via `/status` JSON polling
* **Dual WiFi Modes:** Station mode (joins router) with automatic Access Point fallback
* **mDNS Hostname:** Always reachable at `http://dynologger.local`
* **Editable Configuration Panel:** WiFi credentials, Modbus ID, log interval, operator, test ID, voltage alarm thresholds
* **Voltage Alarms:** High/Low voltage thresholds with pulsing status indicators
* **Relay Controls:** 
  * **Relay 1:** Physical power/system status indicator
  * **Relay 2:** Logging active indicator; blinks on Modbus communication error
* **Notes & Memo:** Saved directly to flash, accessible via the UI
* **Dynamic CSV Naming:** Saved as `dyno_DDMMYYYY_HHMM_operator_testid.csv`

---

## 2. Hardware Architecture & Wiring

### Bill of Materials

| Component | Model / Spec | Role |
| :--- | :--- | :--- |
| **Main Controller** | Kincony KC868-A2 V2 (ESP32 MCU) | WiFi, web server, Modbus master, relay control |
| **Energy Meter** | PZEM-017 DC | Measures V, I, P, Energy via RS485 Modbus RTU |
| **Current Shunt** | 200A-FL (included) | Current sensing on negative rail |
| **Power Supply** | AC/DC adapter, 12V | Powers the KC868-A2 board |
| **Logic Supply** | 5V DC supply (Buck converter) | Powers PZEM-017 logic circuit |
| **Battery / DUT** | 80V DC battery under test | Measurement source |
| **Load** | Resistive / motor load | Connected across battery via shunt |

### GPIO Pin Assignment

| Signal | GPIO | Direction | Notes |
| :--- | :--- | :--- | :--- |
| **RS485 RXD** | GPIO 35 | Input only | Input-only pin — no pull-up required |
| **RS485 TXD** | GPIO 32 | Output | Drives RS485 transceiver TX |
| **Relay 1** | GPIO 15 | Output | ON while unit is powered — Power LED |
| **Relay 2** | GPIO 2 | Output | ON during logging; blinks on Modbus error |

### RS485 Wiring Diagram

| KC868-A2 Terminal | PZEM-017 Terminal | Typical Wire Colour |
| :--- | :--- | :--- |
| **RS485 A** | A | Purple |
| **RS485 B** | B | Yellow |
| **GND** | GND | Black |
| **—** | 5V | 5V supply positive |

> **Note:** RS485 A/B labelling is not universal across manufacturers. If Modbus returns error `0xE2` (Response Timeout), swap the A and B wires first.

### Relay Output Behaviour

| State | Relay 1 — Power | Relay 2 — Logging |
| :--- | :--- | :--- |
| **Unit powered on** | ON | OFF |
| **WiFi connected, server running** | ON | OFF |
| **Logging session started** | ON | ON (solid) |
| **Logging session stopped** | ON | OFF |
| **Modbus error (any state)** | ON | Blinks every 2 s |
| **Error cleared, not logging** | ON | OFF |
| **Error cleared, logging active** | ON | ON (solid) |
| **Unit powered off** | OFF | OFF |

> **Logic:** KC868-A2 V2 relay outputs are active-LOW internally; the firmware writes `HIGH` to energise (ON) and `LOW` to de-energise (OFF).

---

## 3. Software Architecture

### Technology Stack

| Layer | Technology | Purpose |
| :--- | :--- | :--- |
| **Firmware** | Arduino-ESP32 (C++) | Main application on the ESP32 |
| **Modbus** | ModbusMaster (Doc Walker) | Modbus RTU master over HardwareSerial |
| **Storage** | SPIFFS (ESP32 core) | Persistent flash filesystem |
| **Network** | WiFi + WebServer (core) | HTTP server on port 80 |
| **DNS** | ESPmDNS (core) | Hostname: `dynologger.local` |
| **Time** | NTP via `configTime()` | Real timestamps — STA mode only |
| **Frontend** | HTML / CSS / JS | Dashboard, charts, config form |
| **Charts** | HTML5 Canvas, vanilla JS | No CDN — works offline in AP mode |

### Flash Memory Layout (SPIFFS)

| File | Path | Content | Max Size |
| :--- | :--- | :--- | :--- |
| **Log data** | `/dyno_log.csv` | Date, Time, V, I, P, E rows | ~1.3 MB, auto-rotates at 90% |
| **Configuration** | `/config.txt` | Key=value settings | < 1 KB |
| **Memo** | `/memo.txt` | User notes / instructions | ~10 KB |

### Configuration Parameters

All parameters are saved to `/config.txt` on SPIFFS and survive reboots. Editable from the dashboard without re-flashing.

| Parameter | Config Key | Default | Notes |
| :--- | :--- | :--- | :--- |
| **Modbus Unit ID** | `modbusID` | `1` | 0x01–0xF7 |
| **Log Interval** | `logInterval` | `5000` | Milliseconds, min. 1000 |
| **Output Folder Label** | `folder` | `dyno_data` | Label only |
| **Operator Name** | `operator` | `operator` | Used in CSV filename |
| **Test ID** | `testID` | `test01` | Used in CSV filename |
| **High Voltage Threshold** | `highVolt` | `300.0` | Volts |
| **Low Voltage Threshold** | `lowVolt` | `7.0` | Volts |
| **WiFi SSID** | `wifiSSID` | *(blank)* | STA network name |
| **WiFi Password** | `wifiPass` | *(blank)* | STA network password |

---

## 4. Data Pipeline

The complete journey of a measurement, from physical sensor to downloaded CSV file:

1. **Step 1 — Physical Measurement:**  
   The battery and load are connected across the PZEM-017 input terminals. The 50A current shunt is wired in series on the negative rail; the PZEM-017 derives current from the shunt voltage drop.
2. **Step 2 — Modbus RTU Query:**  
   The ESP32 sends a Modbus RTU request (Function Code `0x04` — Read Input Registers, `0x0000–0x0007`) over RS485 to the PZEM-017 at its configured slave address (9600 baud, 8N2).
3. **Step 3 — PZEM-017 Response:**  
   The PZEM-017 returns 8 × 16-bit registers:
   * `Register 0x00` → Voltage (`raw / 100 = V`)
   * `Register 0x01` → Current (`raw / 100 = A`)
   * `Registers 0x02–0x03` → Power, 32-bit low word first (`raw / 10 = W`)
   * `Registers 0x04–0x05` → Energy, 32-bit low word first (`raw / 1000 = kWh`)
   * `Registers 0x06 / 0x07` → High / Low voltage alarm flags (`0xFFFF` = alarm)
4. **Step 4 — Voltage Alarm Evaluation:**  
   Voltage is compared against high/low thresholds. Alarm flags update immediately and reflect on the dashboard on the next poll.
5. **Step 5 — Relay 2 State Update (non-blocking):**  
   On every `loop()` iteration, `updateRelay2()` checks status. On failure, Relay 2 blinks every 2 seconds via `millis()`. On recovery, it snaps back to the active logging state.
6. **Step 6 — Trend Buffer Update:**  
   Readings push to a 120-point RAM ring buffer for live trend charts. This buffer is display-only and resets on reboot.
7. **Step 7 — CSV Append (Session-Gated):**  
   If logging is active, a row appends to `/dyno_log.csv`:
   ```csv
   2025-09-08,14:32:01,79.85,12.50,998.1,0.125
   ```
8. **Step 8 — Auto Log Rotation:**  
   When SPIFFS free space drops below 150 KB (~90% used), the oldest 20% of rows are deleted in place while keeping the header.
9. **Step 9 — Download:**  
   The `/download` endpoint streams `/dyno_log.csv` directly from SPIFFS, renaming it on the fly to `dyno_DDMMYYYY_HHMM_operator_testid.csv`.

---

## 5. Network & Web Server

### WiFi Connection Logic

| Step | Condition | Outcome |
| :---: | :--- | :--- |
| **1** | `cfg.wifiSSID` is blank | Skip STA, start AP mode directly |
| **2** | STA connects within 10 s | DHCP IP assigned, NTP synced, mDNS started |
| **3** | STA fails (wrong password / no router) | Disconnect, fall back to AP mode |
| **4** | AP mode started | Fixed IP `192.168.4.1`, mDNS started, no NTP |

### Access Information

| Mode | How to Connect | Dashboard URL | Hostname URL |
| :--- | :--- | :--- | :--- |
| **STA** | Same network as router | `http://<DHCP-IP>` | `http://dynologger.local` |
| **AP** | Connect to SSID `DynoLogger` | `http://192.168.4.1` | `http://dynologger.local` * |

> **\* Note:** `dynologger.local` works on macOS, iOS, Android, and Linux. Windows systems without Bonjour should use `192.168.4.1` directly. For consistent connection, connect board and run it via Virtual terminal (TeraTerm 5).

### Web API Endpoints

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/` | GET | Live dashboard — readings, charts, alarms, config, memo |
| `/status` | GET | JSON: readings, alarms, rows, flash usage, logging state |
| `/trend` | GET | JSON array of last 120 trend points |
| `/start` | GET | Begin logging session — Relay 2 ON |
| `/stop` | GET | Pause logging session — Relay 2 OFF |
| `/download` | GET | Stream CSV with timestamped filename |
| `/clear` | GET | Wipe CSV log, reset counters |
| `/config` | POST | Save configuration fields |
| `/reboot` | GET | Save, display message, reboot after 800 ms |
| `/memo` | GET / POST | Read or write memo text |

---

## 6. Dashboard & Controls

* **Live Readings & Alarms:** Metric cards (Voltage, Current, Power, Energy) update via `/status` polling. Displays green for normal, red for high voltage, and amber for low voltage.
* **Trend Charts:** Tabbed HTML5 Canvas panel displaying Voltage & Energy (Cyan/Pink) and Current & Power (Orange/Green) with zero-floor baselines and independent auto-scaling.
* **Controls:**
  * `▶ Start Logging` / `■ Stop Logging` — Toggles CSV writes & Relay 2 state
  * `↓ Download CSV` — Downloads timestamped CSV file
  * `🗑 Clear Log` — Wipes CSV after user confirmation
  * `Configuration` — Edit hardware/network settings (reboot required for WiFi)
  * `? Memo` — Popup notes editor backed by `/memo.txt`. Memo is erased fully when flashing new version into the board.

---

## 7. Storage Capacity Reference

SPIFFS partition: **1,472 KB total**, 150 KB reserved, **~1,321 KB usable**. Auto-rotation triggers at ~90% capacity.

| Log Interval | Rows / Hour | Time to 90% Full | Typical Use |
| :--- | :--- | :--- | :--- |
| **1 second** | 3,600 | ~8.5 hours | Maximum resolution, short runs |
| **3 seconds** | 1,200 | ~1 day 1 hour | Half-day sessions |
| **5 seconds** | 720 | ~1 day 18 hours | **Default** — most runs |
| **10 seconds** | 360 | ~3 days 13 hours | Multi-day testing |
| **30 seconds** | 120 | ~10 days 16 hours | Long-term monitoring |

---

## 8. Modbus Error Reference

| Code | Name | Likely Cause | Fix |
| :--- | :--- | :--- | :--- |
| `0xE0` | Illegal Function | Wrong function code | Use `0x04` for readings |
| `0xE1` | Illegal Data Address | Register out of range | Verify register map `0x00–0x07` |
| `0xE2` | Response Timeout | Wiring, wrong ID, wrong stop bits | Swap A/B, verify 8N2, check slave ID |
| `0xE3` | Invalid Slave ID | Address mismatch | Confirm PZEM-017 address (`0x01`) |
| `0xE4` | Invalid Function | Corrupted frame | Check RS485 shielding/cable length |
| `0xE5` | CRC Mismatch | Electrical noise | Shorten cable, add termination resistor |
| `0xE6` | Response Too Long | Bus contention | Check for multiple slaves on the bus |

> **Relay 2 Indicator:** On any Modbus error, Relay 2 blinks every 2 seconds regardless of logging state, returning to normal automatically upon successful read recovery.

---

## 9. First-Time Setup & Workflow

### Arduino IDE Setup
1. Install ESP32 board package via Boards Manager.
2. Select Board: **ESP32 Dev Module** — Partition Scheme: **Default 4MB with spiffs**.
3. Install Library: **ModbusMaster** (by Doc Walker).
4. Upload sketch to KC868-A2 over USB.

### First Boot & Network Setup
1. Open Serial Monitor at `115200` baud.
2. On initial boot (blank SSID), the device starts in AP mode automatically.
3. Connect your PC/phone to WiFi network **DynoLogger** (Password: `dyno1234`).
4. Navigate to `http://192.168.4.1` → **Configuration** → **WiFi Settings**.
5. Input your network SSID and password → click **Save & Reboot**.
6. Reconnect to the router network; access the dashboard at `http://dynologger.local`.

---

## 10. Troubleshooting Guide

| Symptom | Likely Cause | Resolution |
| :--- | :--- | :--- |
| **0xE2 timeout on every read** | A/B swapped, wrong stop bits/ID | Swap A & B; confirm 8N2; verify ID = 1 |
| **Dashboard will not load** | Wrong IP or network | Check Serial Monitor; try `dynologger.local` or `192.168.4.1` |
| **Timestamps show N/A** | AP mode (no NTP) | Connect to router and reboot to sync time |
| **CSV file is empty** | Session not started | Click `▶ Start Logging` before running the test |
| **Relay 2 blinking constantly** | Persistent Modbus error | Check wiring, PZEM 5V supply, and A/B polarity |
| **Flash bar is red** | Storage at ~90% | Download CSV, then execute Clear Log |
| **Cannot find hotspot** | Channel conflict | Change `AP_CHANNEL` in firmware from 6 to 1 or 11 |
| **.local fails on Windows** | No Bonjour service installed | Use the DHCP/AP IP address directly |

---

## 11. Reference link:

1.	https://www.kincony.com/esp32-4g-relay.html (KinCony 2 channel esp32 4G relay board – KC868-A2)
2.	https://base.sato-power.com/thtimages/Power%20Meter/PZEM-003-Manual.pdf (PZEM017 manual)
3.	https://randomnerdtutorials.com/esp32-how-to-log-data/ (ESP32 Data Logger)
4.	https://www.kincony.com/forum/showthread.php?tid=2691 (KC868-A2 ESP32 I/O pin define)
5.	https://www.youtube.com/watch?v=AnPCe3VOae0 (Adding PZEM-017 current and voltage monitor to DIYBMS)
6.	https://developerinsider.co/solar-battery-dc-energy-meter-for-home-assistant/ (Solar/Battery DC Energy Meter for Home Assistant)



