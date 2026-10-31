#!/usr/bin/env bash
# 生成 LVGL 中文子集字库（main/fonts/lv_font_misans_*.c）
#
# 两个坑（2026-10-31 M1 实测）：
#   1) 必须 --no-compress：lv_font_conv 默认输出 bitmap_format=1（压缩），
#      在 LVGL 9.6 上字形表查得到（ok=1、adv/box 正确）但一个像素都不画。
#   2) 字符集是白名单：漏掉的字会显示成方块（库/清/共/稳 就漏过一次），
#      新增界面文案后必须同步扩充 CHARS 再重新生成。
#
# 依赖：node + npx（首次会自动下载 lv_font_conv）
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_REGULAR="/home/dcz/.local/share/fonts/MiSans/MiSans-Regular.ttf"
FONT_BOLD="/home/dcz/.local/share/fonts/MiSans/MiSans-Bold.ttf"

# 16px：正文 / 按钮 / 状态（覆盖全部界面文案，含 M6 的八屏文字）
CHARS='门禁系统人脸库清空已录入共无人失败识别陌生人稳定确认检测屏幕请面向左右转头靠近通行成功开启秒后自动落锁工号活体通过拒绝次连续将定剩余尝试管理员菜单删除成员查看记录阈值与设置容量保持正缓慢采集第帧写入均值过多期间绝一切认证请求联系或稍后再试重启网络连接断等待处理数据同步心跳异常错误警告提示信息姓名版本状态时间数量颜色上下线关启默认重置保存按键松操作原因调限制重语音中欢迎使用验证记录并上报'

# 24px：标题 / 大字（尽量精简，省 Flash）
CHARS24='人脸门禁请面向屏幕识别中通行成功拒绝锁定录入管理员菜单陌生人已开启秒活体通过失败验证次剩余'

npx --yes lv_font_conv@1.5.3 --size 16 --bpp 4 --no-compress --no-prefilter \
    --format lvgl -o main/fonts/lv_font_misans_16.c \
    --font "$FONT_REGULAR" -r 0x20-0x7F --symbols "$CHARS"

npx --yes lv_font_conv@1.5.3 --size 24 --bpp 4 --no-compress --no-prefilter \
    --format lvgl -o main/fonts/lv_font_misans_24.c \
    --font "$FONT_BOLD" -r 0x20-0x7F --symbols "$CHARS24"

echo "done: main/fonts/lv_font_misans_{16,24}.c"
