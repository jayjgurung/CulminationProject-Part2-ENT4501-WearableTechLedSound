Audio-Visual Reactive System Tech

By Jay Jung Gurung — Emerging Media Technology | Culmination Project II (MTEC 4501)

🎧 Overview

Audio-Visual Reactive System Tech is an interactive hardware-software system that transforms real-time audio input into dynamic LED visualizations.
Built using an ESP32 microcontroller, I2S MEMS microphone, WS2812B LEDs, and a custom PCB, this project explores the intersection of embedded systems, acoustics, digital signal processing, and wearable/portable audiovisual design.

The system analyzes incoming sound using FFT (Fast Fourier Transform) and maps frequency energy to visual patterns, brightness, and color behavior. Additional control inputs (potentiometers, infrared remote, buttons, sensors) allow the user to modify the visual output and sensitivity on the fly.

This project aims to create an elegant, portable, and customizable audio-reactive LED module suitable for performance, stage design, wearables, and experimental media technology applications.

⸻

🚀 Features
	•	Real-time audio analysis via ESP32 I2S interface
	•	FFT-driven LED visualization (frequency bins → light patterns)
	•	Custom PCB designed in KiCad 8/9
	•	3D-printed enclosure modeled in Autodesk Fusion 360
	•	WS2812B LED output with multiple animation modes
	•	Potentiometer-based sensitivity control
	•	Optional IR remote support for future expansion
	•	Modular design for wearable or tabletop use

⸻

🧩 Hardware Components
	•	ESP32-WROVER-IE DevKit
	•	INMP441 (or Fermion) I2S MEMS microphone
	•	WS2812B LED strip or 5-LED chain
	•	Potentiometer for sensitivity
	•	IR receiver (optional)
	•	USB-C Li-ion charging board
	•	21700 lithium battery
	•	Custom PCB (routed & manufactured)
	•	3D-printed casing

⸻

🛠️ Software / Tools
	•	Arduino IDE (ESP32 Core v3.x)
	•	KiCad 8/9 for schematic + PCB design
	•	Autodesk Fusion 360 for enclosure modeling
	•	FastLED library
	•	arduinoFFT library
	•	ESP-IDF I2S standard driver (new API)

⸻

📁 Repository Structure
```text
CULMINATIONPROJECT-PART2-ENT4501-WEARABLETECHLEDSOUND/
│
├── **Agile_Reports/**
│   ├── 01_Agile_Sprint_Entry.md
│   ├── 02_Agile_Sprint_Entry.md
│   ├── 03_Agile_Sprint_Entry.md
│   ├── 04_Agile_Sprint_Entry.md
│   ├── 05.1_Agile_Sprint_Entry.md
│   └── 05.2_Agile_Sprint_Entry.md
│
├── **Docs/**
│   ├── Budget Report/
│   ├── KiCad/
│   ├── Panel Feedback/
│   ├── Reference/
│   └── Timeline/
│
├── **Iteration_Plans/**
│   └── IterationPlan-Roadmap.md
│
├── **Presentations/**
│   ├── Culmination-Part1-ProgressReport_Presentation_Fall2024.pdf
│   ├── Jury#1 Presentation links -JayGurung-fall2025.md
│   ├── Presentation_#2_JayGurung.pptx
│   └── Presentation-w-video-link.md
│
├── **src/**
│   ├── Arduino IDE/
│   └── KiCad/
│
├── .gitignore
└── README.md
```

⸻

📊 System Workflow
	1.	Microphone captures audio through I2S
	2.	ESP32 samples audio at 16 kHz
	3.	FFT processes 1024-sample frames
	4.	Frequency bins → mapped to LED color, brightness, animations
	5.	Potentiometer adjusts sensitivity threshold
	6.	LEDs render animations in real time

⸻

🎨 Visual Modes

(Future expansion possible)
	•	Low-frequency pulsing
	•	Mid-range bar graph effect
	•	High-frequency sparkle
	•	Full-spectrum rainbow reactive
	•	Silence → idle animation

⸻

🧪 Current Status
	•	Hardware wiring + soldering complete
	•	PCB v1 assembled and tested
	•	Audio capture + FFT working
	•	LED visualization stable
	•	Pot control implemented
	•	3D casing in prototype stage

⸻

📚 Acknowledgements
	•	Mentored by Professor Dr. David Smith
	•	Open-source libraries: FastLED, arduinoFFT
	•	Espressif documentation & community forums

⸻

📄 License

This project is for academic use under MTEC 4501 at City Tech.
Feel free to reference or fork with credit.