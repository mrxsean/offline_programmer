#include "ui_main.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_app_format.h"

#include "drv_st7789v.h"
#include "drv_touch_key.h"
#include "drv_beep.h"
#include "prog_job.h"
#include "storage.h"

static const char *TAG = "ui";

/* ---------------- light theme palette ---------------- */
#define C_BG        0xEEF2F7   /* app background */
#define C_BAR       0xFFFFFF   /* top/bottom bar */
#define C_CARD      0xFFFFFF
#define C_BORDER    0xD5DEE8
#define C_TRACK     0xE3E9F0
#define C_TEXT      0x1F2733
#define C_MUTED     0x6B7A8C
#define C_ACCENT    0x1D5FD1
#define C_ACCENT_BG 0xDCE8FB
#define C_OK        0x1E9E50
#define C_ERR       0xD93B2B
#define C_WARN      0xC77700

LV_FONT_DECLARE(lv_font_prog_cjk_16);
#define UI_FONT   (&lv_font_prog_cjk_16)

#define PAGE_STATUS 0
#define PAGE_FILES  1
#define PAGE_PROG   2
#define PAGE_COUNT  3

#define FILE_VISIBLE 6
#define FILE_MAX     20
#define PROG_ROWS    4   /* 擦除 / 编程 / 校验 / 开始 */

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
    lv_obj_t *connect_btn;
    lv_obj_t *connect_text;

    /* files page */
    lv_obj_t *files_head;
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
    lv_obj_t *opt_boxes[3];         /* 擦除/编程/校验 */
    lv_obj_t *opt_labels[3];
    lv_obj_t *start_btn;
    lv_obj_t *start_text;
    lv_obj_t *bar;
    lv_obj_t *stage_label;
    lv_obj_t *log_label;
    int prog_cursor;
    bool opt_erase;
    bool opt_program;
    bool opt_verify;
    char selected_file[STORAGE_MAX_NAME];

} s = {
    .file_selected = -1,
    .opt_erase = true,
    .opt_program = true,
    .opt_verify = true,
};

/* ---------------- widget helpers ---------------- */

static void style_text(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(obj, UI_FONT, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    style_text(l, color);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* white card with border, pad 8 */
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    return c;
}

/* list row: white bg, radius; cursor highlight handled by row_set_cursor */
static lv_obj_t *make_row(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_radius(r, 6, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    return r;
}

static void row_set_cursor(lv_obj_t *row, bool on, bool danger)
{
    uint32_t bg = C_CARD, fg = C_TEXT;
    if (danger) {
        bg = C_ERR; fg = 0xFFFFFF;
    } else if (on) {
        bg = C_ACCENT; fg = 0xFFFFFF;
    }
    lv_obj_set_style_bg_color(row, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(on || danger ? bg : C_BORDER), 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *lbl = lv_obj_get_child(row, i);
        if (lbl) {
            style_text(lbl, fg);
        }
    }
}

/* filled button with centered label; returns the label */
static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h,
                             uint32_t bg, const char *text, lv_obj_t **btn_out)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_style_bg_color(r, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 6, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_t *l = lv_label_create(r);
    style_text(l, 0xFFFFFF);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (btn_out) {
        *btn_out = r;
    }
    return l;
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
        lv_obj_t *name_lbl = lv_obj_get_child(row, 0);
        lv_obj_t *size_lbl = lv_obj_get_child(row, 1);
        if (idx < s.file_count) {
            char sz[12];
            fmt_size(s.file_sizes[idx], sz, sizeof(sz));
            char line[64];
            snprintf(line, sizeof(line), "%s %s",
                     idx == s.file_selected ? "*" : " ", s.files[idx]);
            lv_label_set_text(name_lbl, line);
            lv_label_set_text(size_lbl, sz);
            row_set_cursor(row, idx == s.file_cursor,
                           s.del_armed && idx == s.file_cursor);
        } else {
            lv_label_set_text(name_lbl, "");
            lv_label_set_text(size_lbl, "");
            row_set_cursor(row, false, false);
        }
    }
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

    size_t total = 0, used = 0;
    storage_usage(&total, &used);
    char u[12], tt[12], head[64];
    fmt_size(used, u, sizeof(u));
    fmt_size(total, tt, sizeof(tt));
    snprintf(head, sizeof(head), "固件文件 (%d)   存储 %s/%s", s.file_count, u, tt);
    lv_label_set_text(s.files_head, head);

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
            beep_play(BEEP_PAT_DONE);
        } else {
            toast_show("删除失败", 1500);
            beep_play(BEEP_PAT_FAIL);
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
        files_select();   /* second long-press within window -> confirm */
        return;
    }
    s.del_armed = true;
    s.del_deadline = lv_tick_get() + 3000;
    files_render();
    toast_show("3 秒内再按 OK 确认删除", 2500);
}

