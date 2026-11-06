
# ATS Mini with GalGame

Firmware for SI4732 (ESP32-S3) Mini/Pocket Receiver, enhanced with AI-powered GalGame (visual novel) features.

Project repository: [https://github.com/B5-Software/ats-mini-with-galgame](https://github.com/B5-Software/ats-mini-with-galgame)

## About This Project

This project is a fork of [esp32-si4732/ats-mini](https://github.com/esp32-si4732/ats-mini), with major modifications by B5-Software to add an AI GalGame (visual novel) mode. All original radio features are preserved, and a new interactive AI game mode is integrated.

## GalGame Feature Overview

This firmware retains the original radio features and adds interactive AI story
and image generation. **No vendor API key or model is embedded in the public
firmware**; a freshly flashed receiver must be configured before cloud AI works.

## First-time AI setup (on-device WebUI)

1. Flash the matching **ESP32-S3 / 16 MB** build from
   [GitHub Actions](https://github.com/B5-Software/ats-mini-with-galgame/actions/workflows/build-firmware.yml).
   Pick **OSPI** or **QSPI** according to your receiver's PSRAM hardware.
2. On the receiver select **WiFi Mode → AP Only** or **AP+Connect**.
   On the first boot the device displays its newly generated WebUI credentials.
   In AP mode the display shows the WiFi password again. **Username: admin** (unless changed).
3. Join the receiver's `ATS-Mini` WiFi AP using the password displayed on the device.
   Open **http://10.1.1.1/ai** in a browser. On a connected LAN you can use
   `http://atsmini.local/ai` (or the device's assigned IP).
4. Configure the **Chat / Narrative Model**: endpoint URL, exact model ID, and
   your own API key. Choose a provider preset such as SiliconFlow, OpenAI,
   DeepSeek, or OpenRouter, or enter a custom OpenAI-compatible chat endpoint.
5. Optionally configure a separate **Image Generation Model** with its own
   endpoint, model ID, API key and request format (SiliconFlow / OpenAI-compatible).
   You can run story generation without an image model.
6. Press the corresponding **Save** button. Your settings persist on the
   receiver's NVS partition after restart; you do not need to recompile.
   Blank API key fields mean **keep existing key**. Use *Remove saved key* to delete one.

**Security:** Never post your API keys in GitHub Issues or repository files.
The device's WebUI uses HTTP Basic authentication (not HTTPS); use it only
on a trusted local WiFi/AP and never expose port 80 to the public Internet.
The receiver's AP uses a unique generated WPA2 password, not an open network.
Credentials are stored in the receiver's flash without hardware-backed
encryption, so physical access to the flash may expose them. Use revocable,
low-limit provider API keys.

**Compatibility:** Text generation uses OpenAI-compatible
`POST /chat/completions` with `model` and `messages`.
Images use either SiliconFlow-style (`image_size`, `batch_size` etc.) or
OpenAI-compatible (`size`, `n`) request bodies. The response reader supports
`images` or `data` arrays with image URLs or base64 fields. Exact capabilities
and model IDs depend on your provider. Some high-resolution image models may
exceed ESP32 memory limits; successful firmware compilation does not prove
provider-side or hardware compatibility.

## How to use GalGame

1. Set WiFi Mode to **AP+Connect** or **Connect** (internet access required).
2. Enter **GalGame** from the receiver's menu.
3. Create a story, select its genre and choose interactive dialogue options.
4. If you see "Set chat URL/model at /ai", open the WebUI and configure a chat provider.
5. The project and story history are stored locally on the receiver.

## Building locally

Use `ats-mini/sketch.yaml` profiles `esp32s3-ospi` or `esp32s3-qspi` with
Arduino CLI. `ats-mini/Makefile` is supplied for supported local setups.
See [Actions](https://github.com/B5-Software/ats-mini-with-galgame/actions)
for reproducible firmware builds and SHA-256 checksums. No WiFi/API credentials
are required at compile time.

---

## Original Project Credits

This project is based on and includes code from:

* [esp32-si4732/ats-mini](https://github.com/esp32-si4732/ats-mini) (MIT License)
* Volos Projects:    https://github.com/VolosR/TEmbedFMRadio
* PU2CLR, Ricardo:   https://github.com/pu2clr/SI4735
* Ralph Xavier:      https://github.com/ralphxavier/SI4735
* Goshante:          https://github.com/goshante/ats20_ats_ex
* G8PTN, Dave:       https://github.com/G8PTN/ATS_MINI

All original copyrights and licenses are retained.

