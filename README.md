# ESP32-S3 High-Performance Pet Monitor 🐾

> **A professional, multi-core, asynchronous edge computing surveillance solution based on FreeRTOS and ESP32-S3.**

This project optimizes the high-performance capabilities of the ESP32-S3 to deliver seamless MJPEG streaming, real-time PIR motion recording (AVI), and instant Telegram notifications without system stutter or memory crashes.

---

## 📸 System Showcase

### 1. Real-time Monitoring Dashboard
A lightweight web interface providing instant system health metrics (Heap/PSRAM utilization) and Wi-Fi signal strength.
![Dashboard](./docs/images/dashboard.jpg)

### 2. Instant Telegram Notifications
Cloud notifications triggered by the PIR motion sensor, delivered via an asynchronous queue to prevent network latency from blocking the camera pipeline.
![Telegram](./docs/images/telegram.jpg)

---

## 🏗 System Architecture

The core of this system is a dedicated **Multimedia Pipeline** decoupled from the **Connectivity Task**, ensuring that high-latency TLS encryption for Telegram uploads never interrupts the live stream.

![Architecture](./docs/images/architecture.jpg)

---

## 🛠 Key Technical Features

*   **Multi-core Architecture (FreeRTOS)**: Strictly isolated tasks across Core 0 (Multimedia) and Core 1 (Communication) to ensure deterministic streaming performance.
*   **Memory Tiering & Triple Buffering**: Explicitly managed 8MB external PSRAM using Triple Buffering. This prevents data races between high-speed DMA camera writes and SD/HTTP tasks.
*   **Low-Latency Streaming (FPS Capping)**: Implemented MJPEG protocol optimizations with 15 FPS capping and dynamic buffering to maintain steady framerates even on congested 2.4GHz Wi-Fi networks.
*   **Asynchronous Non-Blocking Pipelines**: Utilizing FreeRTOS Queues (Producer-Consumer Pattern) to smooth the load of simultaneous photo acquisition, SD writing, and cloud uploading.
*   **System Observability**: Integrated dashboard for real-time diagnostics of Heap usage, PSRAM health, and uptime tracking to prevent long-term memory exhaustion.

---

## 🔧 Setup & Development
*   **Framework**: Arduino (ESP32)
*   **Dev Env**: [PlatformIO](https://platformio.org/)
*   **Hardware**: ESP32-S3-WROOM-1 (8MB PSRAM recommended) | OV2640 Camera | PIR Sensor | MicroSD Card

---

## 📜 License
This project is for educational and portfolio demonstration purposes.

---
*Created by [chen870527](https://github.com/chen870527)*
