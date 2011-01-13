/*
 * Copyright Metaparadigm Pte. Ltd. 2004.
 * Michael Clark <michael@metaparadigm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public (LGPL)
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details: http://www.gnu.org/
 *
 */

#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>

#include "strbuf.h"

#if STDC_HEADERS
# include <stdlib.h>
# include <string.h>
#endif /* STDC_HEADERS */

#if !(defined(_MSC_VER))
# include <strings.h>
#endif

#if HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */

#if defined(_MSC_VER)
# define strncasecmp _strnicmp
#endif

#define hexdigit(x) (((x) <= '9') ? (x) - '0' : ((x) & 7) + 9)

int serialize_generic(json_object *me, strbuf *buf);

#if !HAVE_STRNDUP
char* strndup(const char* str, size_t n)
{
    size_t len;
    char * s;

    if (!str)
        return NULL;
    len = strlen(str);
    if (n < len)
        len = n;
    s = (char*)malloc(sizeof(char) * (len + 1));
    if (!s)
        return NULL;

    memcpy(s, str, len);
    s[len] = '\0';
    return s;
}
#endif

struct array_list*
array_list_new(array_list_free_fn *free_fn)
{
    struct array_list *me;

    me = (array_list*)calloc(1, sizeof(struct array_list));
    if (!me) 
        return NULL;
    me->size = ARRAY_LIST_DEFAULT_SIZE;
    me->length = 0;
    me->free_fn = free_fn;
    me->array = (void**)calloc(sizeof(void*), me->size);
    if (!me->array) {
        free(me);
        return NULL;
    }
    return me;
}

void
array_list_free(struct array_list *me)
{
    int i;
    for (i = 0; i < me->length; i++)
    {
        if (me->array[i]) 
            me->free_fn(me->array[i]);
    }
    free(me->array);
    free(me);
}

void*
array_list_get_idx(struct array_list *me, int i)
{
    if(i >= me->length) 
        return NULL;
    return me->array[i];
}

static int array_list_expand_internal(struct array_list *me, int max)
{
    void **t;
    int new_size;

    if (max < me->size) 
        return 0;
    new_size = max;
    if (me->size << 1 > max)
        new_size = me->size << 1;
    t = (void**)realloc(me->array, new_size*sizeof(void*));
    if (!t) 
        return -1;
    me->array = t;
    memset(me->array + me->size, 0, (new_size-me->size)*sizeof(void*));
    me->size = new_size;
    return 0;
}

int array_list_put_idx(struct array_list *me, int idx, void *data)
{
    if (array_list_expand_internal(me, idx)) 
        return -1;
    if (me->array[idx]) 
        me->free_fn(me->array[idx]);
    me->array[idx] = data;
    if (me->length <= idx) 
        me->length = idx + 1;
    return 0;
}

int array_list_add(struct array_list *me, void *data)
{
  return array_list_put_idx(me, me->length, data);
}

int array_list_length(struct array_list *me)
{
  return me->length;
}

/** LINKHASH.C */

/**
 * golden prime used in hash functions
 */
#define LH_PRIME 0x9e370001UL

/**
 * sentinel pointer value for empty slots
 */
#define LH_EMPTY (void*)-1

/**
 * sentinel pointer value for freed slots
 */
#define LH_FREED (void*)-2

unsigned long lh_ptr_hash(void *k)
{
    return (unsigned long)((((ptrdiff_t)k * LH_PRIME) >> 4) & ULONG_MAX);
}

int lh_ptr_equal(void *k1, void *k2)
{
    return (k1 == k2);
}

unsigned long lh_char_hash(void *k)
{
    unsigned int h = 0;
    const char* data = (const char*)k;

    while( *data ) 
        h = h*129 + (unsigned int)(*data++) + LH_PRIME;

    return h;
}

int lh_char_equal(void *k1, void *k2)
{
    return (strcmp((char*)k1, (char*)k2) == 0);
}

