/* 
A very simple growable buffer. Especially good for C strings since the string
inside is always zero-terminated.

Author: Krzysztof Kowalczyk.

This code is in Public Domain. Take all the code you want, we'll
just write more.
*/

#include "strbuf.h"

#include <assert.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef WIN32
#define vsnprintf _vsnprintf
#endif

strbuf *strbuf_new(void)
{
    return strbuf_new_with_size(0);
}

strbuf *strbuf_new_with_size(int size)
{
    strbuf *buf = (strbuf*)malloc(sizeof(strbuf));
    if (!buf)
        return NULL;
    buf->cur_size = 0;
    buf->data = NULL;
    buf->allocated = size;
    if (0 == size)
        return buf;
    buf->data = (char*)malloc(size);
    if (!buf->data) {
        free((void*)buf);
        return NULL;
    }
    buf->data[0] = 0;
    return buf;
}

void strbuf_free(strbuf *buf)
{
    if (buf)
        free(buf->data);
    free(buf);
}

void strbuf_reset(strbuf *buf)
{
    if (!buf || !buf->data)
        return;
    buf->data[0] = 0;
    buf->cur_size = 0;
}

char *strbuf_getstr(strbuf *buf)
{
    if (!buf)
        return NULL;
    return buf->data;
}

/* make sure that 'buf' is of size at least 'size' + 1 (for terminating zero).
   Reallocates if necessary.
   Returns 0 (FALSE) if reallocation failed */
int strbuf_reserve(strbuf *buf, int size)
{
    char *  new_data;

    ++size;

    if (buf->allocated >= size)
        return 1;

    new_data = (char*)malloc(size);
    if (!new_data)
        return 0;
    if (buf->data) {
        memcpy(new_data, buf->data, buf->allocated);
        free(buf->data);
    }
    buf->allocated = size;
    buf->data = new_data;
    return 1;    
}

/* insert 'data' of size 'len' at position 'pos'. 
Return 0 (FALSE) if something failed. */
int strbuf_insert(strbuf *buf, int pos, char *data, int len)
{
    int to_move;
    if (!buf || (pos > buf->cur_size) || (len < 0))
        return 0;

    if (0 == len)
        return 1;

    if (!strbuf_reserve(buf, buf->cur_size + len))
        return 0;

    assert(buf->allocated - buf->cur_size - 1 >= len);
    to_move = buf->cur_size - pos;
    if (to_move > 0)
        memmove(buf->data + pos + len, buf->data + pos, to_move);

    memcpy(buf->data+pos, data, len);
    buf->cur_size += len;
    buf->data[buf->cur_size] = 0;
    return 1;
}

/* append 'data' of size 'len' at the end of the buffer. Return
0 (FALSE) if something failed. */
int strbuf_append(strbuf *buf, char *data, int len)
{
    return strbuf_insert(buf, buf->cur_size, data, len);
}

int strbuf_appendc(strbuf *buf, char c)
{
    char data[1];
    data[0] = c;
    return strbuf_insert(buf, buf->cur_size, data, 1);
}
#define BUF_SIZE 128
int strbuf_appendf(strbuf *str_buf, const char *fmt, ...)
{
    va_list args;
    char    static_buf[BUF_SIZE];
    char *  buf = static_buf;
    int     buf_len = BUF_SIZE;
    int     len_needed;
    int     ok;

    for (;;) {
        va_start(args, fmt);
        len_needed = vsnprintf(buf, buf_len, fmt, args);
        va_end(args);

        if (len_needed < buf_len)
            break;

        /* here we now that the buffer wasn't big enough, we need to reallocate. */
        if (-1 == len_needed)
        {
            /* account for the bug in some versions of vsnprintf that return -1
               and not number of characters needed. In that case blindly grow
               buffer. */
            len_needed = buf_len + 1024;
        }

        buf_len = len_needed;
        if (buf != static_buf)
            free(buf);
        buf = (char*)malloc(buf_len);
        if (!buf)
            return 0;
    }

    ok = strbuf_append(str_buf, buf, len_needed);
    assert(ok);
    if (buf != static_buf)
        free(buf);
    return ok;
}
