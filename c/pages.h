#ifndef PAGE_H__
#define PAGE_H__

#define MAX_PAGE_SIZE 4096
//#define MAX_PAGE_SIZE 16

class Page {
public:
    u8 *        data;
    int         size; /* size of data*/ 
    unsigned    read_only:1;    /* the page is read only */
    unsigned    valid_pos:1;    /* set if the nb_lines / col fields are up to date */
    unsigned    valid_char:1;   /* nb_chars is valid */
    unsigned    valid_colors:1; /* color state is valid */

    /* the following are needed to handle line / column computation */
    int         nb_lines; /* Number of '\n' in data */
    int         col;      /* Number of chars since the last '\n' */
    /* the following is needed for char offset computation */
    int         nb_chars;

    void InvalidateAttrs() {
        valid_pos = 0;
        valid_char = 0;
        valid_colors = 0;
    }

    void ClearAttrs() {
        read_only = 0;
        InvalidateAttrs();
    }

    void PrepareForUpdate();

    void CalcPos(CharsetDecodeState *charset_state);
    void CalcChars(QECharset *charset);

};

typedef struct Pages {
    Page *  page_table;
    int     nb_pages;

    /* page cache */
    Page *  cur_page;
    int     cur_offset;

    int     total_size; /* sum of Page.size in page_table */
} Pages;

#if 0
static inline void copy_attrs(Page *src, Page *dst)
{
    dst->valid_pos = src->valid_pos;
    dst->valid_char = src->valid_char;
    dst->valid_colors = src->valid_colors;
    dst->read_only = src->read_only;
}
#endif

int  pages_limit_size(Pages *pages, int offset, int size);
void pages_rw(Pages *pages, int offset, u8 *buf, int size, int do_write);
int  pages_read(Pages *pages, int offset, void *buf, int size);
void pages_delete(Pages *pages, int offset, int size);
void pages_insert_lowlevel(Pages *pages, int offset, const u8 *buf, int size);
void pages_insert_from(Pages *dest_pages, int dest_offset, Pages *src_pages, int src_offset, int size);
int  pages_get_char_offset(Pages *pages, int offset, QECharset *charset);
int  pages_get_pos(Pages *pages, CharsetDecodeState *charset_state, int *line_ptr, int *col_ptr, int offset);
int  pages_goto_char(Pages *pages, QECharset *charset, int pos);
int  pages_nextc(Pages *pages, CharsetDecodeState *charset_state, int offset, int *next_offset);
int  pages_prevc(Pages *pages, QECharset *charset, int offset, int *prev_offset);

#endif

