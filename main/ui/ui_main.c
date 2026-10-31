#include "ui_main.h"
#include "app_config.h"
#include "app_settings.h"
#include "lv_fonts.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

#define H_RES   320
#define V_RES   480

#define PREVIEW_X  40
#define PREVIEW_Y  40

#define BRACKET_ARM   26
#define BRACKET_THICK 4

#define HOLD_MENU_MS   1500   /* 长按进入管理员验证 */
#define MENU_IDLE_SEC  20

#define LIST_ROWS  8

/* ---------------- 对象 ---------------- */
static lv_obj_t *s_scr;
static lv_obj_t *s_img;
static lv_image_dsc_t s_dsc;
static lv_obj_t *s_brk[8];
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_right;
static lv_obj_t *s_lbl_hint;

static lv_obj_t *s_ov_ok;
static lv_obj_t *s_lbl_ok_name;
static lv_obj_t *s_lbl_ok_sub;
static lv_obj_t *s_lbl_ok_time;

static lv_obj_t *s_ov_no;
static lv_obj_t *s_lbl_no_reason;
static lv_obj_t *s_lbl_no_sub;

static lv_obj_t *s_ov_lock;
static lv_obj_t *s_lbl_lock_cnt;

static lv_obj_t *s_ov_auth;
static lv_obj_t *s_lbl_pw;

static lv_obj_t *s_ov_menu;

static lv_obj_t *s_ov_list;          /* 查看成员 / 删除成员 共用 */
static lv_obj_t *s_lbl_list_title;
static lv_obj_t *s_lbl_list_sub;
static lv_obj_t *s_row[LIST_ROWS];
static lv_obj_t *s_row_lbl[LIST_ROWS];

static lv_obj_t *s_ov_set;
static lv_obj_t *s_lbl_thr;
static lv_obj_t *s_lbl_fail;
static lv_obj_t *s_lbl_lock;
static lv_obj_t *s_ov_enroll;
static lv_obj_t *s_lbl_enroll_tip;
static lv_obj_t *s_bar_enroll;

/* ---------------- 状态 ---------------- */
static ui_state_t s_state = UI_STANDBY;
static uint32_t s_state_tick;
static int s_fail_cnt;
static int s_lock_left;
static int s_stable;
static bool s_recog_req;
static bool s_enroll_req;
static bool s_must_leave;
static int s_enroll_frames;
static bool s_enroll_started;   /* 点了「开始」才开始采集 */
static lv_obj_t *s_btn_enroll_start;

/* 结果页停留：先显示结果，过 ms 之后再跳到 target（否则一改文字就被下一屏盖掉） */
static bool s_hold_active;
static ui_state_t s_hold_target;
static uint32_t s_hold_tick;
static uint32_t s_hold_ms;

static void ui_hold(ui_state_t target, uint32_t ms)
{
    s_hold_active = true;
    s_hold_target = target;
    s_hold_tick = lv_tick_get();
    s_hold_ms = ms;
}

static char s_pw[8];
static int s_pw_len;
static bool s_list_delete_mode;
static uint16_t s_list_ids[MEMBER_MAX];   /* 第 i 行对应的真实 id */
static int s_list_cnt;

/* ---------------- 小工具 ---------------- */
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *make_overlay(uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(s_scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, H_RES, V_RES);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t *make_bar(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BRACKET_ARM, BRACKET_THICK);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4ce08a), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, int x, int y, int w, int h,
                          lv_event_cb_t cb, void *user, uint32_t bg, uint32_t fg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *l = make_label(b, txt, &lv_font_misans_16, fg);
    lv_obj_center(l);
    return b;
}

/* ---------------- 长按进管理员验证 ---------------- */
static uint32_t s_press_tick;

static void scr_press_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        s_press_tick = lv_tick_get();
    } else if (c == LV_EVENT_RELEASED) {
        if (s_state == UI_STANDBY && lv_tick_elaps(s_press_tick) > HOLD_MENU_MS) {
            s_pw_len = 0;
            s_pw[0] = '\0';
            ui_set_state(UI_AUTH);
        }
    }
}

/* ---------------- 密码页 ---------------- */
static void pw_refresh(void)
{
    char buf[16] = {0};
    for (int i = 0; i < s_pw_len; i++) buf[i] = '*';
    if (s_pw_len == 0) snprintf(buf, sizeof(buf), "输入密码");
    lv_label_set_text(s_lbl_pw, buf);
}

