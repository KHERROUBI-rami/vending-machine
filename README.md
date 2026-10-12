# RFID Prepaid Service Controller

## Overview
An Arduino-based prepaid card system for vending machines or automated paid services. It uses an MFRC522 RFID reader to scan MIFARE cards. User balances are stored directly in the Arduino's EEPROM, allowing data persistence across power cycles and preventing balance loss during re-swipes. 

A unique feature of this repository is the included **C++ PC simulation environment**, which allows developers to compile and test the core logic, EEPROM behavior, and hardware interactions on a standard computer without needing the physical Arduino board.

## Features
- **Non-blocking Architecture:** Strictly uses `millis()` for timers; no `delay()` is used, ensuring zero missed inputs.
- **PC Simulation Framework:** Includes a custom mock API (`Arduino.h`, `EEPROM.h`, etc.) to run automated unit tests on PC.
- **EEPROM Persistence:** Card UIDs and balances are saved in slots on the EEPROM.
- **Multi-tier Pricing:** Supports different service outputs (e.g., 100 DA or 200 DA) based on button inputs, with a combination logic if both are pressed.
- **Session Management:** Interactive I2C LCD prompts with automatic session timeouts.

## Hardware Requirements
- Arduino Uno / Nano
- MFRC522 RFID Reader
- 16x2 I2C LCD Display
- Push Buttons (for service selection)
- Relay module (to drive the actual vending/service mechanism)

## Pin Configuration
| Component | Arduino Pin |
| :--- | :--- |
| RFID SDA (SS) | 10 |
| RFID RST | 9 |
| Button 1 (100 DA) | 7 |
| Button 2 (200 DA) | 8 |
| Service Output | 3 |
| Signal / Buzzer | 4 |

*Note: Button inputs assume external 10k pull-down resistors (Active HIGH). This can be changed to `INPUT_PULLUP` by setting `INPUT_ACTIVE_LEVEL LOW` in the code.*

## How to Run the PC Simulation
You can verify the logic and run the test suite directly on your PC.
1. Navigate to the simulation folder in your terminal.
2. Compile the test using a standard C++11 compiler (like GCC/MinGW):
   ```bash
   g++ sim_test.cpp -o sim_test -std=c++11