lh_table* lh_table_new(int size, char *name,
                              lh_entry_free_fn *free_fn,
                              lh_hash_fn *hash_fn,
                              lh_equal_fn *equal_fn)
{
    int i;
    lh_table *t;

    t = (lh_table*)calloc(1, sizeof(lh_table));
    if (!t) 
        return NULL;
    t->count = 0;
    t->size = size;
    t->name = name;
    t->table = (lh_entry*)calloc(size, sizeof(lh_entry));
    if (!t->table) 
    {
        free(t);
        return NULL;
    }
    t->free_fn = free_fn;
    t->hash_fn = hash_fn;
    t->equal_fn = equal_fn;
    for (i = 0; i < size; i++) 
        t->table[i].key = LH_EMPTY;
    return t;
}

lh_table* lh_kchar_table_new(int size, char *name,
                                    lh_entry_free_fn *free_fn)
{
    return lh_table_new(size, name, free_fn, lh_char_hash, lh_char_equal);
}

lh_table* lh_kptr_table_new(int size, char *name,
                                   lh_entry_free_fn *free_fn)
{
    return lh_table_new(size, name, free_fn, lh_ptr_hash, lh_ptr_equal);
}

int lh_table_resize(lh_table *t, int new_size)
{
    lh_table *new_t;
    lh_entry *ent;

    new_t = lh_table_new(new_size, t->name, NULL, t->hash_fn, t->equal_fn);
    if (!new_t)
        return 0;
    ent = t->head;
    while (ent) {
        lh_table_insert(new_t, ent->key, ent->val);
        ent = ent->next;
    }
    free(t->table);
    t->table = new_t->table;
    t->size = new_size;
    t->head = new_t->head;
    t->tail = new_t->tail;
    t->resizes++;
    free(new_t);
    return 1;
}

void lh_table_free(lh_table *t)
{
    lh_entry *c;
    for (c = t->head; c != NULL; c = c->next) {
            if(t->free_fn) {
                    t->free_fn(c);
            }
    }
    free(t->table);
    free(t);
}

/* insert 'key'/'val' into hash table.
   Return 0 (FALSE) if failed, 1 (TRUE) if ok */
int lh_table_insert(lh_table *t, void *key, void *val)
{
    unsigned long h;
    int           n;
    int           ok;

    t->inserts++;
    if (t->count > t->size * 0.66)
    {
        ok = lh_table_resize(t, t->size * 2);
        if (!ok)
            return 0;
    }

    h = t->hash_fn(key);
    n = h % t->size;

    for (;;) {
        if (t->table[n].key == LH_EMPTY || t->table[n].key == LH_FREED) 
            break;
        t->collisions++;
        if (++n == t->size) 
            n = 0;
    }

    t->table[n].key = key;
    t->table[n].val = val;
    t->count++;

    if(t->head == NULL) {
            t->head = t->tail = &t->table[n];
            t->table[n].next = t->table[n].prev = NULL;
    } else {
            t->tail->next = &t->table[n];
            t->table[n].prev = t->tail;
            t->table[n].next = NULL;
            t->tail = &t->table[n];
    }

    return 1;
}

lh_entry* lh_table_lookup_entry(lh_table *t, void *k)
{
    unsigned long h = t->hash_fn(k);
    unsigned long n = h % t->size;

    t->lookups++;
    for(;;) {
        if (t->table[n].key == LH_EMPTY) 
            break;
        if ( (t->table[n].key != LH_FREED) && t->equal_fn(t->table[n].key, k)) 
           return &t->table[n];
        if (++n == t->size) 
            n = 0;
    }
    return NULL;
}

void* lh_table_lookup(lh_table *t, void *k)
{
    lh_entry *e = lh_table_lookup_entry(t, k);
    if(e) 
        return e->val;
    return NULL;
}

int lh_table_delete_entry(lh_table *t, lh_entry *e)
{
    ptrdiff_t n = (ptrdiff_t)(e - t->table); /* CAW: fixed to be 64bit nice, still need the crazy negative case... */

    /* CAW: this is bad, really bad, maybe stack goes other direction on this machine... */
    if(n < 0) 
        return -2;

    if (t->table[n].key == LH_EMPTY || t->table[n].key == LH_FREED) 
        return -1;
    t->count--;
    if (t->free_fn) 
        t->free_fn(e);
    t->table[n].val = NULL;
    t->table[n].key = LH_FREED;
    if (t->tail == &t->table[n] && t->head == &t->table[n]) {
            t->head = t->tail = NULL;
    } else if (t->head == &t->table[n]) {
            t->head->next->prev = NULL;
            t->head = t->head->next;
    } else if (t->tail == &t->table[n]) {
            t->tail->prev->next = NULL;
            t->tail = t->tail->prev;
    } else {
            t->table[n].prev->next = t->table[n].next;
            t->table[n].next->prev = t->table[n].prev;
    }
    t->table[n].next = t->table[n].prev = NULL;
    return 0;
}


