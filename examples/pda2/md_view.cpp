/**
 * @file      md_view.cpp
 * @brief     LVGL rendering of a Markdown page. See md_view.h.
 */
#include "md_view.h"
#include "cjk_font.h"
#include "src/assets.h"
#include <stdio.h>
#include <string.h>

#define MD_VIEW_LINE_GAP  2
#define MD_VIEW_SCRATCH   2048

/* One scratch buffer for stripped text; md_layout only needs it for the row it
 * is emitting, and rendering never runs re-entrantly. */
static char s_scratch[MD_VIEW_SCRATCH];

/* Fonts per block kind. `big` shifts the body text up for comfortable reading;
 * headings are already large so H1 stays put. */
static const lv_font_t *block_font(md_kind_t k, bool big)
{
    switch (k) {
    case MD_H1:   return &lv_font_montserrat_26;
    case MD_H2:   return big ? &lv_font_montserrat_26 : &g_font_cn_large;
    case MD_H3:   return big ? &g_font_cn_large : &Font_Mono_Bold_16;
    case MD_CODE: return big ? &Font_Mono_Bold_16 : &Font_Mono_Bold_14;
    default:      return big ? &g_font_cn_large : &g_font_cn;
    }
}

static int block_indent(const md_block_t *b)
{
    switch (b->kind) {
    case MD_BULLET:
    case MD_NUMBER: return 10 + 12 * b->indent;
    case MD_QUOTE:  return 12;
    case MD_CODE:   return 8;
    default:        return 0;
    }
}

/* ---- md_layout environment ---- */

static int mv_advance(uint32_t cp, void *f)
{
    return lv_font_get_glyph_width((const lv_font_t *)f, cp, 0);
}

static void *mv_font_for(md_kind_t k, void *ctx)
{
    return (void *)block_font(k, *(const bool *)ctx);
}

static int mv_line_height(void *f)
{
    return lv_font_get_line_height((const lv_font_t *)f);
}

static int mv_indent_for(const md_block_t *b, void *ctx)
{
    LV_UNUSED(ctx);
    return block_indent(b);
}

static void build_env(const md_view_t *v, md_layout_env_t *env, const bool *big)
{
    env->advance     = mv_advance;
    env->font_for    = mv_font_for;
    env->line_height = mv_line_height;
    env->indent_for  = mv_indent_for;
    env->ctx         = (void *)big;
    env->view_w      = v->view_w;
    env->view_h      = v->view_h;
    env->line_gap    = MD_VIEW_LINE_GAP;
    env->max_rows    = v->max_rows;
    env->scratch     = s_scratch;
    env->scratch_sz  = sizeof(s_scratch);
}

/* ---- emit ---- */

struct emit_ctx {
    const md_view_t *v;
    int row;
};

static void mv_emit(void *user, const md_block_t *b, int line_idx,
                    const char *text, size_t len, int x, int y, void *font)
{
    struct emit_ctx *e = (struct emit_ctx *)user;
    const md_view_t *v = e->v;
    if (e->row >= v->max_rows) return;

    if (b->kind == MD_RULE) {
        lv_obj_t *hr = lv_obj_create(v->parent);
        lv_obj_set_size(hr, v->view_w - 20, 2);
        lv_obj_set_pos(hr, v->origin_x + 10, y + 3);
        lv_obj_set_style_bg_color(hr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(hr, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(hr, 0, LV_PART_MAIN);
        v->rows[e->row++] = hr;
        return;
    }

    lv_obj_t *lb = lv_label_create(v->parent);
    lv_obj_set_style_text_font(lb, (const lv_font_t *)font, LV_PART_MAIN);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lb, v->view_w - x + 4);

    /* The list marker / quote bar belongs on the block's first row only. */
    char buf[320];
    char numbuf[12];
    const char *prefix = "";
    if (line_idx == 0 && b->kind == MD_BULLET) prefix = "\xE2\x80\xA2 ";     /* • */
    else if (line_idx == 0 && b->kind == MD_NUMBER) {
        snprintf(numbuf, sizeof(numbuf), "%u. ", (unsigned)b->ord);
        prefix = numbuf;
    } else if (b->kind == MD_QUOTE) prefix = "\xE2\x94\x82 ";                /* │ */

    int pl = snprintf(buf, sizeof(buf), "%s", prefix);
    int cp = (int)len;
    if (pl + cp > (int)sizeof(buf) - 1) cp = (int)sizeof(buf) - 1 - pl;
    if (cp > 0) memcpy(buf + pl, text, (size_t)cp);
    buf[pl + (cp > 0 ? cp : 0)] = '\0';
    lv_label_set_text(lb, buf);

    /* Hang the marker into the indent so wrapped rows align under the text. */
    int px = v->origin_x + x;
    if (line_idx == 0 && (b->kind == MD_BULLET || b->kind == MD_NUMBER)) px -= 10;
    else if (b->kind == MD_QUOTE) px -= 12;
    lv_obj_set_pos(lb, px, y);

    v->rows[e->row++] = lb;
}

/* ---- public ---- */

void md_view_clear(const md_view_t *v)
{
    for (int i = 0; i < v->max_rows; i++) {
        if (v->rows[i]) { lv_obj_del(v->rows[i]); v->rows[i] = NULL; }
    }
}

md_cursor_t md_view_render(const md_view_t *v, const char *text,
                           const md_block_t *blocks, int nblocks, md_cursor_t cur)
{
    md_view_clear(v);
    bool big = v->big;
    md_layout_env_t env;
    build_env(v, &env, &big);
    struct emit_ctx ec = { v, 0 };
    return md_layout_page(text, blocks, nblocks, cur, &env, mv_emit, &ec);
}

md_cursor_t md_view_measure(const md_view_t *v, const char *text,
                            const md_block_t *blocks, int nblocks, md_cursor_t cur)
{
    bool big = v->big;
    md_layout_env_t env;
    build_env(v, &env, &big);
    return md_layout_page(text, blocks, nblocks, cur, &env, NULL, NULL);
}
