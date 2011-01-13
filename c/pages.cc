#include "qe.h"
#include "pages.h"

/* char offset computation */
static int get_chars(u8 *buf, int size, QECharset *charset)
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
static void get_pos(u8 *buf, int size, int *line_ptr, int *col_ptr, CharsetDecodeState *s)
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
void Page::PrepareForUpdate()
{
    u8 *buf;

    /* if the page is read only, copy it */
    if (read_only) {
        buf = (u8*)malloc(size);
        /* XXX: should return an error */
        if (!buf)
            return;
        memcpy(buf, data, size);
        data = buf;
        read_only = 0;
    }
    InvalidateAttrs();
}

void Page::CalcChars(QECharset *charset)
{
    if (!valid_char) {
        valid_char = 1;
        nb_chars = get_chars(data, size, charset);
    }
}

void Page::CalcPos(CharsetDecodeState *charset_state)
{
    if (!valid_pos) {
        valid_pos = 1;
        get_pos(data, size, &nb_lines, &col, charset_state);
    }
}

static int pages_offset_in_cache(Pages *pages, int offset)
{
    return (NULL != pages->cur_page) && 
           (offset >= pages->cur_offset) && 
           (offset < (pages->cur_offset + pages->cur_page->size));
}

/* find a page at a given offset */
static Page *pages_find_page(Pages *pages, int *offset_ptr)
{
    Page *p;

    int offset = *offset_ptr;
    if (pages_offset_in_cache(pages, offset)) {
        *offset_ptr -= pages->cur_offset;
        return pages->cur_page;
    }

    p = pages->page_table;
    while (offset >= p->size) {
        offset -= p->size;
        p++;
    }
    pages->cur_page = p;
    pages->cur_offset = *offset_ptr - offset;
    *offset_ptr = offset;
    return p;
}

void pages_rw(Pages *pages, int offset, u8 *buf, int size, int do_write)
{
    int len;

    Page *p = pages_find_page(pages, &offset);
    while (size > 0) {
        len = p->size - offset;
        if (len > size)
            len = size;
        if (do_write) {
            p->PrepareForUpdate();
            memcpy(p->data + offset, buf, len);
        } else {
            memcpy(buf, p->data + offset, len);
        }
        buf += len;
        size -= len;
        offset += len;
        if (offset >= p->size) {
            p++;
            offset = 0;
        }
    }
}

void pages_delete(Pages *pages, int offset, int size)
{
    int n, len;
    Page *del_start, *p;

    pages->total_size -= size;
    p = pages_find_page(pages, &offset);
    n = 0;
    del_start = NULL;
    while (size > 0) {
        len = p->size - offset;
        if (len > size)
            len = size;
        if (len == p->size) {
            if (!del_start)
                del_start = p;
            /* we cannot free if read only */
            if (!p->read_only)
                free(p->data);
            p++;
            offset = 0;
            n++;
        } else {
            p->PrepareForUpdate();
            memmove(p->data + offset, p->data + offset + len, 
                    p->size - offset - len);
            p->size -= len;
            p->data = (u8*)realloc(p->data, p->size);
            offset += len;
            if (offset >= p->size) {
                p++;
                offset = 0;
            }
        }
        size -= len;
    }

    /* now delete the requested pages */
    if (n > 0) {
        pages->nb_pages -= n;
        memmove(del_start, del_start + n, 
                (pages->page_table + pages->nb_pages - del_start) * sizeof(Page));
        pages->page_table = (Page*)realloc(pages->page_table, pages->nb_pages * sizeof(Page));
    }

    /* the page cache is no longer valid */
    pages->cur_page = NULL;
}

/* internal function for insertion : 'buf' of size 'size' at the
   beginning of the page at page_index */
static void pages_insert(Pages *pages, int page_index, const u8 *buf, int size)
{
    int len, n;
    Page *p;

    if (page_index < pages->nb_pages) {
        p = pages->page_table + page_index;
        len = MAX_PAGE_SIZE - p->size;
        if (len > size)
            len = size;
        if (len > 0) {
            p->PrepareForUpdate();
            p->data = (u8*)realloc(p->data, p->size + len);
            memmove(p->data + len, p->data, p->size);
            memcpy(p->data, buf + size - len, len);
            size -= len;
            p->size += len;
        }
    }
    
    /* now add new pages if necessary */
    n = (size + MAX_PAGE_SIZE - 1) / MAX_PAGE_SIZE;
    if (0 == n)
        return;

    pages->nb_pages += n;
    pages->page_table = (Page*)realloc(pages->page_table, pages->nb_pages * sizeof(Page));
    p = pages->page_table + page_index;
    memmove(p + n, p, sizeof(Page) * (pages->nb_pages - n - page_index));
    while (size > 0) {
        len = size;
        if (len > MAX_PAGE_SIZE)
            len = MAX_PAGE_SIZE;
        p->size = len;
        p->data = (u8*)malloc(len);
        p->ClearAttrs();
        memcpy(p->data, buf, len);
        buf += len;
        size -= len;
        p++;
    }
}