int lh_table_delete(lh_table *t, void *k)
{
    lh_entry *e = lh_table_lookup_entry(t, k);
    if (!e) 
        return -1;
    return lh_table_delete_entry(t, e);
}

/** JSON_OBJECT.H */

#define NUMBER_CHARS "0123456789.+-e"
#define HEX_CHARS "0123456789abcdef"

static void json_generic_delete(json_object* me);
static json_object* json_new(enum json_type o_type);

/* string escaping */

static int json_escape_str(strbuf *pb, char *str)
{
    int     phase;
    int     escaped_len;
    char *  tmp;
    int     ok = 1;

    /* during phase 0 calculate the length of escaped string, during phase
       1 do the escaping */
    escaped_len = 0;
    for (phase = 0; phase < 2; phase++)
    {
        tmp = str;
        while (*tmp)
        {
            switch (*tmp)
            {
                case '\b':
                case '\n':
                case '\r':
                case '\t':
                case '"':
                    if (0 == phase)
                        escaped_len += 1;
                    else
                        if (*tmp == '\b') 
                            ok = strbuf_append(pb, "\\b", 2);
                        else if(*tmp == '\n') 
                            ok = strbuf_append(pb, "\\n", 2);
                        else if(*tmp == '\r') 
                            ok = strbuf_append(pb, "\\r", 2);
                        else if(*tmp == '\t') 
                            ok = strbuf_append(pb, "\\t", 2);
                        else if(*tmp == '"') 
                            ok = strbuf_append(pb, "\\\"", 2);
                break;
                default:
                    if (*tmp < ' ')
                    {
                        if (0 == phase)
                            escaped_len += 6;
                        else
                            ok = strbuf_appendf(pb, "\\u00%c%c",
                                      HEX_CHARS[*tmp >> 4],
                                      HEX_CHARS[*tmp & 0xf]);
                    }
                    else
                    {
                        if (0 == phase)
                            ++escaped_len;
                        else
                            ok = strbuf_appendc(pb, *tmp);
                    }
            }
            if (!ok)
                return 0;
            ++tmp;
        }
        if (0 == phase)
        {
            ok = strbuf_reserve(pb, pb->cur_size + escaped_len + 4);
            if (!ok)
                return 0;
        }
    }
    return ok;
}

json_object* json_addref(json_object *me)
{
  if (me)
    me->_ref_count++;
  return me;
}

static void json_generic_delete(json_object* me)
{
    strbuf_free(me->_pb);
    free(me);
}

static void json_delete_object(json_object *me)
{
    if (!me)
        return;

    switch (me->o_type)
    {
        case json_type_object:
            lh_table_free(me->o.c_object);
            break;
        case json_type_string:
            free(me->o.c_string);
            break;
        case json_type_array:
            array_list_free(me->o.c_array);
            break;
        default:
            break;
    }
    json_generic_delete(me);
}

/* Unref and delete if refcount drops to zero.
   Return 1 (TRUE) if was deleted, 0 (FALSE) otherwise */
int json_unref(json_object *me)
{
    if (!me)
        return 0;
    --me->_ref_count;
    if (0 == me->_ref_count)
    {
        json_delete_object(me);
        return 1;
    }
    return 0;
}

static json_object* json_new(enum json_type o_type)
{
    json_object *me = (json_object*)calloc(sizeof(json_object), 1);
    if (!me) 
        return NULL;
    me->o_type = o_type;
    me->_ref_count = 1;
    return me;
}

enum json_type json_get_type(json_object *me)
{
    return me->o_type;
}

char* json_serialize(json_object *me)
{
    if (!me->_pb) {
        me->_pb = strbuf_new();
        if(!me->_pb) 
            return NULL;
    } else {
        strbuf_reset(me->_pb);
    }

    if (!serialize_generic(me, me->_pb)) 
        return NULL;
    return me->_pb->data;
}

