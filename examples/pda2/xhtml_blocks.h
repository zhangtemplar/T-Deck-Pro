/**
 * @file      xhtml_blocks.h
 * @brief     Convert an EPUB chapter's XHTML into the block model the renderer
 *            already understands.
 *
 * XHTML maps almost one-to-one onto md_block_t — h1..h3, p, li, blockquote,
 * pre and hr all have direct equivalents — so a chapter can go straight into
 * md_layout/md_view and inherit pagination, CJK wrapping and styling for free.
 *
 * This is a tag scanner, not an XML parser: it ignores attributes it doesn't
 * need, tolerates unclosed tags, and never allocates. Inline markup (<em>,
 * <strong>, <a>) is dropped since there is no bold face to render it with.
 */
#ifndef __XHTML_BLOCKS_H__
#define __XHTML_BLOCKS_H__

#include "md_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Convert `xml` into flattened text plus blocks pointing into it.
 *
 *   text_out / text_cap  destination for the extracted text
 *   blocks / max_blocks  destination for the block table
 *
 * Returns the number of blocks produced; *text_len receives the text length.
 * Output is truncated rather than overflowing if either buffer fills. */
int xhtml_to_blocks(const char *xml, size_t xml_len,
                    char *text_out, size_t text_cap, size_t *text_len,
                    md_block_t *blocks, int max_blocks);

#ifdef __cplusplus
}
#endif

#endif /* __XHTML_BLOCKS_H__ */
