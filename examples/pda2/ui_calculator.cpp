/**
 * @file      ui_calculator.cpp
 * @brief     Scientific Calculator app using Shunting-Yard expression parser.
 *            Adapted for T-Deck Pro factory framework (scr_mgr pattern).
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

// ---- Expression Parser (Shunting-Yard) ----

#define MAX_EXPR_LEN    256
#define MAX_STACK       64
#define MAX_HISTORY     10

static bool deg_mode = true;
static char history[MAX_HISTORY][MAX_EXPR_LEN];
static char history_results[MAX_HISTORY][64];
static int history_count = 0;

static double to_rad(double x) { return deg_mode ? x * M_PI / 180.0 : x; }
static double from_rad(double x) { return deg_mode ? x * 180.0 / M_PI : x; }

static int factorial_int(int n)
{
    if (n < 0 || n > 20) return -1;
    int r = 1;
    for (int i = 2; i <= n; i++) r *= i;
    return r;
}

enum TokenType { TOK_NUM, TOK_OP, TOK_FUNC, TOK_LPAREN, TOK_RPAREN, TOK_END };

struct Token {
    TokenType type;
    double num;
    char op;
    char func[10];
};

static int op_prec(char op)
{
    switch (op) {
    case '+': case '-': return 1;
    case '*': case '/': case '%': return 2;
    case '^': return 3;
    default: return 0;
    }
}

static bool op_right_assoc(char op) { return op == '^'; }
static const char *skip_spaces(const char *s) { while (*s == ' ') s++; return s; }

static const char *parse_token(const char *s, Token *t)
{
    s = skip_spaces(s);
    if (*s == '\0') { t->type = TOK_END; return s; }
    if (*s == '(') { t->type = TOK_LPAREN; return s + 1; }
    if (*s == ')') { t->type = TOK_RPAREN; return s + 1; }

    if (*s == '+' || *s == '-' || *s == '*' || *s == '/' || *s == '^' || *s == '%') {
        t->type = TOK_OP;
        t->op = *s;
        return s + 1;
    }

    if (isdigit(*s) || *s == '.') {
        t->type = TOK_NUM;
        char *end;
        t->num = strtod(s, &end);
        return end;
    }

    if (isalpha(*s)) {
        int i = 0;
        while (isalpha(*s) && i < 9) t->func[i++] = *s++;
        t->func[i] = '\0';
        if (strcmp(t->func, "pi") == 0) { t->type = TOK_NUM; t->num = M_PI; return s; }
        if (strcmp(t->func, "e") == 0 && !isalpha(*s)) { t->type = TOK_NUM; t->num = M_E; return s; }
        t->type = TOK_FUNC;
        return s;
    }

    t->type = TOK_END;
    return s;
}

static bool apply_func(const char *func, double arg, double *result)
{
    if (strcmp(func, "sin") == 0) { *result = sin(to_rad(arg)); return true; }
    if (strcmp(func, "cos") == 0) { *result = cos(to_rad(arg)); return true; }
    if (strcmp(func, "tan") == 0) { *result = tan(to_rad(arg)); return true; }
    if (strcmp(func, "asin") == 0) { *result = from_rad(asin(arg)); return true; }
    if (strcmp(func, "acos") == 0) { *result = from_rad(acos(arg)); return true; }
    if (strcmp(func, "atan") == 0) { *result = from_rad(atan(arg)); return true; }
    if (strcmp(func, "log") == 0) { *result = log10(arg); return true; }
    if (strcmp(func, "ln") == 0) { *result = log(arg); return true; }
    if (strcmp(func, "exp") == 0) { *result = exp(arg); return true; }
    if (strcmp(func, "sqrt") == 0) { *result = sqrt(arg); return true; }
    if (strcmp(func, "abs") == 0) { *result = fabs(arg); return true; }
    if (strcmp(func, "fact") == 0) {
        int f = factorial_int((int)arg);
        if (f < 0) return false;
        *result = (double)f;
        return true;
    }
    return false;
}

static bool apply_op(char op, double a, double b, double *result)
{
    switch (op) {
    case '+': *result = a + b; return true;
    case '-': *result = a - b; return true;
    case '*': *result = a * b; return true;
    case '/': if (b == 0) return false; *result = a / b; return true;
    case '%': if (b == 0) return false; *result = fmod(a, b); return true;
    case '^': *result = pow(a, b); return true;
    default: return false;
    }
}

static bool evaluate(const char *expr, double *result)
{
    double num_stack[MAX_STACK];
    int num_top = -1;
    char op_stack[MAX_STACK];
    int op_top = -1;
    char func_stack[MAX_STACK][10];
    int func_top = -1;
    int last_type = 0;

    const char *s = expr;
    Token tok;

    while (1) {
        s = parse_token(s, &tok);
        if (tok.type == TOK_END) break;

        if (tok.type == TOK_NUM) {
            if (num_top >= MAX_STACK - 1) return false;
            num_stack[++num_top] = tok.num;
            last_type = 1;
        } else if (tok.type == TOK_FUNC) {
            if (func_top >= MAX_STACK - 1 || op_top >= MAX_STACK - 1) return false;
            func_top++;
            strncpy(func_stack[func_top], tok.func, 9);
            func_stack[func_top][9] = '\0';
            op_stack[++op_top] = 'F';
            last_type = 0;
        } else if (tok.type == TOK_OP) {
            if (tok.op == '-' && last_type == 0) {
                s = parse_token(s, &tok);
                if (tok.type == TOK_NUM) {
                    if (num_top >= MAX_STACK - 1) return false;
                    num_stack[++num_top] = -tok.num;
                    last_type = 1;
                    continue;
                } else if (tok.type == TOK_LPAREN) {
                    if (num_top >= MAX_STACK - 1) return false;
                    num_stack[++num_top] = 0;
                    op_stack[++op_top] = '-';
                    op_stack[++op_top] = '(';
                    last_type = 0;
                    continue;
                } else {
                    return false;
                }
            }
            while (op_top >= 0 && op_stack[op_top] != '(' && op_stack[op_top] != 'F' &&
                   (op_prec(op_stack[op_top]) > op_prec(tok.op) ||
                    (op_prec(op_stack[op_top]) == op_prec(tok.op) && !op_right_assoc(tok.op)))) {
                if (num_top < 1) return false;
                double b = num_stack[num_top--];
                double a = num_stack[num_top--];
                double r;
                if (!apply_op(op_stack[op_top--], a, b, &r)) return false;
                num_stack[++num_top] = r;
            }
            if (op_top >= MAX_STACK - 1) return false;
            op_stack[++op_top] = tok.op;
            last_type = 0;
        } else if (tok.type == TOK_LPAREN) {
            if (op_top >= MAX_STACK - 1) return false;
            op_stack[++op_top] = '(';
            last_type = 0;
        } else if (tok.type == TOK_RPAREN) {
            while (op_top >= 0 && op_stack[op_top] != '(') {
                if (op_stack[op_top] == 'F') {
                    if (num_top < 0 || func_top < 0) return false;
                    double a = num_stack[num_top--];
                    double r;
                    if (!apply_func(func_stack[func_top--], a, &r)) return false;
                    op_top--;
                    num_stack[++num_top] = r;
                } else {
                    if (num_top < 1) return false;
                    double b = num_stack[num_top--];
                    double a = num_stack[num_top--];
                    double r;
                    if (!apply_op(op_stack[op_top--], a, b, &r)) return false;
                    num_stack[++num_top] = r;
                }
            }
            if (op_top < 0) return false;
            op_top--;
            if (op_top >= 0 && op_stack[op_top] == 'F') {
                if (num_top < 0 || func_top < 0) return false;
                double a = num_stack[num_top--];
                double r;
                if (!apply_func(func_stack[func_top--], a, &r)) return false;
                op_top--;
                num_stack[++num_top] = r;
            }
            last_type = 1;
        }
    }

    while (op_top >= 0) {
        if (op_stack[op_top] == '(' || op_stack[op_top] == ')') return false;
        if (op_stack[op_top] == 'F') {
            if (num_top < 0 || func_top < 0) return false;
            double a = num_stack[num_top--];
            double r;
            if (!apply_func(func_stack[func_top--], a, &r)) return false;
            op_top--;
            num_stack[++num_top] = r;
        } else {
            if (num_top < 1) return false;
            double b = num_stack[num_top--];
            double a = num_stack[num_top--];
            double r;
            if (!apply_op(op_stack[op_top--], a, b, &r)) return false;
            num_stack[++num_top] = r;
        }
    }

    if (num_top != 0) return false;
    *result = num_stack[0];
    return true;
}

// ---- UI (factory screen manager pattern) ----

#include "peripheral.h"

#define FONT_CALC &lv_font_montserrat_14

static lv_obj_t *expr_ta = NULL;
static lv_obj_t *result_label = NULL;
static lv_obj_t *history_list = NULL;
static bool calc_active = false;

static void format_result(double val, char *buf, size_t len)
{
    if (isinf(val)) snprintf(buf, len, "Infinity");
    else if (isnan(val)) snprintf(buf, len, "NaN");
    else if (val == (long long)val && fabs(val) < 1e15) snprintf(buf, len, "%lld", (long long)val);
    else snprintf(buf, len, "%.10g", val);
}

static void do_calculate()
{
    const char *expr = lv_textarea_get_text(expr_ta);
    if (!expr || expr[0] == '\0') return;

    double result;
    char buf[64];
    if (evaluate(expr, &result)) {
        format_result(result, buf, sizeof(buf));
        lv_label_set_text_fmt(result_label, "= %s", buf);

        if (history_count < MAX_HISTORY) {
            strncpy(history[history_count], expr, MAX_EXPR_LEN - 1);
            strncpy(history_results[history_count], buf, 63);
            history_count++;
        } else {
            memmove(history[0], history[1], (MAX_HISTORY - 1) * MAX_EXPR_LEN);
            memmove(history_results[0], history_results[1], (MAX_HISTORY - 1) * 64);
            strncpy(history[MAX_HISTORY - 1], expr, MAX_EXPR_LEN - 1);
            strncpy(history_results[MAX_HISTORY - 1], buf, 63);
        }

        if (history_list) {
            static char hist_buf[2048];
            hist_buf[0] = '\0';
            for (int i = history_count - 1; i >= 0; i--) {
                char line[384];
                snprintf(line, sizeof(line), "%s = %s\n", history[i], history_results[i]);
                strncat(hist_buf, line, sizeof(hist_buf) - strlen(hist_buf) - 1);
            }
            lv_label_set_text(history_list, hist_buf);
        }
    } else {
        lv_label_set_text(result_label, "= Error");
    }
}

static void calc_back_cb(lv_event_t *e)
{
    scr_mgr_pop(false);
}

void calc_keyboard_poll()
{
    if (!calc_active || !expr_ta) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    Serial.printf("[CALC] key: '%c' (0x%02x)\n", c, c);

    if (c == '\n') {
        do_calculate();
    } else if (c == '\b') {
        const char *text = lv_textarea_get_text(expr_ta);
        if (!text || text[0] == '\0') {
            calc_active = false;
            scr_mgr_pop(false);
        } else {
            lv_textarea_del_char(expr_ta);
        }
    } else if (c == '$') {
        /* speaker key — ignore */
    } else {
        lv_textarea_add_char(expr_ta, c);
    }
}

