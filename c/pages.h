#ifndef PAGE_H__
#define PAGE_H__

#define MAX_PAGE_SIZE 4096
//#define MAX_PAGE_SIZE 16

typedef struct Page {
    int size; /* data size */ 
    u8 *data;
    unsigned read_only:1;    /* the page is read only */
    unsigned valid_pos:1;    /* set if the nb_lines / col fields are up to date */
    unsigned valid_char:1;   /* nb_chars is valid */
    unsigned valid_colors:1; /* color state is valid */

    /* the following are needed to handle line / column computation */
    int nb_lines; /* Number of '\n' in data */
    int col;      /* Number of chars since the last '\n' */
    /* the following is needed for char offset computation */
    int nb_chars;
} Page;

static inline void invalidate_attrs(Page *p)
{
    p->valid_pos = 0;
    p->valid_char = 0;
    p->valid_colors = 0;
}

static inline void clear_attrs(Page *p)
{
    invalidate_attrs(p);
    p->read_only = 0;
}

static inline void copy_attrs(Page *src, Page *dst)
{
    dst->valid_pos = src->valid_pos;
    dst->valid_char = src->valid_char;
    dst->valid_colors = src->valid_colors;
    dst->read_only = src->read_only;
}

int get_chars(u8 *buf, int size, QECharset *charset);
void get_pos(u8 *buf, int size, int *line_ptr, int *col_ptr, CharsetDecodeState *s);

void update_page(Page *p);
void page_calc_pos(Page *p, CharsetDecodeState *charset_state);
void page_calc_chars(Page *p, QECharset *charset);

#endif

