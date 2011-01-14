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

static int goto_char(u8 *buf, int pos, QECharset *charset)
{
    int nb_chars, c;
    u8 *buf_ptr;

    if (charset != &charset_utf8)
        return pos;

    nb_chars = 0;
    buf_ptr = buf;
    for (;;) {
        c = *buf_ptr;
        if (c < 0x80 || c >= 0xc0) {
            if (nb_chars >= pos)
                break;
            nb_chars++;
        }
        buf_ptr++;
    }
    return buf_ptr - buf;
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

/* find a page at a given offset */
Page *Pages::FindPage(int *offset_ptr, int *idx_ptr)
{
    int offset = *offset_ptr;
    if (!IsOffsetInCache(offset)) {
        int idx = 0;
        Page *p = PageAt(idx);
        while (offset >= p->size) {
            offset -= p->size;
            p = PageAt(++idx);
        }
        cur_page = p;
        cur_offset = *offset_ptr - offset;
        cur_idx = idx;
    }

    *offset_ptr -= cur_offset;
    if (idx_ptr)
        *idx_ptr = cur_idx;
    return cur_page;
}

int Pages::LimitSize(int offset, int size)
{
    if ((offset + size) > total_size)
        size = total_size - offset;
    if (size <= 0)
        return 0;
    return size;
}

void Pages::ReadWrite(int offset, u8 *buf, int size, int do_write)
{
    int len;

    Page *p = FindPage(&offset);
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

int Pages::Read(int offset, void *buf, int size)
{
    size = LimitSize(offset, size);
    if (size > 0)
        ReadWrite(offset, (u8*)buf, size, 0);
    return size;
}

void Pages::Delete(int offset, int size)
{
    int len;

    total_size -= size;
    int idx;
    Page *p = FindPage(&offset, &idx);
    while (size > 0) {
        len = p->size - offset;
        if (len > size)
            len = size;
        if (len == p->size) {
            /* we cannot free if read only */
            if (!p->read_only)
                free(p->data);
            page_table->RemoveAt(idx);
            p = PageAt(idx);
            offset = 0;
        } else {
            p->PrepareForUpdate();
            memmove(p->data + offset, p->data + offset + len, 
                    p->size - offset - len);
            p->size -= len;
            p->data = (u8*)realloc(p->data, p->size);
            offset += len;
            if (offset >= p->size) {
                p = PageAt(++idx);
                offset = 0;
            }
        }
        size -= len;
    }

    /* the page cache is no longer valid */
    InvalidateCache();
    VerifySize();
}

/* internal function for insertion : 'buf' of size 'size' at the
   beginning of the page at page_index */
static void pages_insert(Pages *pages, int page_index, const u8 *buf, int size)
{
    int len;

    if (page_index < pages->nb_pages()) {
        Page *p = pages->PageAt(page_index);
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
    while (size > 0) {
        len = size;
        if (len > MAX_PAGE_SIZE)
            len = MAX_PAGE_SIZE;
        Page *p = new Page(buf, len);
        buf += len;
        size -= len;
        pages->page_table->InsertAt(page_index++, p);
    }
}

/* We must have : 0 <= offset <= pages->total_size */
void Pages::InsertLowLevel(int offset, const u8 *buf, int size)
{
    int len, len_out;
    int page_index = -1;
    total_size += size;
    if (offset > 0) {
        int page_index;
        offset--;
        Page *p = FindPage(&offset, &page_index);
        offset++;

        /* compute what we can insert in current page */
        len = MAX_PAGE_SIZE - offset;
        if (len > size)
            len = size;
        /* number of bytes to put in next pages */
        len_out = p->size + len - MAX_PAGE_SIZE;
        if (len_out > 0)
            pages_insert(this, page_index + 1, p->data + p->size - len_out, len_out);
        else
            len_out = 0;

        /* now we can insert in current page */
        if (len > 0) {
            p = PageAt(page_index);
            p->PrepareForUpdate();
            p->size += len - len_out;
            p->data = (u8*)realloc(p->data, p->size);
            memmove(p->data + offset + len, p->data + offset, p->size - (offset + len));
            memcpy(p->data + offset, buf, len);
            buf += len;
            size -= len;
        }
    }

    /* insert the remaining data in the next pages */
    if (size > 0)
        pages_insert(this, page_index + 1, buf, size);

    InvalidateCache();
    VerifySize();
}

// TODO: not sure I didn't make mistakes converting this to page_table as PtrVec
void Pages::InsertFrom(int dest_offset,
                  Pages *src_pages, int src_offset, int size)
{
    Page *p, *q;
    int size_start, len, n, page_index;
    int p_idx;

    /* insert the data from the first page if it is not completely selected */
    p = src_pages->FindPage(&src_offset, &p_idx);
    if (src_offset > 0) {
        len = p->size - src_offset;
        if (len > size)
            len = size;
        InsertLowLevel(dest_offset, p->data + src_offset, len);
        dest_offset += len;
        size -= len;
        p = src_pages->PageAt(++p_idx);
    }

    if (size == 0)
        return;

    /* cut the page at dest offset if needed */
    page_index = nb_pages();
    if (dest_offset < total_size) {
        q = FindPage(&dest_offset, &page_index);
        if (dest_offset > 0) {
            page_index++;
            pages_insert(this, page_index, q->data + dest_offset, q->size - dest_offset);
            /* must reload q because page_table may have been
               realloced */
            q = PageAt(page_index - 1);
            p->PrepareForUpdate();
            q->data = (u8*)realloc(q->data, dest_offset);
            q->size = dest_offset;
        }
    }

    total_size += size;

    /* compute the number of complete pages to insert */
    n = 0;
    int p_start = p_idx;
    size_start = size;
    while (size > 0 && p->size <= size) {
        size -= p->size;
        ++n;
        if (size > 0)
            p = src_pages->PageAt(++p_idx);
    }

    if (n > 0) {
        Page **qarr = page_table->MakeSpaceAt(page_index, n);
        p_idx = p_start;
        p = src_pages->PageAt(p_idx);
        page_index += n;
        while (n > 0) {
            len = p->size;
            q = new Page();
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
            p = src_pages->PageAt(++p_idx);
            q++;
        }
    }
    
    /* insert the remaning bytes */
    if (size > 0) {
        pages_insert(this, page_index, p->data, size);
    }

    InvalidateCache();
    VerifySize();
}

int pages_get_char_offset(Pages *pages, int offset, QECharset *charset)
{
    int pos = 0;
    for (int idx=0; idx < pages->nb_pages(); idx++) {
        Page *p = pages->PageAt(idx);
        if (offset < p->size) {
            pos += get_chars(p->data, offset, charset);
            break;
        }
        p->CalcChars(charset);
        pos += p->nb_chars;
        offset -= p->size;
    }
    return pos;
}

int pages_goto_char(Pages *pages, QECharset *charset, int pos)
{
    int offset = 0;
    for (int idx=0; idx < pages->nb_pages(); idx++) {
        Page *p = pages->PageAt(idx);
        p->CalcChars(charset);
        if (pos < p->nb_chars) {
            offset += goto_char(p->data, pos, charset);
            break;
        } else {
            pos -= p->nb_chars;
            offset += p->size;
        }
    }
    return offset;
}

int pages_get_pos(Pages *pages, CharsetDecodeState *charset_state, int *line_ptr, int *col_ptr, int offset)
{
    QASSERT(offset >= 0);
    int line = 0, col = 0;
    for (int idx=0; idx < pages->nb_pages(); idx++) {
        Page *p = pages->PageAt(idx);
        if (offset < p->size) {
            int line1, col1;
            get_pos(p->data, offset, &line1, &col1, charset_state);
            line += line1;
            if (line1)
                col = 0;
            col += col1;
            break;
        }
        p->CalcPos(charset_state);
        line += p->nb_lines;
        if (p->nb_lines)
            col = 0;
        col += p->col;
        offset -= p->size;
    }
    *line_ptr = line;
    *col_ptr = col;
    return line;
}

int pages_goto_pos(Pages *pages, CharsetDecodeState *charset_state, int line1, int col1)
{
    int line2, col2, offset1;
    u8 *q, *q_end;

    int line = 0, col = 0, offset = 0;
    for (int idx=0; idx < pages->nb_pages(); idx++) {
        Page *p = pages->PageAt(idx);
        p->CalcPos(charset_state);
        line2 = line + p->nb_lines;
        if (p->nb_lines)
            col2 = 0;
        col2 = col + p->col;
        if (line2 > line1 || (line2 == line1 && col2 >= col1)) {
            /* compute offset */
            q = p->data;
            q_end = p->data + p->size;
            /* seek to the correct line */
            while (line < line1) {
                col = 0;
                q = (u8*)memchr(q, '\n', q_end - q);
                q++;
                line++;
            }
            /* test if we want to go after the end of the line */
            offset += q - p->data;
            while (col < col1 && pages_nextc(pages, charset_state, offset, &offset1) != '\n') {
                col++;
                offset = offset1;
            }
            return offset;
        }
        line = line2;
        col = col2;
        offset += p->size;
    }
    return pages->total_size;
}

int pages_nextc(Pages *pages, CharsetDecodeState *charset_state, int offset, int *next_offset)
{
    u8 buf[MAX_CHAR_BYTES], *p;
    int ch;

    if (offset >= pages->total_size) {
        offset = pages->total_size;
        ch = '\n';
        goto Exit;
    }

    pages->Read(offset, buf, 1);
    
    /* we use directly the charset conversion table to go faster */
    ch = charset_state->table[buf[0]];
    offset++;
    if (ch == ESCAPE_CHAR) {
        pages->Read(offset, buf + 1, MAX_CHAR_BYTES - 1);
        p = buf;
        ch = charset_state->decode_func(charset_state, (const u8 **)&p);
        offset += (p - buf) - 1;
    }

Exit:
    if (next_offset)
        *next_offset = offset;
    return ch;
}

int pages_prevc(Pages *pages, QECharset *charset, int offset, int *prev_offset)
{
   int ch;
   u8 buf[MAX_CHAR_BYTES], *q;

   if (offset <= 0) {
       offset = 0;
       ch = '\n';
   } else {
       /* XXX: it cannot be generic here. Should use the
          line/column system to be really generic */
       offset--;
       q = buf + sizeof(buf) - 1;
       pages->Read(offset, q, 1);
       if (charset == &charset_utf8) {
           while (*q >= 0x80 && *q < 0xc0) {
               if (offset == 0 || q == buf) {
                   /* error : take only previous char */
                   offset += buf - 1 - q;
                   ch = buf[sizeof(buf) - 1];
                   goto the_end;
               }
               offset--;
               q--;
               pages->Read(offset, q, 1);
           }
           ch = utf8_decode((const char **)(void *)&q);
       } else {
           ch = *q;
       }
   }
the_end:
   if (prev_offset)
       *prev_offset = offset;
   return ch;
}

