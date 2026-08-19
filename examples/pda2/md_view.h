/**
 * @file      md_view.h
 * @brief     Draws a laid-out Markdown page with LVGL widgets.
 *
 * The block styling (which font, indent and marker each kind gets) lives here
 * so the notes editor and the reader render Markdown identically; md_layout.c
 * does the pagination, this turns its rows into labels.
 */
#ifndef __MD_VIEW_H__
#define __MD_VIEW_H__

#include "lvgl.h"
#include "md_parse.h"
#include "md_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t  *parent;    /* container the rows are created in            */
    lv_obj_t **rows;      /* caller-owned array, max_rows entries         */
    int        max_rows;
    int        view_w;    /* usable text width, px                        */
    int        view_h;    /* usable height, px                            */
    int        origin_x;  /* left padding inside the container            */
    bool       big;       /* bump body/code up a size for easier reading  */
} md_view_t;

/* Delete the row objects from a previous page. */
void md_view_clear(const md_view_t *v);

/* Render one page starting at `cur`; returns the cursor for the next page. */
md_cursor_t md_view_render(const md_view_t *v, const char *text,
                           const md_block_t *blocks, int nblocks, md_cursor_t cur);

/* Same pagination without creating objects — for "where would the next page
 * start?" queries. */
md_cursor_t md_view_measure(const md_view_t *v, const char *text,
                            const md_block_t *blocks, int nblocks, md_cursor_t cur);

#ifdef __cplusplus
}
#endif

#endif /* __MD_VIEW_H__ */