/* ---------------- prog page ---------------- */

static const char *s_opt_names[3] = { "擦除", "编程", "校验" };

static void prog_render_options(void)
{
    bool vals[3] = { s.opt_erase, s.opt_program, s.opt_verify };
    bool busy = prog_job_busy();
    for (int i = 0; i < 3; i++) {
        lv_obj_t *box = s.opt_boxes[i];
        lv_obj_t *lbl = s.opt_labels[i];
        char line[16];
        snprintf(line, sizeof(line), "%s %s", s_opt_names[i], vals[i] ? "√" : "－");
        lv_label_set_text(lbl, line);

        uint32_t bg, fg, bd;
        if (busy) {
            bg = C_CARD; fg = C_MUTED; bd = C_BORDER;          /* locked */
        } else if (s.prog_cursor == i) {
            bg = C_ACCENT; fg = 0xFFFFFF; bd = C_ACCENT;       /* cursor */
        } else if (vals[i]) {
            bg = C_ACCENT_BG; fg = C_ACCENT; bd = C_ACCENT;    /* checked */
        } else {
            bg = C_CARD; fg = C_MUTED; bd = C_BORDER;          /* unchecked */
        }
        lv_obj_set_style_bg_color(box, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(box, lv_color_hex(bd), 0);
        style_text(lbl, fg);
    }
    lv_label_set_text(s.start_text, busy ? "取 消 任 务" : "开 始 烧 录");
    lv_obj_set_style_bg_color(s.start_btn, lv_color_hex(busy ? C_ERR : C_OK), 0);
    /* cursor outline on the start button */
    lv_obj_set_style_border_width(s.start_btn, s.prog_cursor == 3 ? 2 : 0, 0);
    lv_obj_set_style_border_color(s.start_btn, lv_color_hex(C_TEXT), 0);
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
        beep_play(BEEP_PAT_START);
    } else {
        toast_show("任务启动失败", 2000);
        beep_play(BEEP_PAT_FAIL);
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
        beep_play(BEEP_PAT_DONE);
    } else {
        toast_show("连接失败：目标无响应", 2500);
        beep_play(BEEP_PAT_FAIL);
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
        "MENU:切页   OK:连接   ONOFF:主页",
        "OK:选定   长按OK:删除   MODE:返回",
        "OK:切换/开始   MODE:返回/取消",
    };
    lv_label_set_text(s.hint_label, hints[page]);
    if (page == PAGE_FILES) {
        files_refresh();
    }
}

