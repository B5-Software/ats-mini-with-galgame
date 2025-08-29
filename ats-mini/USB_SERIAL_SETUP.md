# ESP32-S3 USB串口配置说明

## Arduino IDE 设置

### 必须的板子配置：
1. **开发板**: ESP32S3 Dev Module
2. **USB CDC On Boot**: "Enabled"  ⚠️ **关键设置**
3. **USB DFU On Boot**: "Disabled" 
4. **Upload Mode**: "UART0 / Hardware CDC"
5. **USB Mode**: "Hardware CDC and JTAG"

### 其他建议设置：
- Flash Size: 16MB
- PSRAM: "OPI PSRAM"
- Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)" (或使用自定义 partitions.csv)

## 重要说明

1. **首次烧录**: 使用 UART0 (TX/RX pins) 或进入下载模式
2. **后续调试**: 通过 USB-C 口 (与烧录同一个口)
3. **若无输出**: 确认 "USB CDC On Boot" = "Enabled"
4. **串口监视器**: 波特率 115200，选择 USB CDC 端口

## 故障排除

如果仍无输出：
1. 重新选择 COM 端口 (USB Serial Device)
2. 重启 ESP32-S3 
3. 检查 USB 线是否支持数据传输
4. 确认 Arduino IDE 中板子配置正确

## 测试命令

发送以下命令测试：
```
:GG STATUS
:GG ENTER  
```

系统每30秒会自动输出心跳信息。
