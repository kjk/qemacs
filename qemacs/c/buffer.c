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

/* the log buffer is used for the undo operation */
/* header of log operation */
typedef struct LogBuffer {
    u8 op;
    u8 was_modified;
    int offset;
    int size;
} LogBuffer;

static void eb_addlog(EditBuffer *b, enum LogOperation op, 
                      int offset, int size);

extern EditBufferDataType raw_data_type;

EditBufferDataType *first_buffer_data_type = NULL;

/************************************************************/
/* basic access to the edit buffer */

/* Read or write in the buffer. We must have 0 <= offset < b->total_size */
static int eb_rw(EditBuffer *b, int offset, u8 *buf, int size, int do_write)
{
    size = b->pages.LimitSize(offset, size);
    if (size > 0) {
        if (do_write)
            eb_addlog(b, LOGOP_WRITE, offset, size);

        b->pages.ReadWrite(offset, buf, size, do_write);
    }
    return size;
}

/* We must have: 0 <= offset < b->total_size */
int eb_read(EditBuffer *b, int offset, void *buf, int size)
{
    return b->pages.Read(offset, (u8*)buf, size);
}

/* Note: eb_write can be used to insert after the end of the buffer */
void eb_write(EditBuffer *b, int offset, void *buf1, int size)
{
    u8 *buf = (u8*)buf1;
    int len = eb_rw(b, offset, buf, size, 1);
    int left = size - len;
    if (left > 0) {
        offset += len;
        buf += len;
        eb_insert(b, offset, buf, left);
    }
}

/* Insert 'size bytes of 'src' buffer from position 'src_offset' into
   buffer 'dest' at offset 'dest_offset'. 'src' MUST BE DIFFERENT from
   'dest' */
void eb_insert_buffer(EditBuffer *dest, int dest_offset, 
                      EditBuffer *src, int src_offset, 
                      int size)
{
    if (size == 0)
        return;

    eb_addlog(dest, LOGOP_INSERT, dest_offset, size);
    dest->pages.InsertFrom(dest_offset, &src->pages, src_offset, size);
}

/* Insert 'size' bytes from 'buf' into 'b' at offset 'offset'. We must
   have : 0 <= offset <= b->total_size */
void eb_insert(EditBuffer *b, int offset, const void *buf, int size)
{
    eb_addlog(b, LOGOP_INSERT, offset, size);
    b->pages.InsertLowLevel(offset, (const u8*)buf, size);
}

/* Append 'size' bytes from 'buf' at the end of 'b' */
void eb_append(EditBuffer *b, const void *buf, int size)
{
    eb_insert(b, eb_total_size(b), buf, size);
}

/* We must have : 0 <= offset <= b->total_size */
void eb_delete(EditBuffer *b, int offset, int size)
{
    if (offset >= eb_total_size(b))
        return;

    eb_addlog(b, LOGOP_DELETE, offset, size);
    b->pages.Delete(offset, size);
}