/* ---------------- periodic refresh ---------------- */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    prog_status_t snap;
    prog_job_snapshot(&snap);

    /* status page */
    if (snap.dev_valid) {
        lv_label_set_text(s.chip_label, snap.sig.device_name);
        char det[96];
        uint32_t data_kb = snap.sig.data_flash_end >= 0xF1000 ?
                           (snap.sig.data_flash_end - 0xF1000 + 1) / 1024 : 0;
        char dsz[16];
        if (data_kb) {
            snprintf(dsz, sizeof(dsz), "%luK", (unsigned long)data_kb);
        } else {
            snprintf(dsz, sizeof(dsz), "无");
        }
        snprintf(det, sizeof(det), "代码闪存 %luK   数据闪存 %s",
                 (unsigned long)((snap.sig.code_flash_end + 1) / 1024), dsz);
        lv_label_set_text(s.chip_detail, det);
    } else {
        lv_label_set_text(s.chip_label, "未连接");
        lv_label_set_text(s.chip_detail, "按 OK 连接目标芯片");
    }

    size_t total = 0, used = 0;
    storage_usage(&total, &used);
    char stor[64], u[12], tt[12];
    fmt_size(used, u, sizeof(u));
    fmt_size(total, tt, sizeof(tt));
    snprintf(stor, sizeof(stor), "存储空间  %s / %s", u, tt);
    lv_label_set_text(s.stor_label, stor);

    uint32_t jc = C_MUTED;
    const char *js = "空闲";
    if (snap.state == PROG_RUNNING)      { jc = C_WARN; js = "烧录中"; }
    else if (snap.state == PROG_DONE)    { jc = C_OK;   js = "完成"; }
    else if (snap.state == PROG_FAILED)  { jc = C_ERR;  js = "失败"; }
    char job[256];
    snprintf(job, sizeof(job), "任务状态  %s  %s", js, snap.message);
    lv_label_set_text(s.job_label, job);
    style_text(s.job_label, jc);

    /* prog page */
    if (snap.state == PROG_RUNNING || snap.percent > 0) {
        lv_bar_set_value(s.bar, snap.percent, LV_ANIM_OFF);
    }
    char stage[96];
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

    /* job-finish audible alerts (fires regardless of trigger: LCD or Web) */
    static prog_state_t last_state = PROG_IDLE;
    if (last_state == PROG_RUNNING && snap.state == PROG_DONE) {
        beep_play(BEEP_PAT_DONE);
    } else if (last_state == PROG_RUNNING && snap.state == PROG_FAILED) {
        beep_play(BEEP_PAT_FAIL);
    }
    last_state = snap.state;

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
    if (que_key == NULL) {
        return;   /* touch driver not initialized yet */
    }
    key_msg_t m;
    while (xQueueReceive(que_key, &m, 0) == pdTRUE) {
        if (m.evt == KEY_EVT_RELEASE) {
            continue;
        }
        uint16_t key = m.code & 0xFF;

        /* audible click for every handled key press / auto-repeat */
        if ((m.evt == KEY_EVT_PRESS || m.evt == KEY_EVT_REPEAT) &&
            (key == KEY_UP || key == KEY_DOWN || key == KEY_OK ||
             key == KEY_MENU || key == KEY_MODE || key == KEY_ONOFF)) {
            beep_play(BEEP_PAT_KEY);
        }

        /* ONOFF: jump straight to the home (status) page from anywhere */
        if (key == KEY_ONOFF && m.evt == KEY_EVT_PRESS) {
            if (s.page != PAGE_STATUS) {
                page_show(PAGE_STATUS);
            }
            continue;
        }
        /* MODE (top-right): Back / cancel */
        if (key == KEY_MODE && m.evt == KEY_EVT_PRESS) {
            if (s.page == PAGE_PROG && prog_job_busy()) {
                prog_job_cancel();
                toast_show("已请求取消", 1500);
            } else if (s.page != PAGE_STATUS) {
                page_show(PAGE_STATUS);
            }
            continue;
        }
        /* MENU: next page */
        if (key == KEY_MENU && m.evt == KEY_EVT_PRESS) {
            page_show((s.page + 1) % PAGE_COUNT);
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
    /* chip card: x8 y4 w304 h88, pad 8 -> content 288x72 */
    lv_obj_t *card = make_card(parent, 8, 4, 304, 88);
    make_label(card, "目标芯片", 0, 1, C_MUTED);
    s.connect_text = make_button(card, 200, 0, 88, 26, C_ACCENT, "连 接 芯 片", &s.connect_btn);
    s.chip_label = make_label(card, "未连接", 0, 30, C_TEXT);
    s.chip_detail = make_label(card, "按 OK 连接目标芯片", 0, 52, C_MUTED);

    /* system card: x8 y98 w304 h86, pad 8 -> content 288x70 */
    lv_obj_t *card2 = make_card(parent, 8, 98, 304, 86);
    make_label(card2, "系统信息", 0, 0, C_MUTED);
    s.net_label = make_label(card2, "", 0, 18, C_TEXT);
    s.stor_label = make_label(card2, "", 0, 36, C_TEXT);
    s.job_label = make_label(card2, "任务状态  空闲", 0, 54, C_MUTED);
}

static void build_files_page(lv_obj_t *parent)
{
    s.files_head = make_label(parent, "固件文件", 8, 3, C_MUTED);
    for (int i = 0; i < FILE_VISIBLE; i++) {
        lv_obj_t *row = make_row(parent, 8, 26 + i * 26, 304, 24);
        lv_obj_t *name_lbl = lv_label_create(row);
        style_text(name_lbl, C_TEXT);
        lv_obj_set_pos(name_lbl, 8, 4);
        lv_obj_set_width(name_lbl, 224);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_t *size_lbl = lv_label_create(row);
        style_text(size_lbl, C_MUTED);
        lv_obj_set_pos(size_lbl, 236, 4);
        lv_obj_set_width(size_lbl, 60);
        lv_obj_set_style_text_align(size_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        s.file_rows[i] = row;
    }
    s.file_empty = make_label(parent, "暂无固件文件\n请通过 Web 页面上传", 60, 70, C_MUTED);
}

static void build_prog_page(lv_obj_t *parent)
{
    make_label(parent, "固件:", 8, 6, C_MUTED);
    s.sel_file_label = make_label(parent, "未选择", 52, 6, C_TEXT);
    lv_obj_set_width(s.sel_file_label, 260);
    lv_label_set_long_mode(s.sel_file_label, LV_LABEL_LONG_CLIP);

    /* three option boxes side by side */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(box, 8 + i * 104, 30);
        lv_obj_set_size(box, 96, 30);
        lv_obj_t *lbl = lv_label_create(box);
        style_text(lbl, C_TEXT);
        lv_obj_center(lbl);
        s.opt_boxes[i] = box;
        s.opt_labels[i] = lbl;
    }

    s.start_text = make_button(parent, 8, 70, 304, 36, C_OK, "开 始 烧 录", &s.start_btn);

    s.bar = lv_bar_create(parent);
    lv_obj_set_pos(s.bar, 8, 118);
    lv_obj_set_size(s.bar, 304, 16);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s.bar, 0, 0);
    lv_obj_set_style_radius(s.bar, 8, 0);
    lv_obj_set_style_radius(s.bar, 8, LV_PART_INDICATOR);
    lv_bar_set_value(s.bar, 0, LV_ANIM_OFF);

    s.stage_label = make_label(parent, "空闲", 8, 142, C_TEXT);
    s.log_label = make_label(parent, "", 8, 162, C_MUTED);
    lv_obj_set_width(s.log_label, 304);
    lv_label_set_long_mode(s.log_label, LV_LABEL_LONG_WRAP);
}

