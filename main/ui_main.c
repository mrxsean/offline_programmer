#include "ui_main.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "drv_st7789v.h"
#include "drv_touch_key.h"
#include "prog_job.h"
#include "storage.h"

static const char *TAG = "ui";

#define C_BG      0x0E141B
#define C_BAR     0x141D27
#define C_CARD    0x1B2634
#define C_TEXT    0xDCE4EC
#define C_MUTED   0x7D8A99
#define C_ACCENT  0x2E7CF6
#define C_OK      0x2ECC71
#define C_ERR     0xE74C3C
#define C_WARN    0xF1C40F

#define UI_FONT   (&lv_font_simsun_16_cjk)

#define PAGE_STATUS 0
#define PAGE_FILES  1
#define PAGE_PROG   2
#define PAGE_COUNT  3

#define FILE_VISIBLE 7
#define FILE_MAX     20
#define PROG_ROWS    4   /* 擦除/编程/校验/开始 */

static const char *s_page_names[PAGE_COUNT] = { "状态", "文件", "烧录" };

static struct {
    int page;
    lv_obj_t *pages[PAGE_COUNT];
    lv_obj_t *title_label;
    lv_obj_t *wifi_label;
    lv_obj_t *hint_label;
    lv_obj_t *toast;
    lv_timer_t *toast_timer;

    /* status page */
    lv_obj_t *chip_label;
    lv_obj_t *chip_detail;
    lv_obj_t *net_label;
    lv_obj_t *stor_label;
    lv_obj_t *job_label;
    lv_obj_t *connect_row;
    lv_obj_t *connect_text;

    /* files page */
    lv_obj_t *file_rows[FILE_VISIBLE];
    lv_obj_t *file_empty;
    char files[FILE_MAX][STORAGE_MAX_NAME];
    size_t file_sizes[FILE_MAX];
    int file_count;
    int file_cursor;
    int file_top;
    int file_selected;      /* -1 = none */
    bool del_armed;
    uint32_t del_deadline;

    /* prog page */
    lv_obj_t *sel_file_label;
    lv_obj_t *opt_rows[3];          /* 擦除/编程/校验 */
    lv_obj_t *start_row;
    lv_obj_t *start_text;
    lv_obj_t *bar;
    lv_obj_t *stage_label;
    lv_obj_t *log_label;
    int prog_cursor;
    bool opt_erase;
    bool opt_program;
    bool opt_verify;
    char selected_file[STORAGE_MAX_NAME];

    bool backlight_on;
} s = {
    .file_selected = -1,
    .opt_erase = true,
    .opt_program = true,
    .opt_verify = true,
    .backlight_on = true,
};

/* ---------------- style helpers ---------------- */

static void style_common(lv_obj_t *obj, uint32_t bg, uint32_t text)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(text), 0);
    lv_obj_set_style_text_font(obj, UI_FONT, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    return c;
}

static lv_obj_t *make_row(lv_obj_t *parent, int y, int w, int h)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 6, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 4, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(r, 0, y);
    lv_obj_set_size(r, w, h);
    return r;
}

static void row_set_cursor(lv_obj_t *row, bool on)
{
    lv_obj_set_style_bg_color(row, lv_color_hex(on ? C_ACCENT : C_CARD), 0);
}

/* ---------------- toast ---------------- */

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(s.toast, LV_OBJ_FLAG_HIDDEN);
    s.toast_timer = NULL;
}

static void toast_show(const char *text, uint32_t ms)
{
    lv_label_set_text(s.toast, text);
    lv_obj_clear_flag(s.toast, LV_OBJ_FLAG_HIDDEN);
    if (s.toast_timer) {
        lv_timer_del(s.toast_timer);
    }
    s.toast_timer = lv_timer_create(toast_hide_cb, ms, NULL);
    lv_timer_set_repeat_count(s.toast_timer, 1);
}

/* ---------------- files page ---------------- */

static void fmt_size(size_t n, char *buf, size_t cap)
{
    if (n < 1024) {
        snprintf(buf, cap, "%uB", (unsigned)n);
    } else if (n < 1024 * 1024) {
        snprintf(buf, cap, "%.1fK", n / 1024.0);
    } else {
        snprintf(buf, cap, "%.2fM", n / 1048576.0);
    }
}

