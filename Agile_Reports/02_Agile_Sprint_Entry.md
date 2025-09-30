Agile Sprint Log (Iteration Report)

Project Title: WearableTech-AV-JayG
Sprint #: 1.2
Date Range: Sept 2 – Sept 16 (Week 2 - 3 Research)
Prepared by: Jay J Gurung

⸻

Sprint Stage Checklist
	✓	Speculate / Plan 
	✓	Research
	•	Design
	•	Make / Produce
	•	Publish / Present
	•	Assess

⸻

0. Review of Previous Sprint (CORE)
	•	Previous Sprint Goal: Identify and evaluate microphone options beyond the unreliable electret module tested last semester.
	•	Achieved / Not Achieved: Achieved — researched multiple alternatives, clarified suitability.
	•	Problems or Bottlenecks Found: Confusion between analog MEMS (ICS-40730) and digital I²S options; difficulty ensuring compatibility with ESP32.
	•	Feedback Received (peer/faculty/AI): Encouraged to consider digital Electret microphones but research indicated INMP441 would be better alternative to avoid analog gain/noise issues.
	•	Carry-over Tasks: Select a specific mic breakout to purchase and integrate into ESP32 testing.

⸻

1. Speculate / Plan (CORE)
	•	Current Sprint Goal (based on last assessment): Decide on a microphone module and supporting components that meet project needs for amplitude + frequency analysis.
	•	Key Tasks (checklist):
	•	Research MEMS microphones (digital vs analog).
	•	Verify ESP32 compatibility with chosen mic.
	•	Assess battery and charger modules for powering ESP32 + LEDs.
	•   Planned Deliverables: Research summary; finalized microphone choice; BOM for power and protection parts.
	•	Research Needed: Amplitude vs frequency detection methods, FFT library needs, and wiring requirements for I²S.

⸻

2. Research (OPTIONAL)
    Questions Investigated:
        •   Which microphone can reliably provide both amplitude and frequency data?
        •   Is a digital I²S mic superior to analog electret/AGC modules for wearables?
        •   What supporting components are needed for ESP32 + LED jacket builds?
	Sources Consulted:
        •   INMP441 datasheets and Arduino/ESP32 I²S examples.
        •   DFRobot Fermion SEN0526 (ICS-43434 I²S MEMS mic) documentation.
        •   Comparison to analog electret modules (MAX9814, MAX4466).
        •   Battery datasheets (LI21700JSV-50) and charger module specs (DFR0668).
	Findings / Insights:
	    •   INMP441 / Fermion SEN0526 (ICS-43434) are digital I²S microphones, plug-and-play with ESP32, ideal for FFT analysis and amplitude detection.
	    •   Analog MEMS (ICS-40730, electret mics) require amplification/ADC and are less stable; not recommended for frequency analysis.
	    •   ESP32 DevKitC handles I²S data + FFT comfortably; S3 variant offers more I/O and DSP headroom.
	    •   Battery: Jauch 21700 (5000 mAh) with PCM + fuse provides adequate runtime (2+ hours with LEDs).
	    •   Charger: DFR0668 is compatible but limited to 500 mA; overnight charging acceptable for class project.

⸻

3. Assess (CORE)
	•	Successes This Sprint: Narrowed mic choice to Fermion SEN0526 (I²S MEMS digital), confirmed ESP32 compatibility, identified battery/charger solutions, and safety components (polyfuse, capacitor, data resistor).
	•	Current Bottlenecks: Charger speed (10–12 h full charge); may need higher-current charger in future iterations.
	•	Feedback Received (peer/faculty/AI): Confirmed viability of digital MEMS approach; recommended prebuilt charger modules instead of bare ICs.
	•	Adjustments for Next Sprint: Order microphone, ESP32, LED strips, and charger. Begin bench tests with I²S mic → FFT → LED mapping pipeline.

4. Shopping list:
    •  https://www.digikey.com/short/525hn9th

5. Disclosures:
    -   Reaserch Assisted by ChatGpt.
    -   Report Summary and Compilation Assisted by Chatgpt.
    -   Research Archive : https://chatgpt.com/share/68b726d3-7894-8006-bfcb-972431b2e975

