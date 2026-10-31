# door_access · 双核智能门禁（ESP32-S3 侧）

ESP32-S3 负责「看与说」——摄像头人脸识别、LVGL 交互、云端上报；
GD32F303ZE 负责「做与守」——协议校验、继电器开锁、密码备用、独立看门狗。
两板之间只有一根 UART（115200，IO10/IO11 ↔ PA3/PA2）。

- 方案文档：`docs/项目总览.html`
- 硬件事实来源：`esp32s3_learn/硬件说明.md`、`esp32s3_learn/08-dvp-camera`

## 模块进度

| 编号 | 模块 | 状态 | 提交 |
|---|---|---|---|
| M0 | 工程骨架（编译 + 启动自检） | ✅ | `6c84ab0` |
| M1 | 屏 BSP（ST7796 竖屏 320×480 + LVGL 9.6 + 中文） | ✅ | `d5f92be` |
| M2 | 触摸 BSP（FT6336 轮询，释放 IO10） | ✅ | `86be421` |
| M3 | 摄像头预览（GC0308 QVGA，旋转 90° 显示） | ✅ | `f46169b` |
| M4 | 人脸检测（MSRMNP_S8_V1 + 四角括号 + 稳定帧） | ✅ | `e34d754` |
| M5 | 人脸识别 + 人脸库（MFN_S8_V1） | ✅ | `50db67d` |
| M6 | UI 八界面状态机 | 待做 | |
| M7 | 协议库 link_proto + PC 模拟器 | 待做 | |
| M8 | UART 链路任务（心跳 / 开锁请求 / 重传） | 待做 | |
| M9 | GD32 侧固件（另一个仓库） | 待做 | |
| M10 | 动作挑战活体 | 待做 | |
| M11 | 端到端联调 | 待做 | |
| M12 | MQTT 上报 + 看板 | 待做 | |
| M13 | 交付文档 + 12V 干扰整改 | 待做 | |

## 构建与烧录

```bash
. /mnt/data/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB1 flash        # CH343（1a86:7522）→ ESP32 板
idf.py -p /dev/ttyACM0 flash        # ESP32 内置 USB-JTAG，更稳
```

N16R8：16MB Flash + Octal 8MB PSRAM，分区表见 `partitions.csv`
（`storage` 1M FAT 分区用来放人脸库 `/storage/face.db`）。

## 串口

| 设备 | VID:PID | 用途 |
|---|---|---|
| `/dev/ttyUSB0` | 1a86:**7523** | GD32 麒麟板（CH340C，USART0 调试口） |
| `/dev/ttyUSB1` | 1a86:**7522** | ESP32 板（CH343，UART0 控制台） |
| `/dev/ttyACM0` | 303a:1001 | ESP32 内置 USB-JTAG |

两个 CH34x 会抢编号，插拔后务必用 `udevadm info -q property -n /dev/ttyUSBx | grep ID_MODEL_ID`
确认哪个是 7522（ESP32）。

## 踩坑记录（都写进对应源码注释了）

1. **中文字库必须 `--no-compress`**：`lv_font_conv` 默认输出压缩位图，
   LVGL 9.6 下字形表查得到（ok=1、adv/box 正常）但一个像素都不画。
2. **面板方向的权威是 `disp_cfg.rotation`**：`esp_lvgl_port` 在 `add_disp` 时会用它
   重新调 `swap_xy/mirror`，覆盖 `bsp_lcd_init` 里的设置。本板必须 `mirror_x=true`。
3. **IO10 三重占用**（LED6 / TP-INT / UART1_TX）：触摸写 `G_MODE=0` 切轮询，
   把 IO10 让给 UART。
4. **摄像头排线虚接**的症状是「SCCB 能识别 0x21，但一帧收不到」——
   I2C 两根线搭着、10 根数据线没通，很像软件问题。
5. **识别前必须把画面转正**：传感器横装、屏幕竖屏，直接喂原始帧给模型，
   相似度只有 0.69~0.72；转正后 0.87。
6. **字库字符集是白名单**，漏字显示成方块，新增文案要同步改 `tools/gen_fonts.sh`。
