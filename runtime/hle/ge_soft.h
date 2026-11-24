#pragma once
#include <cstdint>

void ge_soft_cmd(uint8_t* mem, uint32_t word, uint32_t base);   // ein Befehl der Displayliste (außer Sprüngen/Ende)
