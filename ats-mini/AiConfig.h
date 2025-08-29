// AIConfig.h - API endpoints and keys (DO NOT COMMIT real secrets)
#pragma once

// 固定配置：禁止运行时/偏好修改（已在代码中移除动态覆盖逻辑）
static const char *AI_CHAT_URL   = "";
static const char *AI_IMAGE_URL  = "";
static const char *AI_CHAT_MODEL = "";
static const char *AI_IMG_MODEL  = "";
// Provide your API key securely (e.g. injected at build or stored encrypted). For now blank.
static const char *AI_API_KEY    = ""; // 硬编码，不允许修改

// Optional: enable JPEG decoding of AI images (needs TJpg_Decoder library) by adding -DUSE_TJPG_DECODER to build flags
// or uncomment the line below (ensure library available and PSRAM sufficient):
#define USE_TJPG_DECODER

// Optional: enable PNG decoding & thumbnail caching (requires PNGdec or similar lightweight decoder)
// Add build flag -DUSE_PNG_DECODER and ensure library available.
#define USE_PNG_DECODER