static void files_render(void)
{
    for (int i = 0; i < FILE_VISIBLE; i++) {
        int idx = s.file_top + i;
        lv_obj_t *row = s.file_rows[i];
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (idx < s.file_count) {
            char sz[12];
            fmt_size(s.file_sizes[idx], sz, sizeof(sz));
            char line[96];
            snprintf(line, sizeof(line), "%s %-28s %6s",
                     idx == s.file_selected ? "*" : " ", s.files[idx], sz);
            lv_label_set_text(lbl, line);
            row_set_cursor(row, idx == s.file_cursor);
            if (s.del_armed && idx == s.file_cursor) {
                lv_obj_set_style_bg_color(row, lv_color_hex(C_ERR), 0);
            }
        } else {
            lv_label_set_text(lbl, "");
            row_set_cursor(row, false);
        }
    }
    lv_obj_clear_flag(s.file_empty, s.file_count ? LV_OBJ_FLAG_HIDDEN : 0);
    if (s.file_count) {
        lv_obj_add_flag(s.file_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s.file_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

static void file_list_cb(const storage_file_info_t *info, void *ctx)
{
    (void)ctx;
    if (s.file_count >= FILE_MAX) {
        return;
    }
    strlcpy(s.files[s.file_count], info->name, sizeof(s.files[0]));
    s.file_sizes[s.file_count] = info->size;
    s.file_count++;
}

static void files_refresh(void)
{
    char sel[STORAGE_MAX_NAME] = {0};
    if (s.file_selected >= 0 && s.file_selected < s.file_count) {
        strlcpy(sel, s.files[s.file_selected], sizeof(sel));
    }
    s.file_count = 0;
    storage_list(file_list_cb, NULL);
    s.file_cursor = s.file_count ? (s.file_cursor < s.file_count ? s.file_cursor : s.file_count - 1) : 0;
    s.file_top = 0;
    s.file_selected = -1;
    for (int i = 0; i < s.file_count; i++) {
        if (sel[0] && strcmp(s.files[i], sel) == 0) {
            s.file_selected = i;
        }
    }
    s.del_armed = false;
    files_render();
}

static void files_move_cursor(int delta)
{
    if (s.file_count == 0) {
        return;
    }
    int c = s.file_cursor + delta;
    if (c < 0) c = 0;
    if (c >= s.file_count) c = s.file_count - 1;
    s.file_cursor = c;
    if (c < s.file_top) s.file_top = c;
    if (c >= s.file_top + FILE_VISIBLE) s.file_top = c - FILE_VISIBLE + 1;
    s.del_armed = false;
    files_render();
}

static void files_select(void)
{
    if (s.file_cursor >= s.file_count) {
        return;
    }
    /* OK short-press while delete armed -> confirm delete */
    if (s.del_armed) {
        const char *name = s.files[s.file_cursor];
        if (storage_delete(name) == ESP_OK) {
            prog_log("文件删除: %s", name);
            toast_show("已删除", 1500);
        } else {
            toast_show("删除失败", 1500);
        }
        s.del_armed = false;
        files_refresh();
        return;
    }
    s.file_selected = s.file_cursor;
    strlcpy(s.selected_file, s.files[s.file_cursor], sizeof(s.selected_file));
    lv_label_set_text(s.sel_file_label, s.selected_file);
    files_render();
    toast_show("已选定烧录文件", 1500);
}

static void files_arm_delete(void)
{
    if (s.file_cursor >= s.file_count) {
        return;
    }
    if (s.del_armed && lv_tick_get() < s.del_deadline) {
        /* second long-press within window -> handled as select/confirm path */
        files_select();
        return;
    }
    s.del_armed = true;
    s.del_deadline = lv_tick_get() + 3000;
    files_render();
    toast_show("3 秒内再按 OK 确认删除", 2500);
}

/* ---------------- prog page ---------------- */

static void prog_render_options(void)
{
    const char *names[3] = { "擦除", "编程", "校验" };
    bool vals[3] = { s.opt_erase, s.opt_program, s.opt_verify };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = lv_obj_get_child(s.opt_rows[i], 0);
        char line[32];
        snprintf(line, sizeof(line), "%s [%s]", names[i], vals[i] ? "√" : " ");
        lv_label_set_text(lbl, line);
        row_set_cursor(s.opt_rows[i], !prog_job_busy() && s.prog_cursor == i);
    }
    bool busy = prog_job_busy();
    lv_label_set_text(lv_obj_get_child(s.start_row, 0), busy ? "取 消 任 务" : "开 始 烧 录");
    row_set_cursor(s.start_row, s.prog_cursor == 3);
}

static void prog_action(void)
{
    if (s.prog_cursor < 3) {
        if (prog_job_busy()) {
            return;
        }
        switch (s.prog_cursor) {
        case 0: s.opt_erase = !s.opt_erase; break;
        case 1: s.opt_program = !s.opt_program; break;
        case 2: s.opt_verify = !s.opt_verify; break;
        }
        prog_render_options();
        return;
    }
    /* start row */
    if (prog_job_busy()) {
        prog_job_cancel();
        toast_show("已请求取消", 1500);
        return;
    }
    if (s.file_selected < 0 || s.file_selected >= s.file_count) {
        toast_show("请先在文件页选择固件", 2000);
        return;
    }
    if (!s.opt_erase && !s.opt_program && !s.opt_verify) {
        toast_show("请至少选择一项操作", 2000);
        return;
    }
    prog_job_req_t req = {0};
    strlcpy(req.file, s.selected_file, sizeof(req.file));
    req.erase = s.opt_erase;
    req.program = s.opt_program;
    req.verify = s.opt_verify;
    if (prog_job_start(&req) == ESP_OK) {
        toast_show("任务已启动", 1500);
    } else {
        toast_show("任务启动失败", 2000);
    }
    prog_render_options();
}

/* ---------------- status page ---------------- */

static void status_connect(void)
{
    if (prog_job_busy()) {
        toast_show("烧录进行中", 1500);
        return;
    }
    lv_label_set_text(s.connect_text, "连 接 中 ...");
    lv_timer_handler();   /* force redraw before the blocking call */
    if (prog_job_connect(NULL) == ESP_OK) {
        toast_show("目标连接成功", 1500);
    } else {
        toast_show("连接失败：目标无响应", 2500);
    }
    lv_label_set_text(s.connect_text, "连 接 芯 片");
}

/* ---------------- page switching ---------------- */

static void page_show(int page)
{
    s.page = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == page) {
            lv_obj_clear_flag(s.pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s.pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text(s.title_label, s_page_names[page]);
    static const char *hints[PAGE_COUNT] = {
        "MODE:切页  OK:连接  ONOFF:背光",
        "MODE:切页  OK:选定  长按OK:删除",
        "MODE:切页  OK:切换/开始  MENU:取消",
    };
    lv_label_set_text(s.hint_label, hints[page]);
    if (page == PAGE_FILES) {
        files_refresh();
    }
}

/* ---------------- periodic refresh ---------------- */

static const char *state_text(prog_state_t st, uint32_t *color)
{
    switch (st) {
    case PROG_RUNNING: *color = C_WARN; return "烧录中";
    case PROG_DONE:    *color = C_OK;   return "完成";
    case PROG_FAILED:  *color = C_ERR;  return "失败";
    default:           *color = C_MUTED; return "空闲";
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    prog_status_t snap;
    prog_job_snapshot(&snap);

    /* status page */
    if (snap.dev_valid) {
        lv_label_set_text(s.chip_label, snap.sig.device_name);
        char det[64];
        uint32_t data_size = snap.sig.data_flash_end >= 0xF1000 ?
                             snap.sig.data_flash_end - 0xF1000 + 1 : 0;
        snprintf(det, sizeof(det), "代码闪存 %luK  数据闪存 %s  版本 %X.%X%X",
                 (unsigned long)((snap.sig.code_flash_end + 1) / 1024),
                 data_size ? "有" : "无",
                 snap.sig.fw_version[0], snap.sig.fw_version[1], snap.sig.fw_version[2]);
        lv_label_set_text(s.chip_detail, det);
    } else {
        lv_label_set_text(s.chip_label, "未连接");
        lv_label_set_text(s.chip_detail, "按 OK 连接目标芯片");
    }

    size_t total = 0, used = 0;
    storage_usage(&total, &used);
    char stor[64];
    char u[12], tt[12];
    fmt_size(used, u, sizeof(u));
    fmt_size(total, tt, sizeof(tt));
    snprintf(stor, sizeof(stor), "存储 %s / %s", u, tt);
    lv_label_set_text(s.stor_label, stor);

    uint32_t color;
    const char *st = state_text(snap.state, &color);
    char job[256];
    snprintf(job, sizeof(job), "任务状态: %s  %s", st, snap.message);
    lv_label_set_text(s.job_label, job);
    lv_obj_set_style_text_color(s.job_label, lv_color_hex(
        snap.state == PROG_FAILED ? C_ERR : snap.state == PROG_DONE ? C_OK :
        snap.state == PROG_RUNNING ? C_WARN : C_MUTED), 0);

    /* prog page */
    if (snap.state == PROG_RUNNING || snap.percent > 0) {
        lv_bar_set_value(s.bar, snap.percent, LV_ANIM_OFF);
    }
    char stage[64];
    snprintf(stage, sizeof(stage), "%s  %lu%%", snap.stage, (unsigned long)snap.percent);
    lv_label_set_text(s.stage_label, stage);

    static char logbuf[1024];
    size_t n = prog_job_log_read(logbuf, sizeof(logbuf));
    if (n > 0) {
        /* keep the last line */
        char *p = logbuf + n;
        while (p > logbuf && *p != '\n') p--;
        if (p > logbuf) {
            p--;
            while (p > logbuf && *p != '\n') p--;
        }
        lv_label_set_text(s.log_label, *p == '\n' ? p + 1 : p);
    }

    /* running state may change option-row availability */
    static bool was_busy = false;
    if (was_busy != (snap.state == PROG_RUNNING)) {
        was_busy = (snap.state == PROG_RUNNING);
        prog_render_options();
    }

    /* delete confirm timeout */
    if (s.del_armed && lv_tick_get() > s.del_deadline) {
        s.del_armed = false;
        files_render();
    }
}

/* ---------------- key handling ---------------- */

static void key_cb(lv_timer_t *t)
{
    (void)t;
    key_msg_t m;
    while (xQueueReceive(que_key, &m, 0) == pdTRUE) {
        if (m.evt == KEY_EVT_RELEASE) {
            continue;
        }
        uint16_t key = m.code & 0xFF;
        if (key == KEY_ONOFF && m.evt == KEY_EVT_PRESS) {
            s.backlight_on = !s.backlight_on;
            lcd_backlight_set(s.backlight_on ? 100 : 0);
            continue;
        }
        if (key == KEY_MODE && m.evt == KEY_EVT_PRESS) {
            page_show((s.page + 1) % PAGE_COUNT);
            continue;
        }
        if (key == KEY_MENU && m.evt == KEY_EVT_PRESS) {
            if (s.page == PAGE_PROG && prog_job_busy()) {
                prog_job_cancel();
                toast_show("已请求取消", 1500);
            } else if (s.page != PAGE_STATUS) {
                page_show(PAGE_STATUS);
            }
            continue;
        }
        switch (s.page) {
        case PAGE_STATUS:
            if (key == KEY_OK && m.evt == KEY_EVT_PRESS) {
                status_connect();
            }
            break;
        case PAGE_FILES:
            if ((key == KEY_UP) && (m.evt == KEY_EVT_PRESS || m.evt == KEY_EVT_REPEAT)) {
                files_move_cursor(-1);
            } else if ((key == KEY_DOWN) && (m.evt == KEY_EVT_PRESS || m.evt == KEY_EVT_REPEAT)) {
                files_move_cursor(1);
            } else if (key == KEY_OK && m.evt == KEY_EVT_PRESS) {
                files_select();
            } else if (key == KEY_OK && m.evt == KEY_EVT_LONG) {
                files_arm_delete();
            }
            break;
        case PAGE_PROG:
            if ((key == KEY_UP) && (m.evt == KEY_EVT_PRESS || m.evt == KEY_EVT_REPEAT)) {
                s.prog_cursor = (s.prog_cursor + PROG_ROWS - 1) % PROG_ROWS;
                prog_render_options();
            } else if ((key == KEY_DOWN) && (m.evt == KEY_EVT_PRESS || m.evt == KEY_EVT_REPEAT)) {
                s.prog_cursor = (s.prog_cursor + 1) % PROG_ROWS;
                prog_render_options();
            } else if (key == KEY_OK && m.evt == KEY_EVT_PRESS) {
                prog_action();
            }
            break;
        }
    }
}

/* ---------------- UI construction ---------------- */

static void build_status_page(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 4, 2, 296, 96);
    make_label(card, "目标芯片", 2, 0, C_MUTED);
    s.chip_label = make_label(card, "未连接", 2, 20, C_TEXT);
    s.chip_detail = make_label(card, "按 OK 连接目标芯片", 2, 42, C_MUTED);
    s.connect_row = make_row(card, 62, 280, 24);
    lv_obj_set_style_bg_color(s.connect_row, lv_color_hex(C_ACCENT), 0);
    s.connect_text = make_label(s.connect_row, "连 接 芯 片", 4, 2, 0xFFFFFF);

    lv_obj_t *card2 = make_card(parent, 4, 102, 296, 76);
    make_label(card2, "系统信息", 2, 0, C_MUTED);
    s.net_label = make_label(card2, "", 2, 20, C_TEXT);
    s.stor_label = make_label(card2, "", 2, 40, C_TEXT);
    s.job_label = make_label(card2, "任务状态: 空闲", 2, 58, C_MUTED);
}

static void build_files_page(lv_obj_t *parent)
{
    for (int i = 0; i < FILE_VISIBLE; i++) {
        s.file_rows[i] = make_row(parent, 2 + i * 25, 296, 23);
        make_label(s.file_rows[i], "", 6, 2, C_TEXT);
    }
    s.file_empty = make_label(parent, "暂无固件文件\n请通过 Web 页面上传", 60, 70, C_MUTED);
}

static void build_prog_page(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 4, 2, 296, 24);
    make_label(card, "固件:", 2, 2, C_MUTED);
    s.sel_file_label = make_label(card, "未选择", 44, 2, C_TEXT);

    const char *names[3] = { "擦除 [√]", "编程 [√]", "校验 [√]" };
    for (int i = 0; i < 3; i++) {
        s.opt_rows[i] = make_row(parent, 30 + i * 25, 296, 23);
        make_label(s.opt_rows[i], names[i], 6, 2, C_TEXT);
    }
    s.start_row = make_row(parent, 108, 296, 26);
    lv_obj_set_style_bg_color(s.start_row, lv_color_hex(C_OK), 0);
    s.start_text = make_label(s.start_row, "开 始 烧 录", 6, 4, 0xFFFFFF);

    s.bar = lv_bar_create(parent);
    lv_obj_set_pos(s.bar, 4, 142);
    lv_obj_set_size(s.bar, 296, 14);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_bar_set_value(s.bar, 0, LV_ANIM_OFF);

    s.stage_label = make_label(parent, "空闲", 4, 160, C_TEXT);
    s.log_label = make_label(parent, "", 4, 178, C_MUTED);
    lv_obj_set_width(s.log_label, 296);
    lv_label_set_long_mode(s.log_label, LV_LABEL_LONG_CLIP);
}

void ui_main_start(void)
{
    lv_obj_t *scr = lv_scr_act();
    style_common(scr, C_BG, C_TEXT);

    /* top bar */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    style_common(top, C_BAR, C_TEXT);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, LCD_H_RES, 26);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *app = make_label(top, "RL78 编程器", 8, 5, C_TEXT);
    (void)app;
    s.title_label = make_label(top, "状态", 128, 5, C_ACCENT);
    s.wifi_label = make_label(top, "AP", 200, 5, C_MUTED);

    /* pages */
    for (int i = 0; i < PAGE_COUNT; i++) {
        s.pages[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s.pages[i]);
        lv_obj_set_pos(s.pages[i], 0, 30);
        lv_obj_set_size(s.pages[i], LCD_H_RES, 182);
        lv_obj_clear_flag(s.pages[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(s.pages[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s.pages[i], 0, 0);
    }
    build_status_page(s.pages[PAGE_STATUS]);
    build_files_page(s.pages[PAGE_FILES]);
    build_prog_page(s.pages[PAGE_PROG]);

    /* bottom hint bar */
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_remove_style_all(bot);
    style_common(bot, C_BAR, C_MUTED);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_size(bot, LCD_H_RES, 24);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    s.hint_label = make_label(bot, "", 8, 4, C_MUTED);

    /* toast */
    s.toast = lv_label_create(scr);
    lv_obj_set_style_text_font(s.toast, UI_FONT, 0);
    lv_obj_set_style_text_color(s.toast, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s.toast, lv_color_hex(0x222B38), 0);
    lv_obj_set_style_bg_opa(s.toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s.toast, 6, 0);
    lv_obj_set_style_pad_all(s.toast, 6, 0);
    lv_obj_align(s.toast, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_flag(s.toast, LV_OBJ_FLAG_HIDDEN);

    /* wifi label */
    char wifi[64];
    snprintf(wifi, sizeof(wifi), "%s", CONFIG_PROG_WIFI_SSID);
    lv_label_set_text(s.wifi_label, wifi);
    char net[64];
    snprintf(net, sizeof(net), "热点 %s  http://192.168.4.1", CONFIG_PROG_WIFI_SSID);
    lv_label_set_text(s.net_label, net);

    /* timers (fire after lcd task starts) */
    lv_timer_create(key_cb, 30, NULL);
    lv_timer_create(refresh_cb, 300, NULL);

    page_show(PAGE_STATUS);
    prog_render_options();
    ESP_LOGI(TAG, "UI started");
}
