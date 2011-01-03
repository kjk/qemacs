/*
 * Buffer handling for QEmacs
 * Copyright (c) 2000 Fabrice Bellard.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "qe.h"
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <assert.h>

static void eb_addlog(EditBuffer *b, enum LogOperation op, 
                      int offset, int size);

extern EditBufferDataType raw_data_type;

EditBufferDataType *first_buffer_data_type = NULL;

/************************************************************/
/* basic access to the edit buffer */

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

static Page *find_page(EditBuffer *b, int *offset_ptr)
{
    Pages *pages = &b->pages;
    return pages_find_page(pages, offset_ptr);
}

/* prepare a page to be written */
static void update_page(Page *p)
{
    u8 *buf;

    /* if the page is read only, copy it */
    if (p->flags & PG_READ_ONLY) {
        buf = malloc(p->size);
        /* XXX: should return an error */
        if (!buf)
            return;
        memcpy(buf, p->data, p->size);
        p->data = buf;
        p->flags &= ~PG_READ_ONLY;
    }
    p->flags &= ~(PG_VALID_POS | PG_VALID_CHAR | PG_VALID_COLORS);
}

/* Read or write in the buffer. We must have 0 <= offset < b->total_size */
static int eb_rw(EditBuffer *b, int offset, u8 *buf, int size1, int do_write)
{
    Page *p;
    int len, size;

    if ((offset + size1) > b->total_size)
        size1 = b->total_size - offset;

    if (size1 <= 0)
        return 0;

    size = size1;
    if (do_write)
        eb_addlog(b, LOGOP_WRITE, offset, size);        
    
    p = find_page(b, &offset);
    while (size > 0) {
        len = p->size - offset;
        if (len > size)
            len = size;
        if (do_write) {
            update_page(p);
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
    return size1;
}

/* We must have: 0 <= offset < b->total_size */
int eb_read(EditBuffer *b, int offset, void *buf, int size)
{
    return eb_rw(b, offset, buf, size, 0);
}

/* Note: eb_write can be used to insert after the end of the buffer */
void eb_write(EditBuffer *b, int offset, void *buf_arg, int size)
{
    int len, left;
    u8 *buf = buf_arg;
    
    len = eb_rw(b, offset, buf, size, 1);
    left = size - len;
    if (left > 0) {
        offset += len;
        buf += len;
        eb_insert(b, offset, buf, left);
    }
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
            update_page(p);
            p->data = realloc(p->data, p->size + len);
            memmove(p->data + len, p->data, p->size);
            memcpy(p->data, buf + size - len, len);
            size -= len;
            p->size += len;
        }
    }
    
    /* now add new pages if necessary */
    n = (size + MAX_PAGE_SIZE - 1) / MAX_PAGE_SIZE;
    if (n > 0) {
        pages->nb_pages += n;
        pages->page_table = realloc(pages->page_table, pages->nb_pages * sizeof(Page));
        p = pages->page_table + page_index;
        memmove(p + n, p, sizeof(Page) * (pages->nb_pages - n - page_index));
        while (size > 0) {
            len = size;
            if (len > MAX_PAGE_SIZE)
                len = MAX_PAGE_SIZE;
            p->size = len;
            p->data = malloc(len);
            p->flags = 0;
            memcpy(p->data, buf, len);
            buf += len;
            size -= len;
            p++;
        }
    }
}

static void eb_insert1(EditBuffer *b, int page_index, const u8 *buf, int size)
{
    Pages *pages = &b->pages;
    pages_insert(pages, page_index, buf, size);
}

