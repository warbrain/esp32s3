# Waveshare 1.75C 本地 Voice 固件

使用 Muse ESP32 SDK 的主页和设置，语音通过 BLE 或 Wi‑Fi UDP 送到本地 Windows Voice。手表主页的 Voice 页面提供自动转写、按住说话和可选朗读。上游版本记录在 `UPSTREAM_REVISION`，许可证随源码保留。

从仓库根目录，在 ESP-IDF 6.1.1 环境下构建：

```powershell
idf.py -C firmware/muse/esp32 build
```

产物位于 `firmware/muse/esp32/build/`。该工程使用 Muse 分区表，旧固件位于 `firmware/main/`。电脑端 Voice 仍位于 `voice/`。

固件可以独立构建。自动转写需要支持 VAD 授权和音频帧序号的 Windows Voice，使用 BLE 时还需要配套的蓝牙转发程序。固件构建和电脑端部署可以分别进行。

本地 Voice 配置默认启用 `CONFIG_MUSE_UI_TELEMETRY`。串口启动日志显示 LVGL 版本，主页每约 5 秒汇总动画帧率、绘制和差异比较的平均与峰值耗时、提交刷新面积。面板传输和语音链路继续各自记录统计。