static void json_lh_entry_free(lh_entry *ent)
{
    free(ent->key);
    json_unref((json_object*)ent->val);
}

static void json_object_delete(json_object* me)
{
    json_generic_delete(me);
}

#define DEF_HASH_ENTIRES 16

json_object* json_new_object()
{
    json_object *me = json_new(json_type_object);
    if (!me) 
        return NULL;
    me->o.c_object = lh_kchar_table_new(DEF_HASH_ENTIRES, NULL, &json_lh_entry_free);
    if (!me->o.c_object)
    {
        free(me);
        return NULL;
    }
    return me;
}

lh_table* json_get_object(json_object *me)
{
    if (!me) 
        return NULL;

    switch (me->o_type) 
    {
        case json_type_object:
            return me->o.c_object;
        default:
            return NULL;
    }
}

/* add an 'key'/'val' to 'this' hash.
   Return 0 (FALSE) if failed, 1 (TRUE) otherwise */
int json_object_add(json_object* me, char *key, json_object *val)
{
    char *  key_dup = strdup(key);
    if (!key_dup)
        return 0;

    lh_table_delete(me->o.c_object, key);
    return lh_table_insert(me->o.c_object, key_dup, val);
}

json_object* json_object_get(json_object* me, char *key)
{
    return (json_object*) lh_table_lookup(me->o.c_object, key);
}

void json_object_del(json_object* me, char *key)
{
    lh_table_delete(me->o.c_object, key);
}

/* json_boolean */
json_object* json_new_boolean(int b)
{
    json_object *me = json_new(json_type_boolean);
    if (!me) 
        return NULL;
    me->o.c_boolean = b;
    return me;
}

int json_get_boolean(json_object *me)
{
    if (!me) 
        return 0;

    switch (me->o_type) 
    {
        case json_type_boolean:
            return me->o.c_boolean;
        case json_type_int:
            return (me->o.c_int != 0);
        case json_type_double:
            return (me->o.c_double != 0);
        case json_type_string:
            if (strlen(me->o.c_string)) 
                return 1;
        default:
            return 1;
    }
}

/* json_int */

json_object* json_new_int(int i)
{
  json_object *me = json_new(json_type_int);
  if (!me) 
      return NULL;
  me->o.c_int = i;
  return me;
}

int json_get_int(json_object *me)
{
    int cint;

    if (!me) 
        return 0;

    switch(me->o_type) 
    {
        case json_type_int:
            return me->o.c_int;
        case json_type_double:
            return (int)me->o.c_double;
        case json_type_boolean:
            return me->o.c_boolean;
        case json_type_string:
            if (sscanf(me->o.c_string, "%d", &cint) == 1) 
                return cint;
        default:
            return 0;
    }
}

/* json_double */
json_object* json_new_double(double d)
{
    json_object *me = json_new(json_type_double);
    if (!me) 
        return NULL;
    me->o.c_double = d;
    return me;
}

double json_get_double(json_object *me)
{
    double cdouble;

    if (!me) 
        return 0.0;
    switch(me->o_type) 
    {
        case json_type_double:
            return me->o.c_double;
        case json_type_int:
            return me->o.c_int;
        case json_type_boolean:
            return me->o.c_boolean;
        case json_type_string:
            if (sscanf(me->o.c_string, "%lf", &cdouble) == 1) 
                return cdouble;
        default:
        return 0.0;
    }
}


/* json_string */
static void json_string_delete(json_object* me)
{
    free(me->o.c_string);
    json_generic_delete(me);
}

json_object* json_new_string(char *s)
{
    int str_len = 0;
    if (s)
        str_len = strlen(s);
    return json_new_string_len(s,str_len);
}

json_object* json_new_string_len(char *s, int len)
{
    json_object *me = json_new(json_type_string);
    if (!me) 
        return NULL;
    me->o.c_string = strndup(s, len);
    if (!me->o.c_string)
    {
        free(me);
        return NULL;
    }
    return me;
}

char* json_get_string(json_object *me)
{
    if (!me) 
        return NULL;
    switch(me->o_type) 
    {
        case json_type_string:
            return me->o.c_string;
        default:
            return json_serialize(me);
    }
}

