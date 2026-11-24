// Tonausgabe über waveOut
#pragma once
#include <cstdint>

// slot: 0..7 sceAudio-Kanal, 8 Output2, 9 Vaudio. Lautstärke 0..0x8000 je Seite.
void audio_freeze();   // T90: Ton hängt, letzte Blöcke wiederholen
void audio_submit(int slot, const uint8_t* mem, uint32_t buf, uint32_t samples, bool mono, int vol_l, int vol_r);

int audio_queued_ms();   // Ton in der Warteschlange + waveOut-Puffer, noch nicht hörbar
