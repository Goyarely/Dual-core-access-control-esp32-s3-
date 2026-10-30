# door_access · 双核智能门禁（ESP32-S3 侧）

ESP32-S3 负责「看与说」——摄像头人脸识别、LVGL 交互、云端上报；
GD32F303ZE 负责「做与守」——协议校验、继电器开锁、密码备用、独立看门狗。
两板之间只有一根 UART（115200，IO10/IO11 ↔ PA3/PA2）。

方案文档：`docs/项目总览.html`

## 模块进度

| 编号 | 模块 | 状态 |
|---|---|---|
| M0 | 工程骨架（编译 + 启动自检） | ✅ |
| M1 | 屏 BSP（ST7796 竖屏 320×480 + LVGL） | 待做 |
| M2 | 触摸 BSP（FT6336 轮询，释放 IO10） | 待做 |
| M3 | 摄像头预览（GC0308 → LVGL canvas） | 待做 |
| M4 | 人脸检测（MSR+MNP，稳定 3 帧） | 待做 |
| M5 | 人脸识别 + NVS 人脸库 | 待做 |
| M6 | UI 八界面状态机 | 待做 |
| M7 | 协议库 link_proto + PC 模拟器 | 待做 |
| M8 | UART 链路任务（心跳/开锁请求/重传） | 待做 |
| M9 | （GD32 侧固件，另一个仓库） | 待做 |
| M10 | 动作挑战活体 | 待做 |
| M11 | 端到端联调 | 待做 |
| M12 | MQTT 上报 + 看板 | 待做 |
| M13 | 交付文档 + 12V 干扰整改 | 待做 |

## 构建

```bash
. /mnt/data/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

16MB Flash + Octal 8MB PSRAM（N16R8），分区表见 `partitions.csv`。
