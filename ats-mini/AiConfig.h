// Image decoders are enabled by default for AI GalGame.
// AI provider URLs, model IDs and keys are configured at runtime via /ai,
// and are stored in the receiver's dedicated NVS partition (see AiSettings.cpp).
#pragma once

#define USE_TJPG_DECODER
#define USE_PNG_DECODER
