#pragma once

#include <Arduino.h>

// Chat and image generation can use separate OpenAI-compatible providers.
enum class AiService : uint8_t { Chat, Image };

struct AiServiceSettings {
  String url;
  String model;
  String apiKey;
  String imageFormat;  // "siliconflow" or "openai" for image requests

  bool ready() const { return url.length() && model.length(); }
};

AiServiceSettings aiLoadSettings(AiService service);
// Empty key keeps the saved key unless clearKey is true.
bool aiSaveSettings(AiService service, const String &url, const String &model,
                    const String &apiKey, bool clearKey,
                    const String &imageFormat, String &error);
