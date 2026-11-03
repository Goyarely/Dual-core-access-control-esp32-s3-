#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "face_engine.h"
#include <stdbool.h>

/*
 * 八界面状态机（总览 doc 的 ①~⑧）
 *
 *   ① 待机      预览 + "请面向屏幕"
 *   ② 识别中    预览 + 四角括号 + "识别中..."
 *   ③ 活体挑战  大字动作提示 + 倒计时条      （M10 接真实判定，M6 只留界面）
 *   ④ 通行成功  整屏绿 + 姓名 + 相似度
 *   ⑤ 通行拒绝  整屏红 + 原因 + 剩余尝试
 *   ⑥ 管理员菜单 长按 2 秒进入
 *   ⑦ 录入进度  进度条 + 帧计数
 *   ⑧ 锁定中    倒计时
 *
 * 分工：main.c 负责"看"（检测/识别），ui 负责"显示"和状态迁移；
 * ui 只通过 ui_take_*_request() 告诉上层"该干活了"，自己不调用模型。
 */

typedef enum {
    UI_STANDBY = 0,
    UI_SCANNING,
    UI_LIVENESS,      /* 活体挑战：请缓慢左右转头（M10） */
    UI_SUCCESS,
    UI_DENIED,
    UI_LOCKOUT,
    UI_AUTH,          /* 管理员密码 */
    UI_MENU,
    UI_MEMBERS,       /* 查看成员 */
    UI_DELETE,        /* 删除成员（点条目即删） */
    UI_SETTINGS,      /* 阈值与锁定设置（只读展示） */
    UI_ENROLLING,
} ui_state_t;

/** 建立界面（需在 LVGL 锁内调用） */
void ui_init(uint8_t *preview_buf, int preview_w, int preview_h);

/** 手动切换界面（内部也在用；长按进菜单、菜单返回都走这里） */
void ui_set_state(ui_state_t st);

/** 刷新成员列表（增删后调用） */
void ui_list_refresh(void);

/** 每帧调用：n 张脸 + 第一张脸的框（无脸传 NULL） */
void ui_frame_feed(int n_face, const face_box_t *box);

/** 识别结果三态：这一帧没人脸 ≠ 陌生人，前者不计失败次数 */
typedef enum {
    UI_RECOG_HIT = 0,     /* 命中 */
    UI_RECOG_STRANGER,    /* 有人脸但库里没有 */
    UI_RECOG_NOFACE,      /* 这一帧没检测到脸（画面抖动/人走开） */
} ui_recog_t;

/** 识别结果回填：title = 大字（如「成员 3」），sub = 副标题（如「编号 03 · 相似度 0.92」） */
void ui_recog_result(ui_recog_t r, const char *title, const char *sub, float sim);

/** 密码开锁请求：触摸屏上密码校验通过后由 main 取走并发给 GD32 */
bool ui_take_pw_unlock_request(void);

/** 开锁结果回填：更新成功页底部提示（门已开 / 被拒绝 / 锁定中） */
void ui_unlock_result(int result);

/** 录入结果回填 */
void ui_enroll_result(bool ok, int total);

/** 录入结果 + 自定义文案（查重命中时用"已录入过"） */
void ui_enroll_msg(bool ok, const char *msg);

/** 1 秒心跳：锁定倒计时、成功/拒绝页自动回落 */
void ui_tick_1s(void);

/** 当前状态 */
ui_state_t ui_state(void);

/** 上层要不要现在跑一次识别？取走后自动清零 */
bool ui_take_recognize_request(void);

/** 上层要不要现在跑一次录入？取走后自动清零 */
bool ui_take_enroll_request(void);
