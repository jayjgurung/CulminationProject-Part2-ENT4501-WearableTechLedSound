Agile Sprint Log (Iteration Report)

Project Title: WearableTech-AV-JayG
Sprint #: 3
Date Range: Sept 18 – Sept 29 (Week 8 – 9 Development & Debugging Phase)
Prepared by: Jay J Gurung

⸻

1. Workflow Steps & Build Progress
	•	Power System
	•	Battery + USB-C charger integration continues to function as intended.
	•	Verified safe boot cycles under both USB and battery modes.
	•	Microcontroller & Processing
	•	ESP32-WROVER in use (selected for additional memory and I2S handling).
	•	FFT pipeline stable at 1024-point, 16 kHz sample rate.
	•	Implemented calibration via potentiometer input (GPIO34) to control sensitivity thresholds.
	•	LED Control
	•	WS2812B 5-LED chain mapped to FFT bands.
	•	Adjusted band thresholds to improve high-frequency LED triggering.
	•	Serial output added for monitoring per-band amplitude values.
	•	Microphone
	•	I2S MEMS microphone wiring complete (WS, SCK, SD pins verified).
	•	Confirmed digital input streaming with test code; calibration in progress.
	•	New Feature (IR Remote)
	•	Began integrating IR remote input for mode toggling (ambient vs. audio-reactive).

⸻

2. Component Upgrade Assessment
	•	Potentiometer
	•	Repurposed as a mic sensitivity dial.
	•	Verified analog input on GPIO34; prevents overdriving LED output.
	•	LEDs
	•	Improved mapping of treble band; now more responsive with realistic sound levels.
	•	IR Remote
	•	Testing basic IR decode library for ESP32.
	•	Initial command mapping planned (mode switching, brightness control).

⸻

3. Issues Encountered
	•	Permanent LED On
	•	Low-frequency LED occasionally stays lit due to baseline noise floor.
	•	Requires further threshold adjustment with pot + code calibration.
	•	Mic Sensitivity
	•	Potentiometer heating issue noted during testing; possible need for resistor or wiring adjustment.
	•	Code Stability
	•	Encountered I2S driver conflicts (resolved by switching to new API: i2s_std.h).
	•	Core dump errors observed when flashing larger builds; investigated memory allocation.

⸻

4. Updates to Code & Components
	•	Code
	•	Updated to new ESP32 I2S standard API (i2s_std.h).
	•	Added potentiometer input for live sensitivity scaling.
	•	Added serial debugging for band-by-band amplitude values.
	•	Started framework for IR remote handling.
	•	Components
	•	Installed and tested I2S MEMS microphone.
	•	Integrated potentiometer control into live loop.
	•	WS2812B LEDs tuned with adjusted thresholds.

⸻

5. Future Iterations
	•	Finalize IR remote integration (mode toggling, brightness presets).
	•	Smooth LED flicker using moving average or exponential filter.
	•	Optimize FFT bin grouping for better distribution across 5 LEDs.
	•	Document mic calibration process (pot vs. hard-coded thresholds).
	•	Begin preparing visuals/slides for Jury #1 presentation.

⸻

6. Research Findings
	•	ESP32-WROVER handles I2S mic + FFT without performance bottleneck.
	•	Potentiometer provides flexible live calibration but requires safe wiring to prevent overheating.
	•	High-frequency FFT bins naturally have lower amplitude; scaling adjustments improve LED balance.
	•	Switching from legacy i2s.h to i2s_std.h ensures compatibility with ESP32 core v3.x.

⸻

7. Next Steps
	•	Complete IR remote command mapping.
	•	Fine-tune mic baseline and LED band thresholds with real audio.
	•	Add smoothing algorithm to stabilize LED flicker.
	•	Update GitHub repo with new code + circuit diagrams.
	•	Prepare Jury #1 deck: slides covering overview, current build, timeline, and budget.

⸻

8. Summary

The project has progressed into an integrated wearable audio-reactive system. The ESP32-WROVER now runs FFT analysis with an I2S MEMS mic input, mapped to a 5-LED WS2812B chain. A potentiometer provides live sensitivity control, addressing threshold calibration challenges. IR remote functionality has been initiated for flexible mode switching. Remaining work focuses on fine-tuning mic sensitivity, smoothing LED response, and preparing for the Jury #1 presentation.

⸻

9. Disclosures
	•	Research and documentation assisted by ChatGPT-5.
	•	Report compiled in Agile format for Sprint #5.