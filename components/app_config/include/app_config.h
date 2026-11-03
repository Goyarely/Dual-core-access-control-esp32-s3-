#pragma once
/*
 * 双核智能门禁 · ESP32-S3 侧 —— 引脚与策略常量
 *
 * 引脚唯一事实来源：esp32s3_learn/硬件说明.md（2026-10-28 实测）
 *
 * ⚠ IO10 三重占用（本工程最需要小心的一根脚）：
 *      板载 LED6（低电平点亮） / 触摸 TP-INT / 本工程 UART1_TX
 *   取舍：UART 用 IO10，触摸初始化时必须写 G_MODE=0 放弃中断改轮询，
 *        否则摸屏会往 TX 线上灌电平，串口直接乱码。
 */

#define APP_VERSION "0.1.0-M1"

/* ---------------- I2C（PCA9557 / 触摸 / 摄像头 SCCB 共用） ---------------- */
#define I2C_SDA_GPIO        1
#define I2C_SCL_GPIO        2
#define I2C_FREQ_HZ         (400 * 1000)

/* ---------------- PCA9557（0x19） ---------------- */
#define PCA9557_ADDR        0x19
#define PCA9557_IO_LCD_CS   0   /* 输出，低有效 */
#define PCA9557_IO_PA_EN    1   /* 输出，高有效 */
#define PCA9557_IO_DVP_PWDN 2   /* 输出，低有效（拉低摄像头才应答 SCCB） */

/* ---------------- LCD ST7796（3.5 寸竖屏 320x480） ---------------- */
#define LCD_MOSI_GPIO       40
#define LCD_SCLK_GPIO       41
#define LCD_DC_GPIO         39
#define LCD_BL_GPIO         42  /* 背光 PWM，高有效 */
#define LCD_H_RES           320
#define LCD_V_RES           480
#define LCD_PCLK_HZ         (10 * 1000 * 1000)

/* ---------------- 触摸 FT6336（0x38） ---------------- */
#define TP_I2C_ADDR         0x38
#define TP_INT_GPIO         10  /* 与 UART1_TX 冲突，靠 G_MODE=0 让出 */
#define TP_RST_GPIO         (-1)

/* ---------------- 摄像头 GC0308 ---------------- */
#define CAM_XCLK_GPIO       5
#define CAM_D7_GPIO         9
#define CAM_D6_GPIO         4
#define CAM_D5_GPIO         6
#define CAM_D4_GPIO         15
#define CAM_D3_GPIO         17
#define CAM_D2_GPIO         8
#define CAM_D1_GPIO         18
#define CAM_D0_GPIO         16
#define CAM_VSYNC_GPIO      3
#define CAM_HREF_GPIO       46
#define CAM_PCLK_GPIO       7
#define CAM_XCLK_HZ         24000000  /* GC0308 手册硬性要求，20MHz 会出花屏 */
#define CAM_FB_COUNT        2         /* 双缓冲；4 帧留给带宽宽裕后再试 */

/* ---------------- 双板链路 UART1 ---------------- */
#define LINK_TX_GPIO        10
#define LINK_RX_GPIO        11
#define LINK_BAUD           115200

/* 后端上报地址（PC 上跑 tools/backend/server.py 的那台机器） */
#define BACKEND_HOST        "http://10.78.54.172:8080"
#define BACKEND_URL         BACKEND_HOST "/api/record"
#define BACKEND_MEMBERS_URL BACKEND_HOST "/api/members"
#define BACKEND_MEMBERS_VER_URL BACKEND_HOST "/api/members/version"
/* M8 自测：IO10 短接 IO11 做自环；接上 GD32 后改成 0 */
#define LINK_LOOPBACK_TEST  0

/* M9b 联调开关：每 10 秒自动发一次开锁请求，用来验证继电器链路。
 * M11 接通真实触发路径后改成 0 */
#define UNLOCK_SELFTEST     0

/* ---------------- 门禁策略 ---------------- */
#define FACE_STABLE_FRAMES  3      /* 连续检测稳定这么多帧才触发识别 */
#define FACE_SIM_THRESHOLD  0.50f  /* 相似度阈值，M5 用正负样本分布标定 */
#define FAIL_MAX            5      /* 连续失败次数上限 */
#define LOCKOUT_SEC         60     /* 超限锁定秒数 */
#define UNLOCK_SEC          2      /* 继电器吸合时长 */
#define MEMBER_MAX          64     /* 人脸库容量 */
/* 活体检测（动作挑战）：默认关闭
 * 原因：常见门禁不该每次要求转头，日常通行体验太差；
 *       而单目 RGB 摄像头做不了无感（静默）活体。
 * 代码保留，需要时改成 1 即启用（设置页里的策略项也随之出现）。 */
#define LIVENESS_ENABLE     0

#define LIVE_MOVE_PX        40     /* 中心点左右位移阈值（像素） */
#define LIVE_TIMEOUT_MS     8000   /* 8 秒内没转够就算失败 */

#define ENROLL_FRAMES       10     /* 录入采集帧数：3 帧太快，脸还没摆正就采完了 */
#define ENROLL_MIN_FACE_W   80     /* 采集时人脸框最小宽度（竖屏 240 宽空间），太小视为没凑近 */
#define ENROLL_CENTER_TOL   70     /* 框中心离画面中心的最大偏差 */

/* 管理员密码：目前编译期固定，M13 交付前改成 NVS 存储 + 可修改（写进 docs） */
#define ADMIN_PASSWORD      "1234"
