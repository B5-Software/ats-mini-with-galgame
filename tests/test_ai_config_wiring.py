"""Source-level security and wiring smoke tests. Hardware integration remains a separate test."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
def read(name):
    return (ROOT / "ats-mini" / name).read_text(encoding="utf-8")

class AiConfigWiring(unittest.TestCase):
    def test_provider_storage_uses_nvs_and_is_independent(self):
        source = read("AiSettings.cpp")
        for name in ("chat_url", "chat_model", "chat_key", "img_url",
                     "img_model", "img_key", "img_format"):
            self.assertIn(f'"{name}"', source)
        self.assertIn('STORAGE_PARTITION', source)
        self.assertIn('url.startsWith("http://") && effectiveKey.length()', source)

    def test_public_build_has_no_baked_api_key(self):
        self.assertNotIn('AI_API_KEY', read("AiConfig.h"))
        self.assertNotIn('AI_CHAT_URL', read("AiConfig.h"))
        self.assertNotIn('AI_IMAGE_URL', read("AiConfig.h"))
        self.assertIn("aiLoadSettings(AiService::Chat)", read("AIGalGame.cpp"))
        self.assertIn("aiLoadSettings(AiService::Image)", read("AIGalGame.cpp"))

    def test_no_key_leak_to_generated_image_url(self):
        source = read("AIGalGame.cpp")
        image = source[source.index('static void parseAndStoreImage('):
                       source.index('static void doSummarizeNow(')]
        self.assertNotIn('hc.addHeader("Authorization"', image)
        self.assertIn('arr = dj["data"]', image)

    def test_config_post_requires_auth_csrf(self):
        source = read("Network.cpp")
        self.assertIn("#define ENABLE_ASYNC_WEB 1", source)
        self.assertIn('if (!webValidPost(request)) return;', source)
        self.assertIn('request->getParam("csrf", true)->value() != webCsrfToken', source)
        self.assertIn('server.on("/ai", HTTP_POST, webSetAi)', source)
        self.assertIn('WiFi.softAP(apSSID, loginPassword.c_str()', source)
        self.assertIn("static const String webAiPage()", source)

if __name__ == "__main__":
    unittest.main()