static void key_cb(lv_event_t *e)
{
    char k = (char)(intptr_t)lv_event_get_user_data(e);

    if (k == 'C') {
        s_pw_len = 0;
        s_pw[0] = '\0';
    } else if (k == 'O') {
        if (strcmp(s_pw, ADMIN_PASSWORD) == 0) {
            s_pw_len = 0;
            s_pw[0] = '\0';
            ui_set_state(UI_MENU);
            return;
        }
        s_pw_len = 0;
        s_pw[0] = '\0';
        lv_label_set_text(s_lbl_pw, "密码错误");
        lv_obj_set_style_text_color(s_lbl_pw, lv_color_hex(0xff8a80), 0);
        return;
    } else if (k >= '0' && k <= '9') {
        if (s_pw_len < 4) {
            s_pw[s_pw_len++] = k;
            s_pw[s_pw_len] = '\0';
        }
    }
    lv_obj_set_style_text_color(s_lbl_pw, lv_color_white(), 0);
    pw_refresh();
}

/* ---------------- 成员列表（查看 / 删除共用） ---------------- */
static void row_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_list_delete_mode || idx < 0 || idx >= s_list_cnt) return;

    uint16_t id = s_list_ids[idx];
    esp_err_t r = face_delete_member(id);
    char buf[48];
    if (r == ESP_OK) {
        snprintf(buf, sizeof(buf), "已删除 成员 %d", idx + 1);
    } else {
        snprintf(buf, sizeof(buf), "删除失败 成员 %d", idx + 1);
    }
    ESP_LOGW(TAG, "%s", buf);
    lv_label_set_text(s_lbl_list_title, buf);
    ui_list_refresh();
}

