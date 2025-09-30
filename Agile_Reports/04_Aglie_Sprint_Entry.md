Agile Sprint Log (Iteration Report)

Project Title: WearableTech-AV-JayG
Sprint #: 2
Date Range: Sept 13 – Sept 17 (Week 4 – 7 Research & Build Progress)
Prepared by: Jay J Gurung

⸻

1. Workflow Steps & Build Progress
	•	Power System
	•	Integrated rechargeable lithium-ion battery module with USB-C charger board.
	•	Added physical power switch to control current flow between the battery and ESP32.
	•	System successfully runs untethered from USB power.
	•	Microcontroller & Processing
	•	ESP32 DevKitC in use (chosen for I2S mic support and FFT capability).
	•	Current code implements FFT with 5 frequency bands (bass → treble).
	•	LED Control
	•	WS2812B addressable LED chain installed with 5 LEDs.
	•	Each LED mapped to one frequency band.
	•	LEDs respond to FFT magnitudes with brightness scaling.
	•	Microphone
	•	Microphone hardware selection done (I2S MEMS mic planned).
	•	Physical installation step remains pending.

⸻

2. Component Upgrade Assessment
	•	Battery & Power
	•	Added lithium-ion battery with charger board (USB-C).
	•	Power switch for safe on/off control during battery mode.
	•	LEDs
	•	Transitioned from earlier 3-band mapping to 5-band LED chain.
	•	Microphone
	•	I2S microphone module is the next hardware addition for clean signal capture.

⸻

3. Issues Encountered
	•	One LED in chain remained permanently on until sensitivity values are tuned.
	•	Mic not yet installed, preventing real-world FFT validation.
	•	Sensitivity thresholds for each band need calibration.

⸻

4. Updates to Code & Components
	•	Code
	•	Expanded FFT from 3 to 5 bands.
	•	LED mapping scaled to 5 LEDs, one per band.
	•	Serial monitor support being prepared for debugging loudness per band.
	•	Components
	•	Added rechargeable battery + USB-C charger board.
	•	Integrated hardware power switch.
	•	WS2812B LED chain mounted and responsive.

⸻

5. Future Iterations
	•	Install I2S MEMS microphone (INMP441 or equivalent).
	•	Fine-tune sensitivity thresholds for LED response.
	•	Add smoothing filters to reduce flicker.
	•	Test full portable operation (battery run-time).
	•	Begin exploring optional WiFi/OSC expansion for remote monitoring.

⸻

6. Research Findings
	•	5-band frequency mapping provides more dynamic visualization than 3-band setup.
	•	ESP32 performance remains stable with 1024-point FFT at 16 kHz sample rate.
	•	Battery operation with switch provides safe, portable use case.
	•	Addressable LEDs (WS2812B) continue to be efficient for small band-to-LED mapping.

⸻

7. Next Steps
	•	Complete installation of microphone.
	•	Validate FFT input against real sound sources.
	•	Calibrate LED sensitivity values via Serial monitor testing.
	•	Document portable power consumption with multimeter readings.

⸻

8. Summary

The project has advanced from planning into a functional 5-band audio-reactive LED system powered by a rechargeable lithium-ion battery. A USB-C charger module and physical switch ensure safe, portable operation. The ESP32 executes FFT processing and drives 5 WS2812B LEDs mapped to 5 frequency bands. The next major milestone is the installation and calibration of the I2S microphone, which will enable true real-time audio reactivity. Once the mic is integrated, the focus will shift to tuning LED sensitivity, smoothing, and preparing for possible WiFi expansion.

⸻

9. Disclosures
	•	Research and documentation assisted by ChatGPT-5.
	•	Report compiled in Agile format for Sprint #4.