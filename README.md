
# ATS Mini with GalGame

Firmware for SI4732 (ESP32-S3) Mini/Pocket Receiver, enhanced with AI-powered GalGame (visual novel) features.

Project repository: [https://github.com/B5-Software/ats-mini-with-galgame](https://github.com/B5-Software/ats-mini-with-galgame)

## About This Project

This project is a fork of [esp32-si4732/ats-mini](https://github.com/esp32-si4732/ats-mini), with major modifications by B5-Software to add an AI GalGame (visual novel) mode. All original radio features are preserved, and a new interactive AI game mode is integrated.

## GalGame Feature Overview

- Enter the GalGame mode from the main menu to start an AI-powered visual novel experience.
- Supports multiple genres (school life, sci-fi, fantasy, etc.), project management (create, delete, resume stories), and context-aware AI conversations.
- Uses SiliconFlow API for both text and image generation. (API endpoints and key are configured in `ats-mini/AiConfig.h`.)
- On-screen logs and status messages for user feedback.
- Images are fetched as base64 and displayed as placeholders (optional JPEG/PNG decoding supported if enabled at build time).


## WiFi Configuration

You can hardcode your WiFi credentials in `ats-mini/Network.cpp` by editing the following section:

```cpp
#ifndef WIFI_SSID
#define WIFI_SSID ""      // e.g., "MyHomeAP" (leave empty to use stored/multiple networks)
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""  // e.g., "MyPassword"
#endif
```

If you do not set these values, the firmware will fall back to using stored or multiple WiFi networks as configured via the web interface or preferences.

---

## How to Use the GalGame Mode

1. **Build and flash the firmware** as you would for the original ATS Mini project.
2. **Configure your API key**:
	- Edit `ats-mini/AiConfig.h` and set your API key in `AI_API_KEY`.
	- (Optional) Enable JPEG/PNG image decoding by defining `USE_TJPG_DECODER` or `USE_PNG_DECODER` if your hardware supports it.
3. **Boot the device** and navigate to the GalGame mode from the main menu.
4. **Create or select a project** (story), choose a genre, and interact with the AI character.
5. **Use the encoder and button** to navigate options, send messages, and progress the story.
6. **Manage projects**: You can create, delete, or resume saved stories at any time.

## Original Project Credits

This project is based on and includes code from:

* [esp32-si4732/ats-mini](https://github.com/esp32-si4732/ats-mini) (MIT License)
* Volos Projects:    https://github.com/VolosR/TEmbedFMRadio
* PU2CLR, Ricardo:   https://github.com/pu2clr/SI4735
* Ralph Xavier:      https://github.com/ralphxavier/SI4735
* Goshante:          https://github.com/goshante/ats20_ats_ex
* G8PTN, Dave:       https://github.com/G8PTN/ATS_MINI

All original copyrights and licenses are retained.

## Releases

Check out the [Releases](https://github.com/B5-Software/ats-mini-with-galgame/releases) page.

## Documentation

Hardware, software, and flashing documentation is available at <https://esp32-si4732.github.io/ats-mini/> (original project).

## Discuss

* [GitHub Discussions](https://github.com/esp32-si4732/ats-mini/discussions)