void ui_list_refresh(void)
{
    s_list_cnt = face_members(s_list_ids, MEMBER_MAX);

    char sub[48];
    snprintf(sub, sizeof(sub), "共 %d 人", face_recog_count());
    lv_label_set_text(s_lbl_list_sub, sub);

    for (int i = 0; i < LIST_ROWS; i++) {
        if (i < s_list_cnt) {
            lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            /* 显示「第几个」而不是库内 id：id 只增不减，删完人重录会是 17、18，
             * 对使用者没意义；删除时再用 s_list_ids[i] 找真实 id */
            char txt[32];
            snprintf(txt, sizeof(txt), "成员 %d", i + 1);
            lv_label_set_text(s_row_lbl[i], txt);
            lv_obj_remove_event_cb(s_row[i], row_cb);
            lv_obj_add_event_cb(s_row[i], row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        } else {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_list_cnt == 0) {
        lv_label_set_text(s_lbl_list_sub, "库为空");
    }
}

/* ---------------- 菜单 / 设置 ---------------- */
static void menu_back_cb(lv_event_t *e)
{
    (void)e;
    ui_set_state(UI_STANDBY);
}

static void menu_enroll_cb(lv_event_t *e)
{
    (void)e;
    s_enroll_frames = 0;
    s_enroll_started = false;      /* 先让管理员对着画面调姿势 */
    ui_set_state(UI_ENROLLING);
}

static void enroll_start_cb(lv_event_t *e)
{
    (void)e;
    s_enroll_started = true;
    s_enroll_frames = 0;
    lv_bar_set_value(s_bar_enroll, 0, LV_ANIM_OFF);
    lv_label_set_text(s_lbl_enroll_tip, "请正对屏幕  0/10");
    lv_obj_set_style_text_color(s_lbl_enroll_tip, lv_color_white(), 0);
    lv_obj_add_flag(s_btn_enroll_start, LV_OBJ_FLAG_HIDDEN);
}

static void settings_refresh(void)
{
    app_settings_t *c = app_settings();
    char b[40];
    snprintf(b, sizeof(b), "相似度阈值 %.2f", c->sim_thr);
    lv_label_set_text(s_lbl_thr, b);
    snprintf(b, sizeof(b), "失败上限  %d 次", c->fail_max);
    lv_label_set_text(s_lbl_fail, b);
    snprintf(b, sizeof(b), "锁定时间  %d 秒", c->lockout_sec);
    lv_label_set_text(s_lbl_lock, b);
}

/* user_data = (字段号 << 1) | 方向(1=加,0=减) */
static void set_adj_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int field = (int)(v >> 1);
    int dir = (v & 1) ? 1 : -1;
    app_settings_t *c = app_settings();

    if (field == 0) {
        c->sim_thr += dir * 0.05f;
        if (c->sim_thr < 0.40f) c->sim_thr = 0.40f;
        if (c->sim_thr > 0.90f) c->sim_thr = 0.90f;
    } else if (field == 1) {
        c->fail_max += dir;
        if (c->fail_max < 1) c->fail_max = 1;
        if (c->fail_max > 10) c->fail_max = 10;
    } else {
        c->lockout_sec += dir * 10;
        if (c->lockout_sec < 10) c->lockout_sec = 10;
        if (c->lockout_sec > 300) c->lockout_sec = 300;
    }
    settings_refresh();
}

static void settings_save_cb(lv_event_t *e)
{
    (void)e;
    app_settings_save();
    lv_label_set_text(s_lbl_thr, "已保存");
    settings_refresh();
}

static void list_back_cb(lv_event_t *e)
{
    (void)e;
    ui_set_state(UI_MENU);      /* 列表 → 菜单，不是直接回待机 */
}

static void menu_members_cb(lv_event_t *e)
{
    (void)e;
    s_list_delete_mode = false;
    lv_label_set_text(s_lbl_list_title, "查看成员");
    ui_list_refresh();
    ui_set_state(UI_MEMBERS);
}

static void menu_delete_cb(lv_event_t *e)
{
    (void)e;
    s_list_delete_mode = true;
    lv_label_set_text(s_lbl_list_title, "点成员即删除");
    ui_list_refresh();
    ui_set_state(UI_DELETE);
}

static void menu_settings_cb(lv_event_t *e)
{
    (void)e;
    ui_set_state(UI_SETTINGS);
}

/* ---------------- 界面搭建 ---------------- */
void ui_init(uint8_t *preview_buf, int preview_w, int preview_h)
{
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0d131b), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, scr_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, scr_press_cb, LV_EVENT_RELEASED, NULL);

    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = preview_w;
    s_dsc.header.h = preview_h;
    s_dsc.header.stride = preview_w * 2;
    s_dsc.data_size = preview_w * preview_h * 2;
    s_dsc.data = preview_buf;
    s_img = lv_image_create(s_scr);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_set_pos(s_img, PREVIEW_X, PREVIEW_Y);

    for (int i = 0; i < 8; i++) {
        s_brk[i] = make_bar(s_scr);
        lv_obj_set_size(s_brk[i], (i % 2 == 0) ? BRACKET_ARM : BRACKET_THICK,
                        (i % 2 == 0) ? BRACKET_THICK : BRACKET_ARM);
    }

    s_lbl_title = make_label(s_scr, "人脸门禁", &lv_font_misans_16, 0x3ddc78);
    lv_obj_set_pos(s_lbl_title, 8, 10);

    s_lbl_right = make_label(s_scr, "", &lv_font_misans_16, 0xffd166);
    lv_obj_align(s_lbl_right, LV_ALIGN_TOP_RIGHT, -8, 10);

    s_lbl_hint = make_label(s_scr, "请面向屏幕", &lv_font_misans_16, 0x7d8ea0);
    lv_obj_set_pos(s_lbl_hint, 8, PREVIEW_Y + preview_h + 14);

    /* ④ 通行成功（照设计稿：顶部时间+开锁中标签 / 勾 / 大字 / 副标 / 底部提示） */
    s_ov_ok = make_overlay(0x1f7a33);

    s_lbl_ok_time = make_label(s_ov_ok, "00:00", &lv_font_misans_16, 0xe8f5ec);
    lv_obj_set_pos(s_lbl_ok_time, 44, 12);

    lv_obj_t *chip = lv_obj_create(s_ov_ok);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 92, 30);
    lv_obj_set_pos(chip, 116, 6);
    lv_obj_set_style_radius(chip, 15, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(0xbfe6cd), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_t *chip_lbl = make_label(chip, "开锁中", &lv_font_misans_16, 0xffffff);
    lv_obj_center(chip_lbl);

    lv_obj_t *cir = lv_obj_create(s_ov_ok);
    lv_obj_remove_style_all(cir);
    lv_obj_set_size(cir, 96, 96);
    lv_obj_set_pos(cir, 112, 108);
    lv_obj_set_style_radius(cir, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cir, lv_color_hex(0x35a052), 0);
    lv_obj_set_style_bg_opa(cir, LV_OPA_COVER, 0);

    static lv_point_precise_t ck[3] = {{140, 156}, {155, 171}, {182, 141}};
    lv_obj_t *line = lv_line_create(s_ov_ok);
    lv_line_set_points(line, ck, 3);
    lv_obj_set_style_line_color(line, lv_color_white(), 0);
    lv_obj_set_style_line_width(line, 7, 0);
    lv_obj_set_style_line_rounded(line, true, 0);

    s_lbl_ok_name = make_label(s_ov_ok, "", &lv_font_misans_24, 0xffffff);
    lv_obj_align(s_lbl_ok_name, LV_ALIGN_TOP_MID, 0, 236);

    s_lbl_ok_sub = make_label(s_ov_ok, "", &lv_font_misans_16, 0xc9ecd1);
    lv_obj_align(s_lbl_ok_sub, LV_ALIGN_TOP_MID, 0, 278);

    lv_obj_t *foot = lv_obj_create(s_ov_ok);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, H_RES, 46);
    lv_obj_set_pos(foot, 0, V_RES - 46);
    lv_obj_set_style_bg_color(foot, lv_color_hex(0x17612a), 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_t *foot_lbl = make_label(foot, "门已开 · 2s 后自动落锁", &lv_font_misans_16, 0xd8f3e1);
    lv_obj_center(foot_lbl);

    /* ⑤ 通行拒绝 */
    s_ov_no = make_overlay(0xa72b22);
    lv_obj_t *no_title = make_label(s_ov_no, "拒绝通行", &lv_font_misans_24, 0xffffff);
    lv_obj_align(no_title, LV_ALIGN_CENTER, 0, -60);
    s_lbl_no_reason = make_label(s_ov_no, "陌生人", &lv_font_misans_16, 0xffffff);
    lv_obj_align(s_lbl_no_reason, LV_ALIGN_CENTER, 0, 0);
    s_lbl_no_sub = make_label(s_ov_no, "", &lv_font_misans_16, 0xffd7d4);
    lv_obj_align(s_lbl_no_sub, LV_ALIGN_CENTER, 0, 50);

    /* ⑧ 锁定 */
    s_ov_lock = make_overlay(0x8a5a00);
    lv_obj_t *lk_title = make_label(s_ov_lock, "尝试次数过多", &lv_font_misans_24, 0xffffff);
    lv_obj_align(lk_title, LV_ALIGN_CENTER, 0, -80);
    s_lbl_lock_cnt = make_label(s_ov_lock, "60s", &lv_font_misans_24, 0xffffff);
    lv_obj_align(s_lbl_lock_cnt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *lk_sub = make_label(s_ov_lock, "期间拒绝一切认证请求", &lv_font_misans_16, 0xffe6b3);
    lv_obj_align(lk_sub, LV_ALIGN_CENTER, 0, 70);

    /* 管理员密码 */
    s_ov_auth = make_overlay(0x0d131b);
    lv_obj_t *at = make_label(s_ov_auth, "管理员验证", &lv_font_misans_24, 0xffffff);
    lv_obj_align(at, LV_ALIGN_TOP_MID, 0, 40);
    s_lbl_pw = make_label(s_ov_auth, "输入密码", &lv_font_misans_24, 0xffffff);
    lv_obj_align(s_lbl_pw, LV_ALIGN_TOP_MID, 0, 100);

    const char *keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "清除", "0", "确认"};
    char kc[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'C', '0', 'O'};
    for (int i = 0; i < 12; i++) {
        int x = 40 + (i % 3) * 84;
        int y = 170 + (i / 3) * 62;
        uint32_t bg = (i == 11) ? 0x2e7d32 : ((i == 9) ? 0x8a5a00 : 0x185fa5);
        make_btn(s_ov_auth, keys[i], x, y, 76, 50, key_cb, (void *)(intptr_t)kc[i], bg, 0xffffff);
    }

    /* ⑥ 管理员菜单 */
    s_ov_menu = make_overlay(0xf2f4f7);
    lv_obj_t *mt = make_label(s_ov_menu, "管理员菜单", &lv_font_misans_24, 0x185fa5);
    lv_obj_align(mt, LV_ALIGN_TOP_LEFT, 20, 20);
    make_btn(s_ov_menu, "录入人脸", 20, 80, 280, 48, menu_enroll_cb, NULL, 0xffffff, 0x222b38);
    make_btn(s_ov_menu, "查看成员", 20, 140, 280, 48, menu_members_cb, NULL, 0xffffff, 0x222b38);
    make_btn(s_ov_menu, "删除成员", 20, 200, 280, 48, menu_delete_cb, NULL, 0xffffff, 0x222b38);
    make_btn(s_ov_menu, "阈值与锁定设置", 20, 260, 280, 48, menu_settings_cb, NULL, 0xffffff, 0x222b38);
    make_btn(s_ov_menu, "返回", 20, 380, 280, 48, menu_back_cb, NULL, 0xdfe5ec, 0x222b38);

    /* 查看/删除成员列表 */
    s_ov_list = make_overlay(0xf2f4f7);
    s_lbl_list_title = make_label(s_ov_list, "查看成员", &lv_font_misans_24, 0x185fa5);
    lv_obj_align(s_lbl_list_title, LV_ALIGN_TOP_LEFT, 20, 16);
    s_lbl_list_sub = make_label(s_ov_list, "", &lv_font_misans_16, 0x5c6675);
    lv_obj_align(s_lbl_list_sub, LV_ALIGN_TOP_RIGHT, -20, 26);

    for (int i = 0; i < LIST_ROWS; i++) {
        s_row[i] = make_btn(s_ov_list, "成员", 20, 62 + i * 40, 280, 36, row_cb, NULL,
                            0xffffff, 0x222b38);
        s_row_lbl[i] = lv_obj_get_child(s_row[i], 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    make_btn(s_ov_list, "返回", 20, 400, 280, 44, list_back_cb, NULL, 0xdfe5ec, 0x222b38);

    /* 阈值与锁定设置（可调 + 保存到 /storage/settings.bin） */
    s_ov_set = make_overlay(0xf2f4f7);
    lv_obj_t *st = make_label(s_ov_set, "阈值与锁定设置", &lv_font_misans_24, 0x185fa5);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 16, 14);

    s_lbl_thr = make_label(s_ov_set, "", &lv_font_misans_16, 0x222b38);
    lv_obj_set_pos(s_lbl_thr, 16, 96);
    s_lbl_fail = make_label(s_ov_set, "", &lv_font_misans_16, 0x222b38);
    lv_obj_set_pos(s_lbl_fail, 16, 156);
    s_lbl_lock = make_label(s_ov_set, "", &lv_font_misans_16, 0x222b38);
    lv_obj_set_pos(s_lbl_lock, 16, 216);

    make_btn(s_ov_set, "-", 196, 90, 48, 40, set_adj_cb, (void *)(intptr_t)((0 << 1) | 0), 0xdfe5ec, 0x222b38);
    make_btn(s_ov_set, "+", 254, 90, 48, 40, set_adj_cb, (void *)(intptr_t)((0 << 1) | 1), 0xdfe5ec, 0x222b38);
    make_btn(s_ov_set, "-", 196, 150, 48, 40, set_adj_cb, (void *)(intptr_t)((1 << 1) | 0), 0xdfe5ec, 0x222b38);
    make_btn(s_ov_set, "+", 254, 150, 48, 40, set_adj_cb, (void *)(intptr_t)((1 << 1) | 1), 0xdfe5ec, 0x222b38);
    make_btn(s_ov_set, "-", 196, 210, 48, 40, set_adj_cb, (void *)(intptr_t)((2 << 1) | 0), 0xdfe5ec, 0x222b38);
    make_btn(s_ov_set, "+", 254, 210, 48, 40, set_adj_cb, (void *)(intptr_t)((2 << 1) | 1), 0xdfe5ec, 0x222b38);

    make_btn(s_ov_set, "保存", 20, 290, 280, 48, settings_save_cb, NULL, 0x2e7d32, 0xffffff);
    make_btn(s_ov_set, "返回", 20, 400, 280, 44, list_back_cb, NULL, 0xdfe5ec, 0x222b38);

    /* ⑦ 录入进度：覆盖层不能挡住预览，只留底部一条半透明条 */
    s_ov_enroll = make_overlay(0x0d131b);
    lv_obj_set_style_bg_opa(s_ov_enroll, LV_OPA_TRANSP, 0);      /* 全屏透明 */
    lv_obj_remove_flag(s_ov_enroll, LV_OBJ_FLAG_CLICKABLE);      /* 不吞触摸 */

    lv_obj_t *strip = lv_obj_create(s_ov_enroll);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, H_RES, V_RES - 360);
    lv_obj_set_pos(strip, 0, 360);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0x0d131b), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_80, 0);

    s_lbl_enroll_tip = make_label(s_ov_enroll, "请正对屏幕", &lv_font_misans_16, 0xffffff);
    lv_obj_set_pos(s_lbl_enroll_tip, 12, 364);

    s_bar_enroll = lv_bar_create(s_ov_enroll);
    lv_obj_set_size(s_bar_enroll, 296, 12);
    lv_obj_set_pos(s_bar_enroll, 12, 390);
    lv_bar_set_range(s_bar_enroll, 0, ENROLL_FRAMES);
    lv_bar_set_value(s_bar_enroll, 0, LV_ANIM_OFF);

    s_btn_enroll_start = make_btn(s_ov_enroll, "开始", 12, 414, 140, 46,
                                  enroll_start_cb, NULL, 0x2e7d32, 0xffffff);
    make_btn(s_ov_enroll, "取消", 168, 414, 140, 46, list_back_cb, NULL, 0x8a5a00, 0xffffff);

    ESP_LOGI(TAG, "ui ready (8 screens + auth/members/settings)");
}

