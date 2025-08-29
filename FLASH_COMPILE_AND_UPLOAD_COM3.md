# 编译与烧录到 COM3 指南 (Windows / ESP32)

## 前置条件
- 已安装 Arduino IDE 或使用 `arduino-cli` / `esp-idf` 方式。这里示例使用 Arduino CLI（推荐自动化）。
- 已安装 ESP32 Arduino Core（>= 2.0.x）。
- 已安装所需库：TFT_eSPI、ArduinoJson、Preferences(随核心)、WiFi、HTTPClient、LittleFS、TJpg_Decoder(可选)。
- 本仓库根目录包含 `ats-mini/ats-mini.ino` 以及可选新的 16MB 分区 `partitions-16MB.csv`。

## 安装 Arduino CLI (如未安装)
```powershell
winget install Arduino.ArduinoCLI -s winget
```

## 初始化核心与库
```powershell
arduino-cli config init
arduino-cli core update-index
arduino-cli core install esp32:esp32
# 可选：安装依赖库
arduino-cli lib install ArduinoJson
arduino-cli lib install TFT_eSPI
arduino-cli lib install TJpg_Decoder
```
> 根据 `TFT_eSPI` 需要在其 `User_Setup.h` 中配置你的屏幕参数；或用预置的 `tft_setup.h` 方式。

## 选择 16MB 分区 (可选)
若需使用自定义 16MB 分区，在 `arduino-cli compile` 时加入：
```
--build-property build.partitions=partitions-16MB --build-property upload.maximum_size=15728640
```
同时在 `boards.txt` 覆盖方案或直接放入本工程 `partitions-16MB.csv` 并指定 `--build-property build.partitions=partitions-16MB`。

## 编译
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 --warnings default --build-property build.partitions=default ats-mini
```
使用 16MB 分区：
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 --warnings default --build-property build.partitions=partitions-16MB ats-mini
```
> 若模型名称/密钥等使用自定义宏，可加 `--build-property build.extra_flags="-DAI_API_KEY=\"sk-xxx\""`

## 烧录到 COM3
确保开发板已连接并识别为 COM3；然后：
```powershell
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32doit-devkit-v1 ats-mini
```
编译 + 烧录一步：
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 ats-mini; if($?) { arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32doit-devkit-v1 ats-mini }
```

## 串口监视
```powershell
arduino-cli monitor -p COM3 -b esp32:esp32:esp32doit-devkit-v1 -c baudrate=115200
```
可直接输入 `:GG` 系列命令（例如 `:GG LIST`、`:GG ENTER`）。

## 可选：启用 JPEG 解码
在 `AIConfig.h` 中取消注释：
```cpp
// #define USE_TJPG_DECODER
```
或在编译加入：
```powershell
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 --build-property build.extra_flags="-DUSE_TJPG_DECODER" ats-mini
```

## 常见问题
- 找不到库：执行 `arduino-cli lib install 库名`。
- 分区报错：确认 `partitions-16MB.csv` 位于工程根并名称匹配。
- 上传失败：按住 BOOT 后再点 RST 进入下载模式，重试 upload。
- 内存不足：尝试禁用 JPEG 解码或降低 `maxContextRounds`。

## 清理构建
```powershell
Remove-Item -Recurse -Force .\.arduino\
```
(或使用 `arduino-cli cache clean`)

## 备忘
- LLM/图像请求需 WiFi；在主菜单进入 GalGame 后等待自动拉取剧情。
- 使用 `:GG ROUNDS=12` 可动态调整上下文窗口大小。
- 长按项目列表条目进入删除确认；游戏内长按呼出菜单。

---
此文档用于快速在 Windows 下通过 COM3 编译与烧录本项目。
