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

#ifndef JSON_H_
#define JSON_H_

#include "json-config.h"

#include "strbuf.h"

#if STDC_HEADERS
# include <stddef.h>
#endif /* STDC_HEADERS */

typedef struct lh_entry {
    void *key;
    void *val;
    struct lh_entry *next;
    struct lh_entry *prev;
} lh_entry;

typedef void (lh_entry_free_fn) (lh_entry *e);
typedef unsigned long (lh_hash_fn) (void *k);
typedef int (lh_equal_fn) (void *k1, void *k2);

/**
 * The hash table structure.
 */
typedef struct lh_table {
    int size;
    int count;
    int collisions;
    int resizes;
    int lookups;
    int inserts;
    int deletes;
    char *name;
    lh_entry *head;
    lh_entry *tail;

    lh_entry *table;

    /**
     * A pointer onto the function responsible for freeing an entry.
     */
    lh_entry_free_fn *free_fn;
    lh_hash_fn *hash_fn;
    lh_equal_fn *equal_fn;
}lh_table;

/**
 * Pre-defined hash and equality functions
 */
extern unsigned long lh_ptr_hash(void *k);
extern int lh_ptr_equal(void *k1, void *k2);

extern unsigned long lh_char_hash(void *k);
extern int lh_char_equal(void *k1, void *k2);

/**
 * Convenience list iterator.
 */
#define lh_foreach(table, entry) \
for(entry = table->head; entry; entry = entry->next)

/**
 * lh_foreach_safe allows calling of deletion routine while iterating.
 */
#define lh_foreach_safe(table, entry, tmp) \
for(entry = table->head; entry && ((tmp = entry->next) || 1); entry = tmp)

lh_table* lh_table_new(int size, char *name,
                                     lh_entry_free_fn *free_fn,
                                     lh_hash_fn *hash_fn,
                                     lh_equal_fn *equal_fn);

lh_table* lh_kchar_table_new(int size, char *name,
                                           lh_entry_free_fn *free_fn);

lh_table* lh_kptr_table_new(int size, char *name,
                                          lh_entry_free_fn *free_fn);
void lh_table_free(lh_table *t);

int lh_table_insert(lh_table *t, void *key, void *val);

lh_entry* lh_table_lookup_entry(lh_table *t, void *k);

void* lh_table_lookup(lh_table *t, void *k);

int lh_table_delete_entry(lh_table *t, lh_entry *e);

int lh_table_delete(lh_table *t, void *k);

/* ARRAY_LIST interface */

#define ARRAY_LIST_DEFAULT_SIZE 32

typedef void (array_list_free_fn) (void *data);

struct array_list
{
  void **array;
  int length;
  int size;
  array_list_free_fn *free_fn;
};

extern struct array_list*
array_list_new(array_list_free_fn *free_fn);

extern void
array_list_free(struct array_list *me);

extern void*
array_list_get_idx(struct array_list *me, int i);

extern int
array_list_put_idx(struct array_list *me, int i, void *data);

extern int
array_list_add(struct array_list *me, void *data);

extern int
array_list_length(struct array_list *me);

/** JSON_OBJECT.H */

enum json_type {
  json_type_boolean,
  json_type_double,
  json_type_int,
  json_type_object,
  json_type_array,
  json_type_string,
};

typedef struct json_object
{
    enum json_type  o_type;
    int             _ref_count;
    strbuf *        _pb;
    union data {
        int     c_boolean;
        double  c_double;
        int     c_int;
        lh_table *c_object;
        struct array_list *c_array;
        char *  c_string;
    } o;
} json_object;

json_object* json_addref(json_object *me);
int json_unref(json_object *me);

extern enum json_type json_get_type(json_object *me);

json_object* json_new_object();

lh_table* json_get_object(json_object *me);

int json_object_add(json_object* me, char *key, json_object *val);
json_object* json_object_get(json_object* me, char *key);
void json_object_del(json_object* me, char *key);

json_object* json_new_array();

struct array_list* json_get_array(json_object *me);

int json_array_length(json_object *me);

int json_array_add(json_object *me, json_object *val);

/** Insert or replace an element at a specified index in an array (a json_object of type json_type_array)
 *
 * The reference count will *not* be incremented. This is to make adding
 * fields to objects in code more compact. If you want to retain a reference
 * to an added object you must wrap the passed object with json_addref
 *
 * The reference count of a replaced object will be decremented.
 *
 * The array size will be automatically be expanded to the size of the
 * index if the index is larger than the current size.
 *
 * @param this the json_object instance
 * @param idx the index to insert the element at
 * @param val the json_object to be added
 */
int json_array_put_idx(json_object *me, int idx, json_object *val);

/** Get the element at specificed index of the array (a json_object of type json_type_array)
 * @param this the json_object instance
 * @param idx the index to get the element at
 * @returns the json_object at the specified index (or NULL)
 */
json_object* json_array_get_idx(json_object *me, int idx);

/* boolean type methods */

json_object* json_new_boolean(int b);

/** Get the boolean value of a json_object
 *
 * The type is coerced to a boolean if the passed object is not a boolean.
 * integer and double objects will return FALSE if there value is zero
 * or TRUE otherwise. If the passed object is a string it will return
 * TRUE if it has a non zero length. If any other object type is passed
 * TRUE will be returned if the object is not NULL.
 *
 * @param this the json_object instance
 * @returns a boolean
 */
int json_get_boolean(json_object *me);

/* int type methods */

json_object* json_new_int(int i);

/** Get the int value of a json_object
 *
 * The type is coerced to a int if the passed object is not a int.
 * double objects will return their integer conversion. Strings will be
 * parsed as an integer. If no conversion exists then 0 is returned.
 *
 * @param this the json_object instance
 * @returns an int
 */
int json_get_int(json_object *me);

/* double type methods */

/** Create a new empty json_object of type json_type_double
 * @param d the double
 * @returns a json_object of type json_type_double
 */
json_object* json_new_double(double d);

/** Get the double value of a json_object
 *
 * The type is coerced to a double if the passed object is not a double.
 * integer objects will return their dboule conversion. Strings will be
 * parsed as a double. If no conversion exists then 0.0 is returned.
 *
 * @param this the json_object instance
 * @returns an double
 */
double json_get_double(json_object *me);


/* string type methods */

/** Create a new empty json_object of type json_type_string
 *
 * A copy of the string is made and the memory is managed by the json_object
 *
 * @param s the string
 * @returns a json_object of type json_type_string
 */
json_object* json_new_string(char *s);

json_object* json_new_string_len(char *s, int len);

/** Get the string value of a json_object
 *
 * If the passed object is not of type json_type_string then the JSON
 * representation of the object is returned.
 *
 * The returned string memory is managed by the json_object and will
 * be freed when the reference count of the json_object drops to zero.
 *
 * @param this the json_object instance
 * @returns a string
 */
char* json_get_string(json_object *me);

char*        json_serialize(json_object *me);
json_object* json_deserialize(char *s);
json_object* json_from_file(char *filename);
int          json_to_file(char *filename, json_object *obj);

#endif
