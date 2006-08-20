#ifndef STRBUF_H_
#define STRBUF_H_

/*
Author: Krzysztof Kowalczyk.

This code is in Public Domain. Take all the code you want, we'll
just write more.
*/

typedef struct strbuf {
    int     cur_size;
    int     allocated;
    char *  data;
} strbuf;

strbuf *  strbuf_new(void);
strbuf *  strbuf_new_with_size(int size);
void      strbuf_free(strbuf *buf);
void      strbuf_reset(strbuf *buf);
int       strbuf_reserve(strbuf *buf, int size);
int       strbuf_appendc(strbuf *buf, char c);
int       strbuf_append(strbuf *buf, char *data, int len);
int       strbuf_appendf(strbuf *buf, const char *msg, ...);
char *    strbuf_getstr(strbuf *buf);

#endif