/* json_array */
static void json_array_entry_free(void *data)
{
    json_unref((json_object*)data);
}

static void json_array_delete(json_object* me)
{
    array_list_free(me->o.c_array);
    json_generic_delete(me);
}

json_object* json_new_array()
{
    json_object *me = json_new(json_type_array);
    if (!me) 
        return NULL;
    me->o.c_array = array_list_new(&json_array_entry_free);
    return me;
}

struct array_list* json_get_array(json_object *me)
{
    if (!me) 
        return NULL;

    switch (me->o_type) 
    {
        case json_type_array:
            return me->o.c_array;
        default:
            return NULL;
    }
}

int json_array_length(json_object *me)
{
    return array_list_length(me->o.c_array);
}

int json_array_add(json_object *me,json_object *val)
{
    return array_list_add(me->o.c_array, val);
}

int json_array_put_idx(json_object *me, int idx, json_object *val)
{
    return array_list_put_idx(me->o.c_array, idx, val);
}

json_object* json_array_get_idx(json_object *me, int idx)
{
    return (json_object*)array_list_get_idx(me->o.c_array, idx);
}

/* serialization support */

static int serialize_array(json_object* me, strbuf *pb)
{
    int i;
    json_object *val;

    strbuf_appendf(pb, "[");
    for (i=0; i < json_array_length(me); i++) {
        if (0 != i)
            strbuf_appendf(pb, ", "); 
        else
            strbuf_appendf(pb, " ");

        val = json_array_get_idx(me, i);
        serialize_generic(val, pb);
    }
    return strbuf_appendf(pb, " ]");
}

int serialize_object(json_object* me, strbuf *pb)
{
    int         ok = 1;
    lh_entry *  entry;
    lh_entry *  head;

    ok = strbuf_appendf(pb, "{");

    head = json_get_object(me)->head;
    entry = head;
    while (entry)
    {
        if (entry != head)
            ok = strbuf_appendf(pb, ",");

        if (ok)
            ok = strbuf_appendf(pb, " \"");
        if (ok)
            ok = json_escape_str(pb, (char*)entry->key);
        if (ok)
            ok = strbuf_appendf(pb, "\": ");
        if (ok)
            ok = serialize_generic((json_object*)entry->val, pb);

        if (!ok)
            return 0;
        entry = entry->next;
    }

    return strbuf_appendf(pb, " }");
}

int serialize_generic(json_object *me, strbuf *buf)
{
    int ok = 1;

    if (!me)
        return strbuf_appendf(buf, "null");

    switch (me->o_type)
    {
        case json_type_boolean:
            if (me->o.c_boolean) 
                ok = strbuf_appendf(buf, "true");
            else
                ok = strbuf_appendf(buf, "false");
            break;

        case json_type_int:
            ok = strbuf_appendf(buf, "%d", me->o.c_int);
            break;

        case json_type_double:
            ok = strbuf_appendf(buf, "%lf", me->o.c_double);
            break;

        case json_type_string:
            ok = strbuf_reserve(buf, 2 + 4 + strlen(me->o.c_string));
            if (ok)
                ok = strbuf_append(buf, "\"", 1);
            if (ok)
                ok = json_escape_str(buf, me->o.c_string);
            if (ok)
                ok = strbuf_append(buf, "\"", 1);
            break;

        case json_type_array:
            ok = serialize_array(me, buf);
            break;

        case json_type_object:
            ok = serialize_object(me, buf);
            break;
    }
    return ok;
}

/** JSON_UTIL.C */
#define JSON_FILE_BUF_SIZE 4096

json_object* json_from_file(char *filename)
{
    strbuf *    pb = NULL;
    FILE *      fp = NULL;

    json_object *obj = NULL;
    char        buf[JSON_FILE_BUF_SIZE];
    int         bytes_read;

    pb = strbuf_new_with_size(1024);
    if (!pb)
        return NULL;

    fp = fopen(filename, "rb");
    if (!fp)
        return NULL;

    for (;;)
    {
        bytes_read = fread(buf, 1, JSON_FILE_BUF_SIZE, fp);
        if (bytes_read > 0)
        {
            if (!strbuf_append(pb, buf, bytes_read))
                goto Error;
        }
        if (bytes_read < JSON_FILE_BUF_SIZE)
        {
            if (ferror(fp))
                goto Error;
            break;
        }
    }

    obj = json_deserialize(pb->data);

Exit:
    if (fp)
        fclose(fp);
    strbuf_free(pb);
    return obj;
Error:
    if (obj)
        json_unref(obj);
    goto Exit;
}

