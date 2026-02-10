# Squawk Box v9.0
**Hardware:** Particle Photon 2 | **Role:** Momentum Trend Monitor & Squawk

A high-frequency momentum scanner for the Particle Photon 2 that tracks the spread between Fast and Slow Exponential Moving Averages (EMAs). It features a real-time web dashboard with auto-updating alerts, event logging, and a "Velocity Spike" detection engine.

## 🚀 Key Features
* **Smart Polling Engine:** Automatically switches update rates based on market hours:
    * **Turbo (2s):** Market Open (9:30-11:00 AM) & Power Hour (3:00-4:00 PM).
    * **Standard (4s):** Pre-Market Warmup (8:30-9:30 AM) & Mid-day trading.
    * **Sleep (60s):** Before 8:30 AM & After 4:00 PM.
* **Velocity Acceleration Logic:** Distinguishes between a normal trend breakout and a high-speed "Rush" or "Dump" (triggered by a >20% spike in momentum).
* **Live Web Dashboard:**
    * **Responsive UI:** 600px mobile-friendly design.
    * **Auto-Updating Alerts:** Pushes the last 5 Bull/Bear events to the browser in real-time.
    * **Gold Logic:** Visual feedback on active presets (SPY/QQQ/IWM).
    * **Self-Documenting:** Includes an on-device "About" section with graph legends and hardware specs.
* **Audio Feedback:**
    * **Solid Tone:** Standard Breakout.
    * **Stutter Tone:** High-urgency Acceleration.

## 🛠 Hardware Setup
* **Microcontroller:** Particle Photon 2 (WiFi 6).
* **Display:** 16x2 LCD with I2C Backpack (Address `0x27`).
* **Audio:** Active 5V Buzzer connected to `Pin D2`.
* **Input:** Tactile Switch connected to `Pin D3` (Ground to trigger).

## 📊 Strategy Parameters
The device calculates `Diff = EMA(Fast) - EMA(Slow)`.

| Preset | Fast Alpha | Slow Alpha | Chop Limit |
| :--- | :--- | :--- | :--- |
| **SPY** | 0.22 | 0.10 | 0.05 |
| **QQQ** | 0.25 | 0.12 | 0.06 |
| **IWM** | 0.28 | 0.14 | 0.07 |

## 📦 Installation
1.  Flash `src/squawk_box_v9.cpp` to your Particle Photon 2.
2.  Ensure you have a valid Finnhub API webhook named `finnhub_quote` set up in the Particle Console.
3.  Access the dashboard via the device's local IP address (e.g., `http://192.168.1.50`).

## 📜 License
Designed and built by **Jason Edgar** in Orillia, Ontario.  
MIT License.
