// H.264-Bild der PSMF-Videos
#pragma once
#include <cstdint>

void mpegv_reset();                                              // neues Video
void mpegv_feed_packets(const uint8_t* data, int packets);       // frisch gefüllte Ringpuffer-Pakete (je 2048 Byte)
bool mpega_decode(uint8_t* mem, uint32_t buf);                    // ATRAC3plus-Rahmen -> 2048 Samples Stereo
bool mpegv_decode(uint8_t* mem, uint32_t dst, uint32_t fw);      // ein Bild als RGBA 8888 nach dst; true = neues Bild