/* serialize 'obj' to a file 'filename'.
   Returns a 0 (FALSE) if failed, TRUE if went ok */
int json_to_file(char *filename, json_object *obj)
{
    FILE *  fp;
    size_t  to_write, written;
    char *  json_str;
    
    json_str = json_serialize(obj);
    if (!json_str)
        return 0;
    fp = fopen(filename, "wb");
    if (!fp)
        return 0;
    to_write = strlen(json_str);
    written = fwrite((void*)json_str, 1, to_write, fp);
    fclose(fp);
    if (written != to_write)
        return 0;
    return 1;
}

/** JSON_TOKENER.C */
enum json_tokener_state {
    json_tokener_state_eatws,
    json_tokener_state_start,
    json_tokener_state_finish,
    json_tokener_state_null,
    json_tokener_state_comment_start,
    json_tokener_state_comment,
    json_tokener_state_comment_eol,
    json_tokener_state_comment_end,
    json_tokener_state_string,
    json_tokener_state_string_escape,
    json_tokener_state_escape_unicode,
    json_tokener_state_boolean,
    json_tokener_state_number,
    json_tokener_state_array,
    json_tokener_state_array_sep,
    json_tokener_state_object,
    json_tokener_state_object_field_start,
    json_tokener_state_object_field,
    json_tokener_state_object_field_end,
    json_tokener_state_object_value,
    json_tokener_state_object_sep,
};

enum json_tokener_error {
  json_tokener_success,
  json_tokener_error_parse_unexpected,
  json_tokener_error_parse_null,
  json_tokener_error_parse_boolean,
  json_tokener_error_parse_number,
  json_tokener_error_parse_array,
  json_tokener_error_parse_object,
  json_tokener_error_parse_string,
  json_tokener_error_parse_comment,
  json_tokener_error_parse_eof,
};

struct json_tokener
{
    char *      source;
    int         pos;
    strbuf *    pb;
};

