// Ersatz für PPSSPPs Common/MemoryUtil.h (ausgerichteter Speicher).
#pragma once
#include <malloc.h>
#include <cstddef>
inline void* AllocateAlignedMemory(size_t size, size_t alignment) { return _aligned_malloc(size, alignment); }
inline void FreeAlignedMemory(void* p) { _aligned_free(p); }