/* ---------------- 状态切换 ---------------- */
void ui_set_state(ui_state_t st)
{
    s_state = st;
    s_state_tick = lv_tick_get();
    if (s_hold_active && st != s_hold_target) s_hold_active = false;

    lv_obj_add_flag(s_ov_ok, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ov_no, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ov_lock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ov_auth, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ov_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ov_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ov_set, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ov_enroll, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_right, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 8; i++) lv_obj_add_flag(s_brk[i], LV_OBJ_FLAG_HIDDEN);

    switch (st) {
    case UI_STANDBY:
        lv_label_set_text(s_lbl_hint, "请面向屏幕");
        lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0x7d8ea0), 0);
        lv_label_set_text(s_lbl_right, "");
        break;

    case UI_SCANNING:
        lv_label_set_text(s_lbl_hint, "识别中...");
        lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0xffd166), 0);
        break;

    case UI_SUCCESS:
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ov_ok, LV_OBJ_FLAG_HIDDEN);
        break;

    case UI_DENIED:
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ov_no, LV_OBJ_FLAG_HIDDEN);
        break;

    case UI_LOCKOUT:
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ov_lock, LV_OBJ_FLAG_HIDDEN);
        break;

    case UI_AUTH:
    case UI_MENU:
    case UI_MEMBERS:
    case UI_DELETE:
    case UI_SETTINGS:
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_right, LV_OBJ_FLAG_HIDDEN);
        if (st == UI_AUTH) {
            lv_obj_remove_flag(s_ov_auth, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_lbl_pw, "输入密码");
            lv_obj_set_style_text_color(s_lbl_pw, lv_color_white(), 0);
        } else if (st == UI_MENU) {
            lv_obj_remove_flag(s_ov_menu, LV_OBJ_FLAG_HIDDEN);
        } else if (st == UI_MEMBERS || st == UI_DELETE) {
            lv_obj_remove_flag(s_ov_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ov_set, LV_OBJ_FLAG_HIDDEN);
            settings_refresh();
        }
        break;

    case UI_ENROLLING:
        /* 预览图必须留着：管理员要看着调姿势，否则不知道有没有对正 */
        lv_obj_remove_flag(s_ov_enroll, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar_enroll, 0, LV_ANIM_OFF);
        lv_label_set_text(s_lbl_enroll_tip, "请正对屏幕");
        lv_obj_set_style_text_color(s_lbl_enroll_tip, lv_color_white(), 0);
        /* 还没点「开始」就把按钮显示出来 */
        if (s_enroll_started) lv_obj_add_flag(s_btn_enroll_start, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_btn_enroll_start, LV_OBJ_FLAG_HIDDEN);
        break;

    default:
        break;
    }
}

