/**
 * @file      md_layout.c
 * @brief     Markdown page layout. See md_layout.h.
 */
#include "md_layout.h"
#include <string.h>

/* Height contributed by a block that occupies no text rows. */
#define BLANK_FRACTION 2      /* blank line = half a line height */
#define RULE_HEIGHT    8

md_cursor_t md_layout_page(const char *text,
                           const md_block_t *blocks, int nblocks,
                           md_cursor_t cur, const md_layout_env_t *env,
                           md_emit_cb emit, void *user)
{
    int y = 0;
    int row = 0;
    int blk = cur.blk;
    int line_in_blk = cur.line;

    if (blk < 0) blk = 0;
    if (line_in_blk < 0) line_in_blk = 0;

    while (blk < nblocks && row < env->max_rows) {
        const md_block_t *b = &blocks[blk];
        void *font = env->font_for(b->kind, env->ctx);
        int lh = env->line_height(font) + env->line_gap;

        if (b->kind == MD_BLANK) {
            int h = lh / BLANK_FRACTION;
            /* Only break on a spacer if something is already on the page,
             * otherwise a run of blanks at a page boundary would never end. */
            if (y + h > env->view_h && row > 0) break;
            y += h;
            blk++;
            line_in_blk = 0;
            continue;
        }

        if (b->kind == MD_RULE) {
            if (y + RULE_HEIGHT > env->view_h && row > 0) break;
            if (emit) emit(user, b, 0, NULL, 0, env->indent_for(b, env->ctx), y, font);
            row++;
            y += RULE_HEIGHT;
            blk++;
            line_in_blk = 0;
            continue;
        }

        /* Inline markers carry no styling, so strip them before measuring —
         * except in code blocks, which are verbatim by definition. */
        size_t clen;
        size_t src = b->len;
        if (src > env->scratch_sz - 1) src = env->scratch_sz - 1;
        if (b->kind == MD_CODE) {
            memcpy(env->scratch, text + b->off, src);
            clen = src;
        } else {
            clen = md_strip_inline(text + b->off, src, env->scratch);
        }
        env->scratch[clen] = '\0';

        int indent = env->indent_for(b, env->ctx);
        txt_layout_t lay;
        lay.advance      = env->advance;
        lay.ctx          = font;
        lay.width        = env->view_w - indent;
        lay.letter_space = 0;
        lay.tab_width    = 4;
        lay.max_lines    = env->max_rows;
        if (lay.width < 40) lay.width = 40;

        /* An empty non-blank block (e.g. "## " with no text) still consumes a
         * row so the cursor keeps moving. */
        if (clen == 0) {
            if (y + lh > env->view_h && row > 0) break;
            if (emit) emit(user, b, 0, env->scratch, 0, indent, y, font);
            row++;
            y += lh;
            blk++;
            line_in_blk = 0;
            continue;
        }

        size_t pos = 0;
        int idx = 0;
        while (pos < clen) {
            uint16_t ll = 0;
            size_t next = txt_layout_line(env->scratch, clen, pos, &lay, &ll);

            if (idx >= line_in_blk) {
                /* Row 0 of a page always fits, which is what guarantees the
                 * cursor advances even for an oversized font. */
                if (row > 0 && (y + lh > env->view_h || row >= env->max_rows)) {
                    md_cursor_t nxt = { blk, idx };
                    return nxt;
                }
                if (emit) emit(user, b, idx, env->scratch + pos, ll, indent, y, font);
                row++;
                y += lh;
            }

            idx++;
            if (next == pos) break;
            pos = next;
        }

        blk++;
        line_in_blk = 0;
    }

    md_cursor_t nxt = { blk, 0 };
    return nxt;
}