/* flush the log */
void eb_log_reset(EditBuffer *b)
{
    b->modified = 0;
    if (!b->log_buffer)
        return;
    eb_free(b->log_buffer);
    b->log_buffer = NULL;
    b->log_new_index = 0;
    b->log_current = 0;
    b->nb_logs = 0;
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
    EditBuffer *b = new EditBuffer();

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

void eb_free_callbacks(EditBuffer *b)
{
    EditBufferCallbackList *l, *l1;
    for (l = b->first_callback; l != NULL;) {
        l1 = l->next;
        free(l);
        l = l1;
    }
    b->first_callback = NULL;
}

void eb_free(EditBuffer *b)
{
    QEmacsState *qs = &qe_state;
    EditBuffer **pb;

    /* call user defined close */
    if (b->close)
        b->close(b);

    eb_free_callbacks(b);

    b->save_log = 0;
    eb_delete(b, 0, eb_total_size(b));
    eb_log_reset(b);
    free(b->saved_data);

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

    delete b;
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

/* callbacks */

int eb_add_callback(EditBuffer *b, EditBufferCallback cb, void *opaque)
{
    EditBufferCallbackList *l;

    l = (EditBufferCallbackList*)malloc(sizeof(EditBufferCallbackList));
    if (!l)
        return -1;
    l->callback = cb;
    l->opaque = opaque;
    l->next = b->first_callback;
    b->first_callback = l;
    return 0;
}

void eb_free_callback(EditBuffer *b, EditBufferCallback cb, void *opaque)
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

class IEditBufferCallback {
public:
    virtual void cb(EditBuffer *b, enum LogOperation op, int offset, int size) = 0;
};

class OffsetCallback : IEditBufferCallback {
public:
    int *offset_ptr;

    OffsetCallback(int *offset_ptr) {
        this->offset_ptr = offset_ptr;
    }

    virtual void cb(EditBuffer *b, enum LogOperation op, int offset, int size);
};

void OffsetCallback::cb(EditBuffer *b, enum LogOperation op, int offset, int size)
{
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

/* standard callback to move offsets */
void eb_offset_callback(EditBuffer *b,
                        void *opaque,
                        enum LogOperation op,
                        int offset,
                        int size)
{
    int *offset_ptr = (int*)opaque;

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

static void eb_limit_log_size(EditBuffer *b)
{
    LogBuffer lb;
    int len;

    /* XXX: better test to limit size */
    if (b->nb_logs < NB_LOGS_MAX-1)
        return;

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

static void eb_addlog(EditBuffer *b, enum LogOperation op, 
                      int offset, int size)
{
    int was_modified, size_trailer;
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

    eb_limit_log_size(b);

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
    return b->pages.NextChar(&b->charset_state, offset, next_offset);
}

/* XXX: only UTF8 charset is supported */
/* XXX: suppress that */
int eb_prevc(EditBuffer *b, int offset, int *prev_offset)
{
    return b->pages.PrevChar(b->charset, offset, prev_offset);
}

int eb_goto_pos(EditBuffer *b, int line1, int col1)
{
    return b->pages.GotoPos(&b->charset_state, line1, col1);
}

int eb_get_pos(EditBuffer *b, int *line_ptr, int *col_ptr, int offset)
{
    return b->pages.GetPos(&b->charset_state, line_ptr, col_ptr, offset);
}

/* gives the byte offset of a given character, taking the charset into
   account */
int eb_goto_char(EditBuffer *b, int pos)
{
    int offset;
    if (b->charset != &charset_utf8) {
        offset = pos;
        if (offset > eb_total_size(b))
            offset = eb_total_size(b);
    } else {
        offset = b->pages.GotoChar(b->charset, pos);
    }
    return offset;
}

/* get the char offset corresponding to a given byte offset, taking
   the charset into account */
int eb_get_char_offset(EditBuffer *b, int offset)
{
    int pos;

    /* if no decoding function in charset, it means it is 8 bit only */
    if (b->charset_state.decode_func == NULL) {
        pos = offset;
        if (pos > eb_total_size(b))
            pos = eb_total_size(b);
    } else {
        pos = b->pages.GetCharOffset(offset, b->charset);
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
    PtrVec<Page> *pages = new PtrVec<Page>(n);
    Page **parr = pages->MakeSpaceAt(0, n);
    if (!parr) {
#ifdef WIN32
        UnmapViewOfFile((void*)file_ptr);
        CloseHandle(file_handle);
        CloseHandle(file_mapping);
#else
        close(file_handle);
#endif
        return -1;
    }
    b->pages.page_table = pages;
    b->pages.total_size = file_size;
    size = file_size;
    ptr = file_ptr;
    while (size > 0) {
        len = size;
        if (len > MAX_PAGE_SIZE)
            len = MAX_PAGE_SIZE;
        Page *p = new Page();
        p->data = ptr;
        p->size = len;
        p->read_only = 1;
        ptr += len;
        size -= len;
        *parr++ = p;
        --n;
    }
    assert(n == 0);
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

    size = eb_total_size(b);
    while (size > 0) {
        len = size;
        if (len > IOBUF_SIZE)
            len = IOBUF_SIZE;
        eb_read(b, eb_total_size(b) - size, buf, len);
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
    eb_insert(b, eb_total_size(b), buf, len);
}

/* pad current line with spaces so that it reaches column n */
void eb_line_pad(EditBuffer *b, int n)
{
    int offset, i;
    i = 0;
    offset = eb_total_size(b);
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

    len = eb_total_size(b);
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
    eb_delete(b, 0, eb_total_size(b));
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