/* We must have : 0 <= offset <= b->total_size */
static void eb_insert_lowlevel(EditBuffer *b, int offset,
                               const u8 *buf, int size)
{
    int len, len_out, page_index;
    Page *p;
    Pages *pages = &b->pages;

    b->total_size += size;

    /* find the correct page */
    p = pages->page_table;
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
            update_page(p);
            p->size += len - len_out;
            p->data = realloc(p->data, p->size);
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

/* Insert 'size bytes of 'src' buffer from position 'src_offset' into
   buffer 'dest' at offset 'dest_offset'. 'src' MUST BE DIFFERENT from
   'dest' */
void eb_insert_buffer(EditBuffer *dest, int dest_offset, 
                      EditBuffer *src, int src_offset, 
                      int size)
{
    Page *p, *p_start, *q;
    int size_start, len, n, page_index;
    Pages *dest_pages, *src_pages;

    if (size == 0)
        return;

    dest_pages = &dest->pages;
    src_pages = &src->pages;

    eb_addlog(dest, LOGOP_INSERT, dest_offset, size);

    /* insert the data from the first page if it is not completely
       selected */
    p = pages_find_page(src_pages, &src_offset);
    if (src_offset > 0) {
        len = p->size - src_offset;
        if (len > size)
            len = size;
        eb_insert_lowlevel(dest, dest_offset, p->data + src_offset, len);
        dest_offset += len;
        size -= len;
        p++;
    }

    if (size == 0)
        return;

    /* cut the page at dest offset if needed */
    if (dest_offset < dest->total_size) {
        q = pages_find_page(dest_pages, &dest_offset);
        page_index = q - dest_pages->page_table;
        if (dest_offset > 0) {
            page_index++;
            pages_insert(dest_pages, page_index, q->data + dest_offset, 
                       q->size - dest_offset);
            /* must reload q because page_table may have been
               realloced */
            q = dest_pages->page_table + page_index - 1;
            update_page(q);
            q->data = realloc(q->data, dest_offset);
            q->size = dest_offset;
        }
    } else {
        page_index = dest_pages->nb_pages;
    }

    /* update total_size */
    dest->total_size += size;
    
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
        dest_pages->page_table = realloc(dest_pages->page_table,
                                   dest_pages->nb_pages * sizeof(Page));
        q = dest_pages->page_table + page_index;
        memmove(q + n, q, 
                sizeof(Page) * (dest_pages->nb_pages - n - page_index));
        p = p_start;
        while (n > 0) {
            len = p->size;
            q->size = len;
            if (p->flags & PG_READ_ONLY) {
                /* simply copy the reference */
                q->flags = PG_READ_ONLY;
                q->data = p->data;
            } else {
                /* allocate a new page */
                q->flags = 0;
                q->data = malloc(len);
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

/* Insert 'size' bytes from 'buf' into 'b' at offset 'offset'. We must
   have : 0 <= offset <= b->total_size */
void eb_insert(EditBuffer *b, int offset, const void *buf, int size)
{
    eb_addlog(b, LOGOP_INSERT, offset, size);

    eb_insert_lowlevel(b, offset, buf, size);
}

/* Append 'size' bytes from 'buf' at the end of 'b' */
void eb_append(EditBuffer *b, const void *buf, int size)
{
    eb_insert(b, b->total_size, buf, size);
}

/* We must have : 0 <= offset <= b->total_size */
void eb_delete(EditBuffer *b, int offset, int size)
{
    int n, len;
    Page *del_start, *p;
    Pages *pages;

    if (offset >= b->total_size)
        return;

    b->total_size -= size;
    eb_addlog(b, LOGOP_DELETE, offset, size);
    pages = &b->pages;

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
            if (!(p->flags & PG_READ_ONLY))
                free(p->data);
            p++;
            offset = 0;
            n++;
        } else {
            update_page(p);
            memmove(p->data + offset, p->data + offset + len, 
                    p->size - offset - len);
            p->size -= len;
            p->data = realloc(p->data, p->size);
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
        pages->page_table = realloc(pages->page_table, pages->nb_pages * sizeof(Page));
    }

    /* the page cache is no longer valid */
    pages->cur_page = NULL;
}

/* flush the log */
void eb_log_reset(EditBuffer *b)
{
    if (b->log_buffer) {
        eb_free(b->log_buffer);
        b->log_buffer = NULL;
        b->log_new_index = 0;
        b->log_current = 0;
        b->nb_logs = 0;
    }
    b->modified = 0;
}

/* rename a buffer and add characters so that the name is unique */
void set_buffer_name(EditBuffer *b, const char *name1)
{
    char name[sizeof(b->name)];
    int n, pos;

    pstrcpy(name, sizeof(b->name) - 10, name1);
    /* set the buffer name to NULL since it will be changed */
    b->name[0] = '\0';
    pos = strlen(name);
    n = 2;
    while (eb_find(name) != NULL) {
        sprintf(name + pos, "<%d>", n);
        n++;
    }
    pstrcpy(b->name, sizeof(b->name), name);
}

EditBuffer *eb_new(const char *name, int flags)
{
    QEmacsState *qs = &qe_state;
    EditBuffer *b;

    b = malloc(sizeof(EditBuffer));
    if (!b)
        return NULL;
    memset(b, 0, sizeof(EditBuffer));

    pstrcpy(b->name, sizeof(b->name), name);
    b->flags = flags;

    /* set default data type */
    b->data_type = &raw_data_type;

    /* XXX: suppress save_log and always use flag ? */
    b->save_log = ((flags & BF_SAVELOG) != 0);

    /* add buffer in global buffer list */
    b->next = qs->first_buffer;
    qs->first_buffer = b;

    /* CG: default charset should be selectable */
    eb_set_charset(b, &charset_8859_1);
    
    /* add mark move callback */
    eb_add_callback(b, eb_offset_callback, &b->mark);

    if (0 == strcmp(name, "*trace*"))
        trace_buffer = b;

    return b;
}

#if WIN32
#include <io.h> /* for _open, _close, _write */
#define open _open
#define write _write

inline int close(int fd)
{
    return _close(fd);
}
#endif

void eb_free(EditBuffer *b)
{
    QEmacsState *qs = &qe_state;
    EditBuffer **pb;
    EditBufferCallbackList *l, *l1;

    /* call user defined close */
    if (b->close)
        b->close(b);

    /* free each callback */
    for (l = b->first_callback; l != NULL;) {
        l1 = l->next;
        free(l);
        l = l1;
    }
    b->first_callback = NULL;

    b->save_log = 0;
    eb_delete(b, 0, b->total_size);
    eb_log_reset(b);

    /* suppress mmap file handle */
#ifdef WIN32
    if (b->file_handle != 0) {
        CloseHandle(b->file_mapping);
        CloseHandle(b->file_handle);
    }
#else
    if (b->file_handle > 0) {
        close(b->file_handle);
    }
#endif

    /* suppress from buffer list */
    pb = &qs->first_buffer;
    while (*pb != NULL) {
        if (*pb == b)
            break;
        pb = &(*pb)->next;
    }
    *pb = (*pb)->next;

    free(b);
}

EditBuffer *eb_find(const char *name)
{
    QEmacsState *qs = &qe_state;
    EditBuffer *b;

    b = qs->first_buffer;
    while (b != NULL) {
        if (!strcmp(b->name, name))
            return b;
        b = b->next;
    }
    return NULL;
}

EditBuffer *eb_find_file(const char *filename)
{
    QEmacsState *qs = &qe_state;
    EditBuffer *b;

    b = qs->first_buffer;
    while (b != NULL) {
        /* XXX: should also use stat to ensure this is same file */
        if (!strcmp(b->filename, filename))
            return b;
        b = b->next;
    }
    return NULL;
}

/************************************************************/
/* callbacks */

int eb_add_callback(EditBuffer *b, EditBufferCallback cb,
                    void *opaque)
{
    EditBufferCallbackList *l;

    l = malloc(sizeof(EditBufferCallbackList));
    if (!l)
        return -1;
    l->callback = cb;
    l->opaque = opaque;
    l->next = b->first_callback;
    b->first_callback = l;
    return 0;
}

void eb_free_callback(EditBuffer *b, EditBufferCallback cb,
                      void *opaque)
{
    EditBufferCallbackList **pl, *l;
    
    for (pl = &b->first_callback; (*pl) != NULL; pl = &(*pl)->next) {
        l = *pl;
        if (l->callback == cb && l->opaque == opaque) {
            *pl = l->next;
            free(l);
            break;
       }
    }
}

/* standard callback to move offsets */
void eb_offset_callback(EditBuffer *b,
                        void *opaque,
                        enum LogOperation op,
                        int offset,
                        int size)
{
    int *offset_ptr = opaque;

    switch (op) {
    case LOGOP_INSERT:
        if (*offset_ptr > offset)
            *offset_ptr += size;
        break;
    case LOGOP_DELETE:
        if (*offset_ptr > offset) {
            *offset_ptr -= size;
            if (*offset_ptr < offset)
                *offset_ptr = offset;
        }
        break;
    default:
        break;
    }
}



/************************************************************/
/* undo buffer */

static void eb_addlog(EditBuffer *b, enum LogOperation op, 
                      int offset, int size)
{
    int was_modified, len, size_trailer;
    LogBuffer lb;
    EditBufferCallbackList *l;

    /* call each callback */
    for (l = b->first_callback; l != NULL; l = l->next) {
        l->callback(b, l->opaque, op, offset, size);
    }

    was_modified = b->modified;
    b->modified = 1;
    if (!b->save_log)
        return;
    if (!b->log_buffer) {
        char buf[256];
        snprintf(buf, sizeof(buf), "*log <%s>*", b->name);
        b->log_buffer = eb_new(buf, BF_SYSTEM);
        if (!b->log_buffer)
            return;
    }
    /* XXX: better test to limit size */
    if (b->nb_logs >= (NB_LOGS_MAX-1)) {
        /* no free space, delete least recent entry */
        eb_read(b->log_buffer, 0, (unsigned char *)&lb, sizeof(LogBuffer));
        len = lb.size;
        if (lb.op == LOGOP_INSERT)
            len = 0;
        len += sizeof(LogBuffer) + sizeof(int);
        eb_delete(b->log_buffer, 0, len);
        b->log_new_index -= len;
        if (b->log_current > 1)
            b->log_current -= len;
        b->nb_logs--;
    }

    /* header */
    lb.op = op;
    lb.offset = offset;
    lb.size = size;
    lb.was_modified = was_modified;
    eb_write(b->log_buffer, b->log_new_index, 
             (unsigned char *) &lb, sizeof(LogBuffer));
    b->log_new_index += sizeof(LogBuffer);

    /* data */
    switch (op) {
    case LOGOP_DELETE:
    case LOGOP_WRITE:
        eb_insert_buffer(b->log_buffer, b->log_new_index, b, offset, size);
        b->log_new_index += size;
        size_trailer = size;
        break;
    default:
        size_trailer = 0;
        break;
    }
    /* trailer */
    eb_write(b->log_buffer, b->log_new_index, 
             (unsigned char *)&size_trailer, sizeof(int));
    b->log_new_index += sizeof(int);

    b->nb_logs++;
}

void do_undo(EditState *s)
{
    EditBuffer *b = s->b;
    int log_index, saved, size_trailer;
    LogBuffer lb;

    if (!b->log_buffer)
        return;

    if (s->qe_state->last_cmd_func != do_undo)
        b->log_current = 0;

    if (b->log_current == 0) {
        log_index = b->log_new_index;
    } else {
        log_index = b->log_current - 1;
    }
    if (log_index == 0) {
        put_status(s, "No futher undo information");
        return;
    } else {
        put_status(s, "Undo!");
    }
    /* go backward */
    log_index -= sizeof(int);
    eb_read(b->log_buffer, log_index, (unsigned char *)&size_trailer, sizeof(int));
    log_index -= size_trailer + sizeof(LogBuffer);
    
    /* log_current is 1 + index to have zero as default value */
    b->log_current = log_index + 1;

    /* play the log entry */
    eb_read(b->log_buffer, log_index, (unsigned char *)&lb, sizeof(LogBuffer));
    log_index += sizeof(LogBuffer);

    switch (lb.op) {
    case LOGOP_WRITE:
        /* we must disable the log because we want to record a single
           write (we should have the single operation: eb_write_buffer) */
        saved = b->save_log;
        b->save_log = 0;
        eb_delete(b, lb.offset, lb.size);
        eb_insert_buffer(b, lb.offset, b->log_buffer, log_index, lb.size);
        b->save_log = saved;
        eb_addlog(b, LOGOP_WRITE, lb.offset, lb.size);
        s->offset = lb.offset + lb.size;
        break;
    case LOGOP_DELETE:
        /* we must also disable the log there because the log buffer
           would be modified BEFORE we insert it by the implicit
           eb_addlog */
        saved = b->save_log;
        b->save_log = 0;
        eb_insert_buffer(b, lb.offset, b->log_buffer, log_index, lb.size);
        b->save_log = saved;
        eb_addlog(b, LOGOP_INSERT, lb.offset, lb.size);
        s->offset = lb.offset + lb.size;
        break;
    case LOGOP_INSERT:
        eb_delete(b, lb.offset, lb.size);
        s->offset = lb.offset;
        break;
    default:
        abort();
    }
    
    b->modified = lb.was_modified;
}

/************************************************************/
/* line related functions */

void eb_set_charset(EditBuffer *b, QECharset *charset)
{
    if (b->charset) {
        charset_decode_close(&b->charset_state);
    }
    b->charset = charset;
    charset_decode_init(&b->charset_state, charset);
}

/* XXX: change API to go faster */
int eb_nextc(EditBuffer *b, int offset, int *next_offset)
{
    u8 buf[MAX_CHAR_BYTES], *p;
    int ch;

    if (offset >= b->total_size) {
        offset = b->total_size;
        ch = '\n';
        goto Exit;
    }

    eb_read(b, offset, buf, 1);
    
    /* we use directly the charset conversion table to go faster */
    ch = b->charset_state.table[buf[0]];
    offset++;
    if (ch == ESCAPE_CHAR) {
        eb_read(b, offset, buf + 1, MAX_CHAR_BYTES - 1);
        p = buf;
        ch = b->charset_state.decode_func(&b->charset_state, 
                                          (const u8 **)&p);
        offset += (p - buf) - 1;
    }

Exit:
    if (next_offset)
        *next_offset = offset;
    return ch;
}

/* XXX: only UTF8 charset is supported */
/* XXX: suppress that */
int eb_prevc(EditBuffer *b, int offset, int *prev_offset)
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
        eb_read(b, offset, q, 1);
        if (b->charset == &charset_utf8) {
            while (*q >= 0x80 && *q < 0xc0) {
                if (offset == 0 || q == buf) {
                    /* error : take only previous char */
                    offset += buf - 1 - q;
                    ch = buf[sizeof(buf) - 1];
                    goto the_end;
                }
                offset--;
                q--;
                eb_read(b, offset, q, 1);
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

/* return the number of lines and column position for a buffer */
static void get_pos(u8 *buf, int size, int *line_ptr, int *col_ptr, 
                    CharsetDecodeState *s)
{
    u8 *p, *p1, *lp;
    int line, len, col, ch;

    QASSERT(size >= 0);

    line = 0;
    p = buf;
    lp = p;
    p1 = p + size;
    for (;;) {
        p = memchr(p, '\n', p1 - p);
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

int eb_goto_pos(EditBuffer *b, int line1, int col1)
{
    Page *p, *p_end;
    int line2, col2, line, col, offset, offset1;
    u8 *q, *q_end;
    Pages *pages;

    line = 0;
    col = 0;
    offset = 0;
    pages = &b->pages;
    p = pages->page_table;
    p_end = pages->page_table + pages->nb_pages;
    while (p < p_end) {
        if (!(p->flags & PG_VALID_POS)) {
            p->flags |= PG_VALID_POS;
            get_pos(p->data, p->size, &p->nb_lines, &p->col, 
                    &b->charset_state);
        }
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
                q = memchr(q, '\n', q_end - q);
                q++;
                line++;
            }
            /* test if we want to go after the end of the line */
            offset += q - p->data;
            while (col < col1 && eb_nextc(b, offset, &offset1) != '\n') {
                col++;
                offset = offset1;
            }
            return offset;
        }
        line = line2;
        col = col2;
        offset += p->size;
        p++;
    }
    return b->total_size;
}
        
int eb_get_pos(EditBuffer *b, int *line_ptr, int *col_ptr, int offset)
{
    Page *p, *p_end;
    int line, col, line1, col1;
    Pages *pages;

    QASSERT(offset >= 0);

    line = 0;
    col = 0;
    pages = &b->pages;
    p = pages->page_table;
    p_end = p + pages->nb_pages;
    for (;;) {
        if (p >= p_end)
            goto the_end;
        if (offset < p->size)
            break;
        if (!(p->flags & PG_VALID_POS)) {
            p->flags |= PG_VALID_POS;
            get_pos(p->data, p->size, &p->nb_lines, &p->col, 
                    &b->charset_state);
        }
        line += p->nb_lines;
        if (p->nb_lines)
            col = 0;
        col += p->col;
        offset -= p->size;
        p++;
    }
    get_pos(p->data, offset, &line1, &col1, &b->charset_state);
    line += line1;
    if (line1)
        col = 0;
    col += col1;
 the_end:
    *line_ptr = line;
    *col_ptr = col;
    return line;
}

/************************************************************/
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

/* gives the byte offset of a given character, taking the charset into
   account */
int eb_goto_char(EditBuffer *b, int pos)
{
    int offset;
    Page *p, *p_end;
    Pages *pages;

    if (b->charset != &charset_utf8) {
        offset = pos;
        if (offset > b->total_size)
            offset = b->total_size;
    } else {
        offset = 0;
        pages = &b->pages;
        p = pages->page_table;
        p_end = pages->page_table + pages->nb_pages;
        while (p < p_end) {
            if (!(p->flags & PG_VALID_CHAR)) {
                p->flags |= PG_VALID_CHAR;
                p->nb_chars = get_chars(p->data, p->size, b->charset);
            }
            if (pos < p->nb_chars) {
                offset += goto_char(p->data, pos, b->charset);
                break;
            } else {
                pos -= p->nb_chars;
                offset += p->size;
                p++;
            }
        }
    }
    return offset;
}

/* get the char offset corresponding to a given byte offset, taking
   the charset into account */
int eb_get_char_offset(EditBuffer *b, int offset)
{
    int pos;
    Page *p, *p_end;
    Pages *pages;

    /* if no decoding function in charset, it means it is 8 bit only */
    if (b->charset_state.decode_func == NULL) {
        pos = offset;
        if (pos > b->total_size)
            pos = b->total_size;
    } else {
        pages = &b->pages;
        p = pages->page_table;
        p_end = p + pages->nb_pages;
        pos = 0;
        for (;;) {
            if (p >= p_end)
                goto the_end;
            if (offset < p->size)
                break;
            if (!(p->flags & PG_VALID_CHAR)) {
                p->nb_chars = get_chars(p->data, p->size, b->charset);
                p->flags |= PG_VALID_CHAR;
            }
            pos += p->nb_chars;
            offset -= p->size;
            p++;
        }
        pos += get_chars(p->data, offset, b->charset);
    the_end: ;
    }
    return pos;
}

/************************************************************/
/* buffer I/O */

#define IOBUF_SIZE 32768

#if 0

typedef struct BufferIOState {
    URLContext *handle;
    void (*progress_cb)(void *opaque, int size);
    void (*completion_cb)(void *opaque, int err);
    void *opaque;
    int offset;
    int saved_flags;
    int saved_log;
    int nolog;
    unsigned char buffer[IOBUF_SIZE];
} BufferIOState;

static void load_connected_cb(void *opaque, int err);
static void load_read_cb(void *opaque, int size);
static void eb_io_stop(EditBuffer *b, int err);

/* load a buffer asynchronously and launch the callback. The buffer
   stays in 'loading' state while begin loaded. It is also marked
   readonly. */
int load_buffer(EditBuffer *b, const char *filename, 
                int offset, int nolog,
                void (*progress_cb)(void *opaque, int size), 
                void (*completion_cb)(void *opaque, int err), void *opaque)
{
    URLContext *h;
    BufferIOState *s;
    
    /* cannot load a buffer if already I/Os or readonly */
    if (b->flags & (BF_LOADING | BF_SAVING | BF_READONLY))
        return -1;
    s = malloc(sizeof(BufferIOState));
    if (!s)
        return -1;
    b->io_state = s;
    h = url_new();
    if (!h) {
        free(b->io_state);
        b->io_state = NULL;
        return -1;
    }
    s->handle = h;
    s->saved_flags = b->flags;
    s->nolog = nolog;
    if (s->nolog) {
        s->saved_log = b->save_log;
        b->save_log = 0;
    }
    b->flags |= BF_LOADING | BF_READONLY;
    s->handle = h;
    s->progress_cb = progress_cb;
    s->completion_cb = completion_cb;
    s->opaque = opaque;
    s->offset = offset;
    printf("connect_async: '%s'\n", filename);
    url_connect_async(s->handle, filename, URL_RDONLY,
                      load_connected_cb, b);
    return 0;
}

static void load_connected_cb(void *opaque, int err)
{
    EditBuffer *b = opaque;
    BufferIOState *s = b->io_state;
    printf("connect_cb: err=%d\n", err);
    if (err) {
        eb_io_stop(b, err);
        return;
    }
    url_read_async(s->handle, s->buffer, IOBUF_SIZE, load_read_cb, b);
}

static void load_read_cb(void *opaque, int size)
{
    EditBuffer *b = opaque;
    BufferIOState *s = b->io_state;

    printf("read_cb: size=%d\n", size);
    if (size < 0) {
        eb_io_stop(b, -EIO);
    } else if (size == 0) {
        /* end of file */
        eb_io_stop(b, 0);
    } else {
        eb_insert(b, s->offset, s->buffer, size);
        s->offset += size;
        /* launch next read request */
        url_read_async(s->handle, s->buffer, IOBUF_SIZE, load_read_cb, b);
    }
}

static void eb_io_stop(EditBuffer *b, int err)
{
    BufferIOState *s = b->io_state;

    b->flags = s->saved_flags;
    if (s->nolog) {
        b->modified = 0;
        b->save_log = s->saved_log;
    }
    url_close(s->handle);
    s->completion_cb(s->opaque, err);
    free(s);
    b->io_state = NULL;
}
#endif

int raw_load_buffer1(EditBuffer *b, FILE *f, int offset)
{
    int len;
    unsigned char buf[IOBUF_SIZE];

    for (;;) {
        len = fread(buf, 1, IOBUF_SIZE, f);
        if (len < 0)
            return -1;
        if (len == 0)
            break;
        eb_insert(b, offset, buf, len);
        offset += len;
    }
    return 0;
}

 void display_error(void)
{
#ifdef WIN32
    char *msgBuf = NULL;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &msgBuf, 0, NULL);
    printf("%s\n", msgBuf);
    LocalFree(msgBuf);
#endif
}

int mmap_buffer(EditBuffer *b, const char *filename)
{
    int len, file_size, n, size;
    u8 *file_ptr, *ptr;
    Page *p;
#ifdef WIN32
    HANDLE file_handle;
    HANDLE file_mapping;
    MEMORY_BASIC_INFORMATION mem_info;
#else
    int file_handle;
#endif

#ifdef WIN32
    file_handle = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (INVALID_HANDLE_VALUE == file_handle)
        return -1;
    file_mapping = CreateFileMapping(file_handle, NULL, PAGE_READONLY, 0, 0, NULL);
    if (NULL == file_mapping) {
        display_error();
        CloseHandle(file_handle);
        return -1;
    }
    file_ptr = (u8*)MapViewOfFile(file_mapping, FILE_MAP_READ, 0, 0, 0); /* map the whole file */
    if (NULL == file_ptr) {
        display_error();
        CloseHandle(file_handle);
        CloseHandle(file_mapping);
        return -1;
    }
    size = (int)VirtualQuery((void*)file_ptr, &mem_info, sizeof(mem_info));
    assert(size == sizeof(mem_info));
    file_size = (int)mem_info.RegionSize;
    
#else
    file_handle = open(filename, O_RDONLY);
    if (file_handle < 0)
        return -1;
    file_size = lseek(file_handle, 0, SEEK_END);
    file_ptr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, file_handle, 0);
    if ((void*)file_ptr == MAP_FAILED) {
        close(file_handle);
        return -1;
    }
#endif
    n = (file_size + MAX_PAGE_SIZE - 1) / MAX_PAGE_SIZE;
    p = malloc(n * sizeof(Page));
    if (!p) {
#ifdef WIN32
        UnmapViewOfFile((void*)file_ptr);
        CloseHandle(file_handle);
        CloseHandle(file_mapping);
#else
        close(file_handle);
#endif
        return -1;
    }
    b->pages.page_table = p;
    b->total_size = file_size;
    b->pages.nb_pages = n;
    size = file_size;
    ptr = file_ptr;
    while (size > 0) {
        len = size;
        if (len > MAX_PAGE_SIZE)
            len = MAX_PAGE_SIZE;
        p->data = ptr;
        p->size = len;
        p->flags = PG_READ_ONLY;
        ptr += len;
        size -= len;
        p++;
    }
    b->file_handle = file_handle;
#ifdef WIN32
    b->file_mapping = file_mapping;
#endif
    return 0;
}

static int raw_load_buffer(EditBuffer *b, FILE *f)
{
    int ret;
    struct stat st;

    if (stat(b->filename, &st) == 0 &&
        st.st_size >= MIN_MMAP_SIZE) {
        ret = mmap_buffer(b, b->filename);
    } else {
        ret = raw_load_buffer1(b, f, 0);
    }
    return ret;
}

static int raw_save_buffer(EditBuffer *b, const char *filename)
{
    int fd, len, size;
    unsigned char buf[IOBUF_SIZE];

    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;

    size = b->total_size;
    while (size > 0) {
        len = size;
        if (len > IOBUF_SIZE)
            len = IOBUF_SIZE;
        eb_read(b, b->total_size - size, buf, len);
        len = write(fd, buf, len);
        if (len < 0) {
            close(fd);
            return -1;
        }
        size -= len;
    }
    close(fd);
    return 0;
}

static void raw_close_buffer(EditBuffer *b)
{
    /* nothing to do */
}

/* Associate a buffer with a file and rename it to match the
   filename. Find a unique buffer name */
void set_filename(EditBuffer *b, const char *filename)
{
    const char *p;

    pstrcpy(b->filename, sizeof(b->filename), filename);
    p = basename(filename);
    set_buffer_name(b, p);
}

void eb_printf(EditBuffer *b, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    eb_insert(b, b->total_size, buf, len);
}

/* pad current line with spaces so that it reaches column n */
void eb_line_pad(EditBuffer *b, int n)
{
    int offset, i;
    i = 0;
    offset = b->total_size;
    for (;;) {
        if (eb_prevc(b, offset, &offset) == '\n')
            break;
        i++;
    }
    while (i < n) {
        eb_printf(b, " ");
        i++;
    }
}

int eb_get_str(EditBuffer *b, char *buf, int buf_size)
{
    int len;

    len = b->total_size;
    if (len > buf_size - 1)
        len = buf_size - 1;
    eb_read(b, 0, buf, len);
    buf[len] = '\0';
    return len;
}

/* get the line starting at offset 'offset' */
int eb_get_line(EditBuffer *b, unsigned int *buf, int buf_size,
                int *offset_ptr)
{
    int c;
    unsigned int *buf_ptr, *buf_end;
    int offset;
    
    offset = *offset_ptr;

    /* record line */
    buf_ptr = buf;
    buf_end = buf + buf_size;
    for (;;) {
        c = eb_nextc(b, offset, &offset);
        if (c == '\n')
            break;
        if (buf_ptr < buf_end)
            *buf_ptr++ = c;
    }
    *offset_ptr = offset;
    return buf_ptr - buf;
}

/* get the line starting at offset 'offset' */
/* XXX: incorrect for UTF8 */
int eb_get_strline(EditBuffer *b, char *buf, int buf_size,
                   int *offset_ptr)
{
    int c;
    char *buf_ptr, *buf_end;
    int offset;
    
    offset = *offset_ptr;

    /* record line */
    buf_ptr = buf;
    buf_end = buf + buf_size - 1;
    for (;;) {
        c = eb_nextc(b, offset, &offset);
        if (c == '\n')
            break;
        if (buf_ptr < buf_end)
            *buf_ptr++ = c;
    }
    *buf_ptr = '\0';
    *offset_ptr = offset;
    return buf_ptr - buf;
}

int eb_goto_bol(EditBuffer *b, int offset)
{
    int c, offset1;

    for (;;) {
        c = eb_prevc(b, offset, &offset1);
        if (c == '\n')
            break;
        offset = offset1;
    }
    return offset;
}

int eb_is_empty_line(EditBuffer *b, int offset)
{
    int c;

    for (;;) {
        c = eb_nextc(b, offset, &offset);
        if (c == '\n')
            return 1;
        if (!isspace(c))
            break;
    }
    return 0;
}

int eb_is_empty_from_to(EditBuffer *b, int offset_start, int offset_end)
{
    int c;
    int offset = offset_start;

    for (;;) {
        if (offset >= offset_end)
            return 1;
        c = eb_nextc(b, offset, &offset);
        if (c == '\n')
            return 1;
        if (!isspace(c))
            break;
    }
    return 0;
}

int eb_next_line(EditBuffer *b, int offset)
{
    int c;
    for (;;) {
        c = eb_nextc(b, offset, &offset);
        if (c == '\n')
            break;
    }
    return offset;
}

/* buffer data type handling */

void eb_register_data_type(EditBufferDataType *bdt)
{
    EditBufferDataType **lp;

    lp = &first_buffer_data_type;
    while (*lp != NULL)
        lp = &(*lp)->next;
    bdt->next = NULL;
    *lp = bdt;
}

/* how we do backups of edited files. */
#define BACKUP_NONE 0   /* no backup */
#define BACKUP_TILDE 1  /* standard emacs way: append ~ to the name of the file */
#define BACKUP_DIR 2    /* save to backup_dir directory */

int backup_method = BACKUP_DIR;

#define APP_NAME "qemacs"
#define BAK_DIR_NAME ".backup"

#ifdef WIN32
#include <windows.h>
#include <shlobj.h>
int get_backup_dir(char *buf, int buf_len)
{
    static int created = 0;
    static char backup_dir[MAX_FILENAME_SIZE] = {0};
    int ret;

    if (!created)
    {
        if (!SHGetSpecialFolderPath(NULL, backup_dir, CSIDL_APPDATA, TRUE))
            return 0;

        if (0 == strlen(backup_dir))
            return 0;

        if (backup_dir[strlen(backup_dir)-1] != DIR_SEP_CHAR)
            pstrcat(backup_dir, MAX_FILENAME_SIZE, DIR_SEP_STR);
        pstrcat(backup_dir, MAX_FILENAME_SIZE, APP_NAME);
        pstrcat(backup_dir, MAX_FILENAME_SIZE, DIR_SEP_STR);
        pstrcat(backup_dir, MAX_FILENAME_SIZE, BAK_DIR_NAME);
        /* create the directory, including all it's subdirectories */
        ret = SHCreateDirectoryEx(NULL, backup_dir, NULL);
        if (! ((ERROR_SUCCESS == ret) || (ERROR_FILE_EXISTS == ret) || (ERROR_ALREADY_EXISTS == ret))) {
            return 0;
        }
        pstrcat(backup_dir, MAX_FILENAME_SIZE, DIR_SEP_STR);
    }

    created = 1;
    pstrcpy(buf, buf_len, backup_dir);
    return 1;
}
#else
int get_backup_dir(char *buf, int buf_len)
{
    /* TODO: on Unix should this be e.g. ~/.qebak/ ? */
    return 0;
}
#endif

/* return the name of a backup file for a buffer 'b' in 'backup_name_out' of
   'backup_name_len' max size.
   The name depends on currently set backup method.
   Return 0 (FALSE) if failed and file should not be backed up. */
int get_backup_name(EditBuffer *b, char *backup_name_out, int backup_name_len)
{
    const char *  base;

    if (BACKUP_NONE == backup_method)
        return 0;

    if (BACKUP_TILDE == backup_method) {
        pstrcpy(backup_name_out, backup_name_len, b->filename);
        pstrcat(backup_name_out, backup_name_len, "~");
        return 1;
    }

    if (BACKUP_DIR == backup_method) {
        /* TODO: if we edit a file with the same name but from different directories,
           then this won't work well. Not sure how to fix that, though */
        if (!get_backup_dir(backup_name_out, backup_name_len))
            return 0;
        base = basename(b->filename);
        pstrcat(backup_name_out, backup_name_len, base);
        return 1;
    }
    return 0;
}

/*
 * save buffer according to its data type
 */
int save_buffer(EditBuffer *b)
{
    int ret, mode;
    char backup_name[MAX_FILENAME_SIZE];
    const char *filename;
    struct stat st;

    if (!b->data_type->buffer_save)
        return -1;

    filename = b->filename;
    /* get old file permission */
    mode = 0644;
    if (stat(filename, &st) == 0)
        mode = st.st_mode & 0777;

    /* backup old file if present */
    if (get_backup_name(b, backup_name, MAX_FILENAME_SIZE))
        rename(filename, backup_name);

    ret = b->data_type->buffer_save(b, filename);
    if (ret < 0)
        return ret;

#ifndef WIN32
    /* set correct file mode to old file permissions */
    chmod(filename, mode);
#endif
    /* reset log */
    eb_log_reset(b);
    b->modified = 0;
    return 0;
}

/* invalidate buffer raw data */
void eb_invalidate_raw_data(EditBuffer *b)
{
    b->save_log = 0;
    eb_delete(b, 0, b->total_size);
    eb_log_reset(b);
}

EditBufferDataType raw_data_type = {
    "raw",
    raw_load_buffer,
    raw_save_buffer,
    raw_close_buffer,
};

/* init buffer handling */
void eb_init(void)
{
    eb_register_data_type(&raw_data_type);
}
