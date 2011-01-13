#include "qe.h"
#include "pages.h"

/************************************************************/
/* char offset computation */

int get_chars(u8 *buf, int size, QECharset *charset)
{
    int nb_chars, c;
    u8 *buf_end, *buf_ptr;

    if (charset != &charset_utf8)
        return size;

    nb_chars = 0;
    buf_ptr = buf;
    buf_end = buf + size;
    while (buf_ptr < buf_end) {
        c = *buf_ptr++;
        if (c < 0x80 || c >= 0xc0)
            nb_chars++;
    }
    return nb_chars;
}

/* return the number of lines and column position for a buffer */
void get_pos(u8 *buf, int size, int *line_ptr, int *col_ptr, CharsetDecodeState *s)
{
    u8 *p, *p1, *lp;
    int line, len, col, ch;

    QASSERT(size >= 0);

    line = 0;
    p = buf;
    lp = p;
    p1 = p + size;
    for (;;) {
        p = (u8*)memchr(p, '\n', p1 - p);
        if (!p)
            break;
        p++;
        lp = p;
        line++;
    }
    /* now compute number of chars (XXX: potential problem if out of
       block, but for UTF8 it works) */
    col = 0;
    while (lp < p1) {
        ch = s->table[*lp];
        if (ch == ESCAPE_CHAR) {
            /* XXX: utf8 only is handled */
            len = utf8_length[*lp];
            lp += len;
        } else {
            lp++;
        }
        col++;
    }
    *line_ptr = line;
    *col_ptr = col;
}

/* prepare a page to be written */
void update_page(Page *p)
{
    u8 *buf;

    /* if the page is read only, copy it */
    if (p->read_only) {
        buf = (u8*)malloc(p->size);
        /* XXX: should return an error */
        if (!buf)
            return;
        memcpy(buf, p->data, p->size);
        p->data = buf;
        p->read_only = 0;
    }
    invalidate_attrs(p);
}

void page_calc_chars(Page *p, QECharset *charset)
{
    if (!p->valid_char) {
        p->valid_char = 1;
        p->nb_chars = get_chars(p->data, p->size, charset);
    }
}

void page_calc_pos(Page *p, CharsetDecodeState *charset_state)
{
    if (!p->valid_pos) {
        p->valid_pos = 1;
        get_pos(p->data, p->size, &p->nb_lines, &p->col, charset_state);
    }
}