static void calc_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Calculator", calc_back_cb);

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 230, 270);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 4, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);

    result_label = lv_label_create(cont);
    lv_obj_set_width(result_label, lv_pct(100));
    lv_obj_set_style_text_font(result_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_text(result_label, "= ");

    history_list = lv_label_create(cont);
    lv_obj_set_width(history_list, lv_pct(100));
    lv_obj_set_flex_grow(history_list, 1);
    lv_obj_set_style_text_font(history_list, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(history_list, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_label_set_long_mode(history_list, LV_LABEL_LONG_WRAP);
    lv_label_set_text(history_list, "Enter to eval, Bksp to back");

    expr_ta = lv_textarea_create(cont);
    /* No cursor blink: each blink is a full e-ink refresh, so a focused
     * field would repaint the panel twice a second forever. */

    lv_obj_set_style_anim_time(expr_ta, 0, LV_PART_CURSOR);
    lv_obj_set_width(expr_ta, lv_pct(100));
    lv_obj_set_height(expr_ta, 36);
    lv_textarea_set_placeholder_text(expr_ta, "e.g. sin(45)+2*3");
    lv_textarea_set_one_line(expr_ta, true);
    lv_textarea_set_max_length(expr_ta, MAX_EXPR_LEN);
    lv_obj_set_style_text_font(expr_ta, FONT_CALC, LV_PART_MAIN);

    calc_active = true;
}

static void calc_entry(void) { ui_disp_full_refr(); }
static void calc_exit(void) { ui_disp_full_refr(); }
static void calc_destroy(void) {
    calc_active = false;
    expr_ta = NULL;
    result_label = NULL;
    history_list = NULL;
}

scr_lifecycle_t screen_calculator = {
    .create = calc_create,
    .entry = calc_entry,
    .exit = calc_exit,
    .destroy = calc_destroy,
};
