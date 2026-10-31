#!/usr/bin/env bash
# 生成 LVGL 中文子集字库（main/fonts/lv_font_misans_*.c）
#
# 两个坑（2026-10-31 M1 实测）：
#   1) 必须 --no-compress：lv_font_conv 默认输出 bitmap_format=1（压缩），
#      在 LVGL 9.6 上字形表查得到（ok=1、adv/box 正确）但一个像素都不画。
#   2) 中文字符集按需列出，新增界面文案后要同步改这里的 CHARS 并重新生成。
#
# 依赖：node + npx（首次会自动下载 lv_font_conv）
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_REGULAR="/home/dcz/.local/share/fonts/MiSans/MiSans-Regular.ttf"
FONT_BOLD="/home/dcz/.local/share/fonts/MiSans/MiSans-Bold.ttf"

CHARS='门禁系统语音提示请面向屏幕识别中欢迎使用人脸通行验证正在检测活体通过失败陌生人已记录并上报剩余尝试次连续将锁定秒管理员菜单录入删除成员查看阈值与设置容保持正缓慢左右转头采集帧写入取均值过多期间绝一切认证请求联系或稍后再试工号开自动落度确定取消返回时间数量颜色状态版本号上下线网关闭启默认重置保存确认按键松操作成功原因调限制重等待处理数据同步心跳连接断异常错误警告信息姓名，。：、·（）'

CHARS24='门禁系统通行成功拒绝管理员菜单录入人脸识别中活体通过失败锁定请面向屏幕剩余次秒工号已开自动落'

npx --yes lv_font_conv@1.5.3 --size 16 --bpp 4 --no-compress --no-prefilter \
    --format lvgl -o main/fonts/lv_font_misans_16.c \
    --font "$FONT_REGULAR" -r 0x20-0x7F --symbols "$CHARS"

npx --yes lv_font_conv@1.5.3 --size 24 --bpp 4 --no-compress --no-prefilter \
    --format lvgl -o main/fonts/lv_font_misans_24.c \
    --font "$FONT_BOLD" -r 0x20-0x7F --symbols "$CHARS24"

echo "done: main/fonts/lv_font_misans_{16,24}.c"
