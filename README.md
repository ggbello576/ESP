<p align="center">
  <img src="assets/CAPRIZERO.jpeg" alt="Capri Zero Logo" width="180" />
</p>

<h1 align="center">Capri Zero</h1>

<p align="center">
  <strong>Open-source modular hardware & RF cybersecurity multi-tool built from scratch on ESP32-S3.</strong>
</p>

> **Status:** Active Development — Currently working on **Phase 1** (Sub-1 GHz transceiver integration & physical UI controls on breadboard).

Capri Zero is an embedded systems and security research platform written in **C (ESP-IDF)**. Designed as an expandable, pocket-sized field tool, it combines multi-band wireless analysis (Sub-1 GHz, 2.4 GHz Wi-Fi/BLE, LoRa), cryptographic USB OTG utilities, NFC/RFID emulation, and hardware bus diagnostics into a single custom device.

---

## Current Hardware Prototype (Phase 1)
- **MCU:** ESP32-S3-N16R8 (Dual-core Xtensa LX7, 16 MB Flash, 8 MB PSRAM, native USB OTG)
- **Sub-1 GHz Radio:** TI CC1101 transceiver module (300–928 MHz over SPI)
- **Controls:** 4-button tactile navigation interface
- **Toolchain:** C11, ESP-IDF, CMake, FreeRTOS

---

## Development Roadmap

- [ ] **Phase 1 — Sub-1 GHz (300–928 MHz):** RSSI spectrum sweep, microsecond RAW signal capture & replay, OOK/FSK protocol decoding, custom signal synthesis, and +10 dBm point-to-point RF link *(ESP32-S3 + CC1101)*.
- [ ] **Phase 2 — Wi-Fi 2.4 GHz & Wi-Fi CSI Radar:** Promiscuous 802.11 sniffer with channel hopping (1–13), Beacon/Probe parser, Deauth detection (WIDS), ESP-NOW, local web dashboard, and through-wall presence/motion detection via Wi-Fi Channel State Information (CSI).
- [ ] **Phase 3 — Bluetooth LE 2.4 GHz:** BLE advertising scanner, active GATT service explorer, mobile companion control, and wireless BLE HID keyboard/mouse emulation.
- [ ] **Phase 4 — USB OTG & Cryptographic Security:** Programmable USB HID payload engine (DuckyScript), Mouse Jiggler, encrypted offline password/macro vault (NVS encryption), U2F/FIDO 2FA token, and Dead Man's Switch for rapid key erasure.
- [ ] **Phase 5 — Visual UI, Offline Audio & Storage:** Color SPI TFT display (`ST7789` 240x240), microSD logging, piezo buzzer alerts, real-time FFT audio spectrogram, and offline voice commands via local AI (`ESP-SR` + `INMP441` I2S mic).
- [ ] **Phase 6 — Infrared (IR):** Capture, decoding, and replay of IR remote protocols (TVs, HVAC units, projectors, LED controllers) via `VS1838B` receiver and 940 nm IR emitter.
- [ ] **Phase 7 — NFC (13.56 MHz) & RFID (125 kHz):** Read, dump, and emulate NFC tags (MIFARE, NTAG via `PN532`) and legacy 125 kHz RFID badges (EM4100, HID Prox via `RDM6300`).
- [ ] **Phase 8 — Off-Grid LoRa Mesh:** Long-range AES-256 encrypted point-to-point and mesh messaging/telemetry (`SX1262` / `RFM95W` 868 MHz) without cellular or internet infrastructure.
- [ ] **Phase 9 — Environmental & Optical Sensing:** 24 GHz mmWave micro-motion/distance radar (`LD2410`), EMF oscilloscope for detecting live wiring/transformers, and IR-unfiltered `OV2640` micro-camera for optical/IR beam inspection.
- [ ] **Phase 10 — iButton & Field Hardware Lab:** 1-Wire key reader/emulator, pogo-pin probe for automated UART/I2C pinout detection and SPI flash dumping, PWM square-wave generator, and 0–15 V DC voltmeter.
- [ ] **Phase 11 — Portability & Custom Enclosure:** Autonomous 1S LiPo battery power (`TP4056` charging/protection), custom perfboard integration, and 3D-printed pocket enclosure.

---

## Disclaimer
This project is developed strictly for educational purposes, embedded systems engineering research, and authorized security testing on owned hardware.