/* ---------------- 对上层接口 ---------------- */
ui_state_t ui_state(void)
{
    return s_state;
}

bool ui_take_recognize_request(void)
{
    bool r = s_recog_req;
    s_recog_req = false;
    return r;
}

bool ui_take_enroll_request(void)
{
    bool r = s_enroll_req;
    s_enroll_req = false;
    return r;
}

static void show_bracket(const face_box_t *b)
{
    int px1 = b->x1, py1 = b->y1, px2 = b->x2, py2 = b->y2;
    if (px1 < 0) px1 = 0;
    if (py1 < 0) py1 = 0;
    if (px2 > s_dsc.header.w - 1) px2 = s_dsc.header.w - 1;
    if (py2 > s_dsc.header.h - 1) py2 = s_dsc.header.h - 1;
    if (px2 - px1 < 8 || py2 - py1 < 8) return;

    const int x1 = PREVIEW_X + px1, y1 = PREVIEW_Y + py1;
    const int x2 = PREVIEW_X + px2, y2 = PREVIEW_Y + py2;

    struct { lv_obj_t *o; int x, y; } pos[8] = {
        {s_brk[0], x1, y1},
        {s_brk[1], x1, y1},
        {s_brk[2], x2 - BRACKET_ARM, y1},
        {s_brk[3], x2 - BRACKET_THICK, y1},
        {s_brk[4], x1, y2 - BRACKET_THICK},
        {s_brk[5], x1, y2 - BRACKET_ARM},
        {s_brk[6], x2 - BRACKET_ARM, y2 - BRACKET_THICK},
        {s_brk[7], x2 - BRACKET_THICK, y2 - BRACKET_ARM},
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_set_pos(pos[i].o, pos[i].x, pos[i].y);
        lv_obj_remove_flag(pos[i].o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_bracket(void)
{
    for (int i = 0; i < 8; i++) lv_obj_add_flag(s_brk[i], LV_OBJ_FLAG_HIDDEN);
}

void ui_frame_feed(int n_face, const face_box_t *box)
{
    /* 预览隔帧刷新：整张 240x320 走 SPI 要 120ms 左右，
     * 每帧都刷会把 LVGL 任务占死，触摸就"不灵敏"了 */
    static uint32_t s_frame_no = 0;
    if (((s_frame_no++ & 1) == 0) && (s_state == UI_STANDBY || s_state == UI_SCANNING ||
                                      s_state == UI_ENROLLING)) {
        lv_obj_invalidate(s_img);
    }

    if (s_state == UI_ENROLLING) {
        char buf[32];

        if (!s_enroll_started) return;   /* 只刷新预览，等管理员点「开始」 */

        /* 采集超时：30 秒还没采满就判失败，别让管理员干等 */
        if (lv_tick_elaps(s_state_tick) > 30000 && s_enroll_frames < ENROLL_FRAMES) {
            lv_label_set_text(s_lbl_enroll_tip, "录入失败 超时");
            lv_obj_set_style_text_color(s_lbl_enroll_tip, lv_color_hex(0xff8a80), 0);
            ui_hold(UI_MENU, 2000);
            ESP_LOGW(TAG, "enroll timeout, frames=%d", s_enroll_frames);
            return;
        }

        if (n_face > 0 && box != NULL) {
            int fw = box->x2 - box->x1;
            int cx = (box->x1 + box->x2) / 2;
            /* 只有"够大 + 大致居中"的帧才计入，避免歪着/太远也算数 */
            bool good = (fw >= ENROLL_MIN_FACE_W) &&
                        (abs(cx - s_dsc.header.w / 2) <= ENROLL_CENTER_TOL);
            if (good) {
                s_enroll_frames++;
                if (s_enroll_frames > ENROLL_FRAMES) s_enroll_frames = ENROLL_FRAMES;
                lv_bar_set_value(s_bar_enroll, s_enroll_frames, LV_ANIM_OFF);
                snprintf(buf, sizeof(buf), "采集 %d/%d", s_enroll_frames, ENROLL_FRAMES);
                lv_label_set_text(s_lbl_enroll_tip, buf);
                lv_obj_set_style_text_color(s_lbl_enroll_tip, lv_color_white(), 0);
                if (s_enroll_frames >= ENROLL_FRAMES) s_enroll_req = true;
            } else {
                snprintf(buf, sizeof(buf), "请正对屏幕  %d/%d", s_enroll_frames, ENROLL_FRAMES);
                lv_label_set_text(s_lbl_enroll_tip, buf);
                lv_obj_set_style_text_color(s_lbl_enroll_tip, lv_color_hex(0xffd166), 0);
            }
        } else {
            snprintf(buf, sizeof(buf), "请面向屏幕  %d/%d", s_enroll_frames, ENROLL_FRAMES);
            lv_label_set_text(s_lbl_enroll_tip, buf);
            lv_obj_set_style_text_color(s_lbl_enroll_tip, lv_color_hex(0x7d8ea0), 0);
        }
        return;
    }

    if (s_state != UI_STANDBY && s_state != UI_SCANNING) {
        hide_bracket();
        return;
    }

    if (n_face > 0 && box != NULL) {
        if (s_stable < FACE_STABLE_FRAMES) s_stable++;
        show_bracket(box);
    } else {
        s_stable = 0;
        s_must_leave = false;
        hide_bracket();
    }

    if (s_state == UI_STANDBY && s_stable >= FACE_STABLE_FRAMES && !s_must_leave) {
        ui_set_state(UI_SCANNING);
        s_recog_req = true;
    } else if (s_state == UI_SCANNING && s_stable == 0) {
        ui_set_state(UI_STANDBY);
    }
}

void ui_recog_result(ui_recog_t r, const char *title, const char *sub, float sim)
{
    if (s_state != UI_SCANNING) return;

    if (r == UI_RECOG_NOFACE) {
        s_stable = 0;
        ui_set_state(UI_STANDBY);
        return;
    }

    s_must_leave = true;

    if (r == UI_RECOG_HIT) {
        /* 时间：暂时用开机时长，接 WiFi/NTP 之后换成真实时间 */
        uint32_t min = (uint32_t)(lv_tick_get() / 60000);
        char tm[16];
        snprintf(tm, sizeof(tm), "%02u:%02u", (unsigned)((min / 60) % 24), (unsigned)(min % 60));
        lv_label_set_text(s_lbl_ok_time, tm);
        lv_label_set_text(s_lbl_ok_name, title ? title : "已登记");
        lv_label_set_text(s_lbl_ok_sub, sub ? sub : "");
        s_fail_cnt = 0;
        ui_set_state(UI_SUCCESS);
        ESP_LOGI(TAG, "SUCCESS %s (%.2f)", title ? title : "-", sim);
    } else {
        s_fail_cnt++;
        if (s_fail_cnt >= app_settings()->fail_max) {
            s_lock_left = app_settings()->lockout_sec;
            char buf[16];
            snprintf(buf, sizeof(buf), "%ds", s_lock_left);
            lv_label_set_text(s_lbl_lock_cnt, buf);
            ui_set_state(UI_LOCKOUT);
            ESP_LOGW(TAG, "LOCKOUT %ds", s_lock_left);
        } else {
            char sub[48];
            snprintf(sub, sizeof(sub), "剩余尝试 %d 次", app_settings()->fail_max - s_fail_cnt);
            lv_label_set_text(s_lbl_no_reason, "陌生人");
            lv_label_set_text(s_lbl_no_sub, sub);
            ui_set_state(UI_DENIED);
            ESP_LOGW(TAG, "DENIED fail=%d", s_fail_cnt);
        }
    }
}

void ui_enroll_msg(bool ok, const char *msg)
{
    if (s_state != UI_ENROLLING) return;

    lv_label_set_text(s_lbl_enroll_tip, msg ? msg : (ok ? "录入成功" : "录入失败"));
    lv_obj_set_style_text_color(s_lbl_enroll_tip,
                                lv_color_hex(ok ? 0x4ce08a : 0xff8a80), 0);
    s_enroll_frames = 0;
    ui_hold(UI_MENU, 2000);     /* 结果停留 2 秒再回菜单 */
}

void ui_enroll_result(bool ok, int total)
{
    char buf[40];
    if (ok) snprintf(buf, sizeof(buf), "录入成功 共%d人", total);
    else snprintf(buf, sizeof(buf), "录入失败 无人脸");
    ui_enroll_msg(ok, buf);
}

void ui_tick_1s(void)
{
    if (s_hold_active && lv_tick_elaps(s_hold_tick) >= s_hold_ms) {
        s_hold_active = false;
        ui_set_state(s_hold_target);
        return;
    }
    uint32_t el = lv_tick_elaps(s_state_tick) / 1000;

    switch (s_state) {
    case UI_SUCCESS:
    case UI_DENIED:
        if (el >= 3) ui_set_state(UI_STANDBY);
        break;

    case UI_LOCKOUT:
        if (s_lock_left > 0) {
            s_lock_left--;
            char buf[16];
            snprintf(buf, sizeof(buf), "%ds", s_lock_left);
            lv_label_set_text(s_lbl_lock_cnt, buf);
        }
        if (s_lock_left == 0) {
            s_fail_cnt = 0;
            ui_set_state(UI_STANDBY);
        }
        break;

    case UI_MENU:
    case UI_MEMBERS:
    case UI_DELETE:
    case UI_SETTINGS:
    case UI_AUTH:
        if (el >= MENU_IDLE_SEC) ui_set_state(UI_STANDBY);
        break;

    default:
        break;
    }
}
