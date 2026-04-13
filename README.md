# SecuriTea

Spilling the tea about security.

SecuriTea is an ESP32-based handheld IR utility that combines:
- Universal IR brute-force profiles
- SD card IR file browsing and replay
- Learn mode to capture IR signals from real remotes and save them to SD
- OLED joystick-driven interface

## Hardware

Target device used in this project:
- Heltec ESP32 LoRa V3 3.2

Main peripherals used:
- IR receiver module (for Learn mode)
- IR LED transmitter (for blasting/replay)
- Joystick (X, Y, button)
- SD card module (SPI)
- Built-in OLED display

## Project Structure

- `main.ino`: main firmware with UI, SD parser, IR send/receive, and task logic.

## Core Features

1. Universal Remote
- Choose a device group (TV, AC, Projector)
- Runs profile-based brute-force transmit from SD files:
  - `/univ_tv.ir`
  - `/univ_ac.ir`
  - `/univ_proj.ir`

2. File Browser
- Navigate SD directories on OLED
- Open IR files and transmit captured raw commands

3. Learn New Remote
- Listens for an IR signal through the receiver
- Captures raw timing data
- Saves new commands to:
  - `/captures/learned.ir`
- Auto-names learned commands as:
  - `Learned_001`, `Learned_002`, etc.

## SD Card Files

Expected profile files for universal mode:
- `/univ_tv.ir`
- `/univ_ac.ir`
- `/univ_proj.ir`

Learned commands are appended to:
- `/captures/learned.ir`

## Build and Flash

Use your Arduino/ESP32 workflow in VS Code:
1. Select the correct board for Heltec ESP32 LoRa V3 3.2
2. Select port/programmer
3. Verify (compile)
4. Upload

## Notes

- If universal mode appears inactive, confirm the required profile file exists on SD.
- If Learn mode does not capture, verify IR receiver wiring and remote alignment.
- Keep SD card formatted and readable by the firmware.

## Name

SecuriTea means "spilling the tea about security" - practical security tooling with a fun identity.
