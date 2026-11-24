// Ersatz für PPSSPPs Common/Log.h: Meldungen des Decoders werden verworfen.
#pragma once
namespace Log { enum { ME }; }
#define DEBUG_LOG(cat, ...) ((void)0)
#define INFO_LOG(cat, ...) ((void)0)
#define WARN_LOG(cat, ...) ((void)0)
#define ERROR_LOG(cat, ...) ((void)0)
