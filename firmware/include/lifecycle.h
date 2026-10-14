#pragma once

// Implemented in main.cpp. For restarts triggered outside main.cpp (OTA
// completion, Reset WiFi): saves the RAM track buffer to flash and marks
// the restart as planned, so the next boot continues the running trip
// (same session, inactivity clock kept) instead of losing up to
// FLASH_PERSIST_INTERVAL_MS of points and splitting the track.
// Returns whether the buffer was saved (false = flash full, points still in RAM).
bool prepareForRestart();
// Additionally opens the setup portal on the next boot (Reset WiFi: a
// planned restart is not a fresh boot, and with no network saved the
// portal otherwise only opens at power-up or while charging).
void requestPortalOnNextBoot();
// Web UI "Shut down": at the next loop() pass the trip is closed, the
// peripherals released and the device deep-sleeps with ONLY the charger
// wake armed - no motion or timer wake - until a charger is connected.
void requestShutdown();
// The system clock was just set from an exact source (NTP) - the reference
// point for the next clock-drift measurement. GPS syncs are tracked in
// main.cpp itself.
void noteClockSynced();