static json_object* json_tokener_do_parse(struct json_tokener *me)
{
  enum json_tokener_state state, saved_state;
  enum json_tokener_error err = json_tokener_success;
  json_object *current = NULL, *obj;
  char *obj_field_name = NULL;
  char quote_char;
  int deemed_double, start_offset;
  char c;

  state = json_tokener_state_eatws;
  saved_state = json_tokener_state_start;

  do {
    c = me->source[me->pos];
    switch(state) {

    case json_tokener_state_eatws:
      if(isspace(c)) {
        me->pos++;
      } else if(c == '/') {
        state = json_tokener_state_comment_start;
        start_offset = me->pos++;
      } else {
        state = saved_state;
      }
      break;

    case json_tokener_state_start:
      switch(c) {
      case '{':
        state = json_tokener_state_eatws;
        saved_state = json_tokener_state_object;
        current = json_new_object();
        me->pos++;
        break;
      case '[':
        state = json_tokener_state_eatws;
        saved_state = json_tokener_state_array;
        current = json_new_array();
        me->pos++;
        break;
      case 'N':
      case 'n':
        state = json_tokener_state_null;
        start_offset = me->pos++;
        break;
      case '"':
      case '\'':
        quote_char = c;
        strbuf_reset(me->pb);
        state = json_tokener_state_string;
        start_offset = ++me->pos;
        break;
      case 'T':
      case 't':
      case 'F':
      case 'f':
        state = json_tokener_state_boolean;
        start_offset = me->pos++;
        break;
          case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      case '-':
        deemed_double = 0;
        state = json_tokener_state_number;
        start_offset = me->pos++;
        break;
      default:
        err = json_tokener_error_parse_unexpected;
        goto out;
      }
      break;

    case json_tokener_state_finish:
      goto out;

    case json_tokener_state_null:
      if (strncasecmp("null", me->source + start_offset, me->pos - start_offset))
            return NULL;
      if(me->pos - start_offset == 4) {
        current = NULL;
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
      } else {
        me->pos++;
      }
      break;

    case json_tokener_state_comment_start:
      if(c == '*') {
        state = json_tokener_state_comment;
      } else if(c == '/') {
        state = json_tokener_state_comment_eol;
      } else {
        err = json_tokener_error_parse_comment;
        goto out;
      }
      me->pos++;
      break;

    case json_tokener_state_comment:
      if(c == '*') state = json_tokener_state_comment_end;
      me->pos++;
      break;

    case json_tokener_state_comment_eol:
      if(c == '\n') {
        state = json_tokener_state_eatws;
      }
      me->pos++;
      break;

    case json_tokener_state_comment_end:
      if(c == '/') {
        state = json_tokener_state_eatws;
      } else {
        state = json_tokener_state_comment;
      }
      me->pos++;
      break;

    case json_tokener_state_string:
      if(c == quote_char) {
        strbuf_append(me->pb, me->source + start_offset,
                           me->pos - start_offset);
        current = json_new_string(me->pb->data);
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
      } else if(c == '\\') {
        saved_state = json_tokener_state_string;
        state = json_tokener_state_string_escape;
      }
      me->pos++;
      break;

    case json_tokener_state_string_escape:
      switch(c) {
      case '"':
      case '\\':
        strbuf_append(me->pb, me->source + start_offset,
                           me->pos - start_offset - 1);
        start_offset = me->pos++;
        state = saved_state;
        break;
      case 'b':
      case 'n':
      case 'r':
      case 't':
        strbuf_append(me->pb, me->source + start_offset,
                           me->pos - start_offset - 1);
        if(c == 'b') strbuf_append(me->pb, "\b", 1);
        else if(c == 'n') strbuf_append(me->pb, "\n", 1);
        else if(c == 'r') strbuf_append(me->pb, "\r", 1);
        else if(c == 't') strbuf_append(me->pb, "\t", 1);
        start_offset = ++me->pos;
        state = saved_state;
        break;
      case 'u':
        strbuf_append(me->pb, me->source + start_offset,
                           me->pos - start_offset - 1);
        start_offset = ++me->pos;
        state = json_tokener_state_escape_unicode;
        break;
      default:
        err = json_tokener_error_parse_string;
        goto out;
      }
      break;

    case json_tokener_state_escape_unicode:
      if(strchr(HEX_CHARS, c)) {
        me->pos++;
        if(me->pos - start_offset == 4) {
          unsigned char utf_out[3];
          unsigned int ucs_char =
            (hexdigit(*(me->source + start_offset)) << 12) +
            (hexdigit(*(me->source + start_offset + 1)) << 8) +
            (hexdigit(*(me->source + start_offset + 2)) << 4) +
            hexdigit(*(me->source + start_offset + 3));
          if (ucs_char < 0x80) {
            utf_out[0] = ucs_char;
            strbuf_append(me->pb, (char*)utf_out, 1);
          } else if (ucs_char < 0x800) {
            utf_out[0] = 0xc0 | (ucs_char >> 6);
            utf_out[1] = 0x80 | (ucs_char & 0x3f);
            strbuf_append(me->pb, (char*)utf_out, 2);
          } else {
            utf_out[0] = 0xe0 | (ucs_char >> 12);
            utf_out[1] = 0x80 | ((ucs_char >> 6) & 0x3f);
            utf_out[2] = 0x80 | (ucs_char & 0x3f);
            strbuf_append(me->pb, (char*)utf_out, 3);
          }
          start_offset = me->pos;
          state = saved_state;
        }
      } else {
        err = json_tokener_error_parse_string;
        goto out;
      }
      break;

    case json_tokener_state_boolean:
      if(strncasecmp("true", me->source + start_offset,
                 me->pos - start_offset) == 0) {
        if(me->pos - start_offset == 4) {
          current = json_new_boolean(1);
          saved_state = json_tokener_state_finish;
          state = json_tokener_state_eatws;
        } else {
          me->pos++;
        }
      } else if(strncasecmp("false", me->source + start_offset,
                        me->pos - start_offset) == 0) {
        if(me->pos - start_offset == 5) {
          current = json_new_boolean(0);
          saved_state = json_tokener_state_finish;
          state = json_tokener_state_eatws;
        } else {
          me->pos++;
        }
      } else {
        err = json_tokener_error_parse_boolean;
        goto out;
      }
      break;

    case json_tokener_state_number:
      if(!c || !strchr(NUMBER_CHARS, c)) {
        int numi;
        double numd;
        char *tmp = strndup(me->source + start_offset,
                            me->pos - start_offset);
        if(!deemed_double && sscanf(tmp, "%d", &numi) == 1) {
          current = json_new_int(numi);
        } else if(deemed_double && sscanf(tmp, "%lf", &numd) == 1) {
          current = json_new_double(numd);
        } else {
          free(tmp);
          err = json_tokener_error_parse_number;
          goto out;
        }
        free(tmp);
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
      } else {
        if(c == '.' || c == 'e') deemed_double = 1;
        me->pos++;
      }
      break;

    case json_tokener_state_array:
      if(c == ']') {
        me->pos++;
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
      } else {
        obj = json_tokener_do_parse(me);
        if(!obj) {
          err = json_tokener_error_parse_unexpected;
          goto out;
        }
        json_array_add(current, obj);
        saved_state = json_tokener_state_array_sep;
        state = json_tokener_state_eatws;
      }
      break;

    case json_tokener_state_array_sep:
      if(c == ']') {
        me->pos++;
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
      } else if(c == ',') {
        me->pos++;
        saved_state = json_tokener_state_array;
        state = json_tokener_state_eatws;
      } else {
        json_unref(current);
        return NULL;
      }
      break;

    case json_tokener_state_object:
      state = json_tokener_state_object_field_start;
      start_offset = me->pos;
      break;

    case json_tokener_state_object_field_start:
      if(c == '}') {
        me->pos++;
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
      } else if (c == '"' || c == '\'') {
        quote_char = c;
        strbuf_reset(me->pb);
        state = json_tokener_state_object_field;
        start_offset = ++me->pos;
      } else {
        err = json_tokener_error_parse_object;
        goto out;
      }
      break;

    case json_tokener_state_object_field:
      if(c == quote_char) {
        strbuf_append(me->pb, me->source + start_offset,
                           me->pos - start_offset);
        obj_field_name = strdup(me->pb->data);
        saved_state = json_tokener_state_object_field_end;
        state = json_tokener_state_eatws;
      } else if(c == '\\') {
        saved_state = json_tokener_state_object_field;
        state = json_tokener_state_string_escape;
      }
      me->pos++;
      break;

    case json_tokener_state_object_field_end:
      if(c == ':') {
        me->pos++;
        saved_state = json_tokener_state_object_value;
        state = json_tokener_state_eatws;
      } else {
        return NULL;
      }
      break;

    case json_tokener_state_object_value:
      obj = json_tokener_do_parse(me);
      if(!obj) {
        err = json_tokener_error_parse_unexpected;
        goto out;
      }
      if (!json_object_add(current, obj_field_name, obj))
      {
        err = json_tokener_error_parse_unexpected;
        goto out;
      }
      free(obj_field_name);
      obj_field_name = NULL;
      saved_state = json_tokener_state_object_sep;
      state = json_tokener_state_eatws;
      break;

    case json_tokener_state_object_sep:
      if(c == '}') {
        me->pos++;
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
      } else if(c == ',') {
        me->pos++;
        saved_state = json_tokener_state_object;
        state = json_tokener_state_eatws;
      } else {
        err = json_tokener_error_parse_object;
        goto out;
      }
      break;

    }
  } while(c);

  if(state != json_tokener_state_finish &&
     saved_state != json_tokener_state_finish)
    err = json_tokener_error_parse_eof;

 out:
  free(obj_field_name);
  if (err == json_tokener_success) 
    return current;
  json_unref(current);
  return NULL;
}

json_object* json_deserialize(char * s)
{
    struct json_tokener tok;
    json_object* obj;

    tok.pb = strbuf_new();
    if (!tok.pb)
        return NULL;

    tok.source = s;
    tok.pos = 0;
    obj = json_tokener_do_parse(&tok);
    strbuf_free(tok.pb);
    return obj;
}

