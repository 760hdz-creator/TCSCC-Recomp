// Guest clocks: KeTimeStampBundle (kernel-updated every ms) and the mftb timebase frequency.
#pragma once
#include <cstdint>

// Writes InterruptTime (100 ns since start) @0, SystemTime (FILETIME) @8, TickCount (ms) @0x10.
void UpdateTimeStampBundle(uint32_t bundle);

// Host TSC frequency, calibrated once against QPC.
uint64_t TimebaseFrequency();

// Guest mftb rate (Xenos/Xenon timebase): what KeQueryPerformanceFrequency reports.
constexpr uint64_t kGuestTimebaseHz = 50000000;
extern "C" uint64_t PpcGuestTimebase();

// Updates the KeTimeStampBundle data import every millisecond.
void GuestTimeStart(uint32_t bundle);
