// Ersatz für PPSSPPs ppsspp_config.h (nur was at3_standalone braucht): Windows x64 mit SSE2.
#pragma once
#define PPSSPP_ARCH(x) PPSSPP_ARCH_##x
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_SSE2 1
#define PPSSPP_ARCH_ARM_NEON 0
#define PPSSPP_ARCH_ARM64 0
