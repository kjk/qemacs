#ifndef PAGE_H__
#define PAGE_H__

#include "qe.h"

typedef enum {
    PG_READ_ONLY    = 0x01, /* the page is read only */
    PG_VALID_POS    = 0x02, /* set if the nb_lines / col fields are up to date */
    PG_VALID_CHAR   = 0x04, /* nb_chars is valid */
    PG_VALID_COLORS = 0x08 /* color state is valid */
} PageFlags;

typedef struct Page {
    int size; /* data size */ 
    u8 *data;
    PageFlags flags;
    /* the following are needed to handle line / column computation */
    int nb_lines; /* Number of '\n' in data */
    int col;      /* Number of chars since the last '\n' */
    /* the following is needed for char offset computation */
    int nb_chars;
} Page;

#endif
