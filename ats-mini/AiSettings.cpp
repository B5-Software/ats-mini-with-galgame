#include "AiSettings.h"
#include "Storage.h"
#include <Preferences.h>

namespace {
constexpr const char *kNamespace = "ggaiconfig";

bool validUrl(const String &url) {
  return (url.startsWith("https://") || url.startsWith("http://")) &&
         url.length() < 200 && url.indexOf('\r') < 0 &&
         url.indexOf('\n') < 0 && url.indexOf(' ') < 0;
}

bool validText(const String &value, size_t limit) {
  return value.length() < limit && value.indexOf('\r') < 0 &&
         value.indexOf('\n') < 0;
}
} // namespace

AiServiceSettings aiLoadSettings(AiService service) {
  AiServiceSettings result;
  Preferences storage;
  if (!storage.begin(kNamespace, true, STORAGE_PARTITION)) return result;

  if (service == AiService::Chat) {
    result.url = storage.getString("chat_url", "");
    result.model = storage.getString("chat_model", "");
    result.apiKey = storage.getString("chat_key", "");
  } else {
    result.url = storage.getString("img_url", "");
    result.model = storage.getString("img_model", "");
    result.apiKey = storage.getString("img_key", "");
    result.imageFormat = storage.getString("img_format", "siliconflow");
  }
  storage.end();
  return result;
}

bool aiSaveSettings(AiService service, const String &url, const String &model,
                    const String &apiKey, bool clearKey,
                    const String &imageFormat, String &error) {
  if (!validUrl(url)) {
    error = "URL must start with http(s):// and be under 200 characters";
    return false;
  }
  if (!validText(model, 128) || !validText(apiKey, 512)) {
    error = "Invalid model or API key (length or newline)";
    return false;
  }
  if (service == AiService::Image &&
      imageFormat != "siliconflow" && imageFormat != "openai") {
    error = "Invalid image API format";
    return false;
  }

  // HTTP without TLS is permitted only for keyless local services such as Ollama.
  AiServiceSettings old = aiLoadSettings(service);
  String effectiveKey = clearKey ? String() : (apiKey.length() ? apiKey : old.apiKey);
  if (url.startsWith("http://") && effectiveKey.length()) {
    error = "API keys require HTTPS. Use a keyless local HTTP endpoint or HTTPS.";
    return false;
  }

  Preferences storage;
  if (!storage.begin(kNamespace, false, STORAGE_PARTITION)) {
    error = "Could not open settings storage";
    return false;
  }
  const bool chat = service == AiService::Chat;
  const char *urlKey = chat ? "chat_url" : "img_url";
  const char *modelKey = chat ? "chat_model" : "img_model";
  const char *secretKey = chat ? "chat_key" : "img_key";
  // NVS allows up to 15 characters per key.
  bool ok = storage.putString(urlKey, url) == url.length();
  ok = (storage.putString(modelKey, model) == model.length()) && ok;
  if (clearKey) {
    ok = storage.remove(secretKey) && ok;
  } else if (apiKey.length()) {
    ok = (storage.putString(secretKey, apiKey) == apiKey.length()) && ok;
  }
  if (!chat) {
    ok = (storage.putString("img_format", imageFormat) == imageFormat.length()) && ok;
  }
  storage.end();
  if (!ok) error = "Failed writing one or more settings";
  return ok;
}