void ui_main_start(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* top bar (28px, bottom border) */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_style_bg_color(top, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(top, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, LCD_H_RES, 28);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    make_label(top, "RL78 编程器", 10, 5, C_TEXT);
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", esp_app_get_description()->version);
    make_label(top, ver, 104, 5, C_MUTED);
    s.title_label = lv_label_create(top);
    style_text(s.title_label, C_ACCENT);
    lv_obj_align(s.title_label, LV_ALIGN_TOP_MID, 0, 5);
    s.wifi_label = lv_label_create(top);
    style_text(s.wifi_label, C_MUTED);
    lv_obj_align(s.wifi_label, LV_ALIGN_TOP_RIGHT, -10, 5);

    /* pages (y=30, h=184) */
    for (int i = 0; i < PAGE_COUNT; i++) {
        s.pages[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s.pages[i]);
        lv_obj_set_pos(s.pages[i], 0, 30);
        lv_obj_set_size(s.pages[i], LCD_H_RES, 184);
        lv_obj_clear_flag(s.pages[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(s.pages[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s.pages[i], 0, 0);
    }
    build_status_page(s.pages[PAGE_STATUS]);
    build_files_page(s.pages[PAGE_FILES]);
    build_prog_page(s.pages[PAGE_PROG]);

    /* bottom hint bar (24px, top border) */
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_remove_style_all(bot);
    lv_obj_set_style_bg_color(bot, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bot, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(bot, 1, 0);
    lv_obj_set_style_border_side(bot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_size(bot, LCD_H_RES, 24);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    s.hint_label = lv_label_create(bot);
    style_text(s.hint_label, C_MUTED);
    lv_obj_align(s.hint_label, LV_ALIGN_TOP_MID, 0, 4);

    /* toast */
    s.toast = lv_label_create(scr);
    style_text(s.toast, 0xFFFFFF);
    lv_obj_set_style_bg_color(s.toast, lv_color_hex(0x222B38), 0);
    lv_obj_set_style_bg_opa(s.toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s.toast, 6, 0);
    lv_obj_set_style_pad_all(s.toast, 6, 0);
    lv_obj_align(s.toast, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_add_flag(s.toast, LV_OBJ_FLAG_HIDDEN);

    /* static labels */
    lv_label_set_text(s.wifi_label, CONFIG_PROG_WIFI_SSID);
    char net[64];
    snprintf(net, sizeof(net), "热点 %s   192.168.4.1", CONFIG_PROG_WIFI_SSID);
    lv_label_set_text(s.net_label, net);

    /* timers (fire after the LVGL task starts) */
    lv_timer_create(key_cb, 30, NULL);
    lv_timer_create(refresh_cb, 300, NULL);

    page_show(PAGE_STATUS);
    prog_render_options();
    ESP_LOGI(TAG, "UI started (light theme)");
}