/* We must have : 0 <= offset <= pages->total_size */
void pages_insert_lowlevel(Pages *pages, int offset, const u8 *buf, int size)
{
    int len, len_out, page_index;
    Page *p = pages->page_table;
    pages->total_size += size;
    if (offset > 0) {
        offset--;
        p = pages_find_page(pages, &offset);
        offset++;

        /* compute what we can insert in current page */
        len = MAX_PAGE_SIZE - offset;
        if (len > size)
            len = size;
        /* number of bytes to put in next pages */
        len_out = p->size + len - MAX_PAGE_SIZE;
        page_index = p - pages->page_table;
        if (len_out > 0)
            pages_insert(pages, page_index + 1, 
                       p->data + p->size - len_out, len_out);
        else
            len_out = 0;
        /* now we can insert in current page */
        if (len > 0) {
            p = pages->page_table + page_index;
            p->PrepareForUpdate();
            p->size += len - len_out;
            p->data = (u8*)realloc(p->data, p->size);
            memmove(p->data + offset + len, p->data + offset, p->size - (offset + len));
            memcpy(p->data + offset, buf, len);
            buf += len;
            size -= len;
        }
    } else {
        page_index = -1;
    }
    /* insert the remaining data in the next pages */
    if (size > 0)
        pages_insert(pages, page_index + 1, buf, size);

    /* the page cache is no longer valid */
    pages->cur_page = NULL;
}

void pages_insert_from(Pages *dest_pages, int dest_offset,
                  Pages *src_pages, int src_offset, int size)
{
    Page *p, *p_start, *q;
    int size_start, len, n, page_index;

    /* insert the data from the first page if it is not completely selected */
    p = pages_find_page(src_pages, &src_offset);
    if (src_offset > 0) {
        len = p->size - src_offset;
        if (len > size)
            len = size;
        pages_insert_lowlevel(dest_pages, dest_offset, p->data + src_offset, len);
        dest_offset += len;
        size -= len;
        p++;
    }

    if (size == 0)
        return;

    /* cut the page at dest offset if needed */
    if (dest_offset < dest_pages->total_size) {
        q = pages_find_page(dest_pages, &dest_offset);
        page_index = q - dest_pages->page_table;
        if (dest_offset > 0) {
            page_index++;
            pages_insert(dest_pages, page_index, q->data + dest_offset, q->size - dest_offset);
            /* must reload q because page_table may have been
               realloced */
            q = dest_pages->page_table + page_index - 1;
            p->PrepareForUpdate();
            q->data = (u8*)realloc(q->data, dest_offset);
            q->size = dest_offset;
        }
    } else {
        page_index = dest_pages->nb_pages;
    }

    dest_pages->total_size += size;

    /* compute the number of complete pages to insert */
    p_start = p;
    size_start = size;
    while (size > 0 && p->size <= size) {
        size -= p->size;
        p++;
    }
    n = p - p_start; /* number of pages to insert */
    p = p_start;
    if (n > 0) {
        /* add the pages */
        dest_pages->nb_pages += n;
        dest_pages->page_table = (Page*)realloc(dest_pages->page_table, dest_pages->nb_pages * sizeof(Page));
        q = dest_pages->page_table + page_index;
        memmove(q + n, q, sizeof(Page) * (dest_pages->nb_pages - n - page_index));
        p = p_start;
        while (n > 0) {
            len = p->size;
            q->size = len;
            if (p->read_only) {
                /* simply copy the reference */
                q->read_only = 1;
                p->InvalidateAttrs();
                q->data = p->data;
            } else {
                /* allocate a new page */
                p->ClearAttrs();
                q->data = (u8*)malloc(len);
                memcpy(q->data, p->data, len);
            }
            n--;
            p++;
            q++;
        }
        page_index = q - dest_pages->page_table;
    }
    
    /* insert the remaning bytes */
    if (size > 0) {
        pages_insert(dest_pages, page_index, p->data, size);
    }

    /* the page cache is no longer valid */
    dest_pages->cur_page = NULL;
}

int pages_get_char_offset(Pages *pages, int offset, QECharset *charset)
{
    int pos = 0;
    Page *p, *p_end;
    p = pages->page_table;
    p_end = p + pages->nb_pages;
    for (;;) {
        if (p >= p_end)
            return pos;

        if (offset < p->size)
            break;
        p->CalcChars(charset);
        pos += p->nb_chars;
        offset -= p->size;
        p++;
    }
    pos += get_chars(p->data, offset, charset);
    return pos;
}

int pages_get_pos(Pages *pages, CharsetDecodeState *charset_state, int *line_ptr, int *col_ptr, int offset)
{
    Page *p, *p_end;
    int line, col, line1, col1;
    QASSERT(offset >= 0);
    line = 0;
    col = 0;
    p = pages->page_table;
    p_end = p + pages->nb_pages;
    for (;;) {
        if (p >= p_end)
            goto the_end;

        if (offset < p->size)
            break;
        p->CalcPos(charset_state);
        line += p->nb_lines;
        if (p->nb_lines)
            col = 0;
        col += p->col;
        offset -= p->size;
        p++;
    }
    get_pos(p->data, offset, &line1, &col1, charset_state);
    line += line1;
    if (line1)
        col = 0;
    col += col1;
the_end:
    *line_ptr = line;
    *col_ptr = col;
    return line;
}

