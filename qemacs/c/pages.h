#ifndef PAGE_H__
#define PAGE_H__

#include "vec.h"
#include <assert.h>

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

    Page() {
        data = NULL;
        size = 0;
        ClearAttrs();
    }

    Page(int size) {
        data = (u8*)malloc(size);
        this->size = size;
        ClearAttrs();
    }

    Page(const u8 *buf, int size) {
        data = (u8*)malloc(size);
        this->size = size;
        ClearAttrs();
        memcpy(data, buf, size);
    }

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

class Pages {

private:
    /* page cache */
    Page *  cur_page;
    int     cur_offset;
    int     cur_idx;

    bool IsOffsetInCache(int offset) {
        return (NULL != cur_page) && 
               (offset >= cur_offset) && 
               (offset < (cur_offset + cur_page->size));
    }

    void Insert(int page_index, const u8 *buf, int size);
    
public:
    PtrVec<Page> *page_table;

    int     TotalSize() {
        int size = 0;
        for (int i=0; i<nb_pages(); i++) {
            Page *p = PageAt(i);
            size += p->size;
        }
        return size;
    }

    void VerifySize() {
        assert(TotalSize() == total_size);
    }

    int     total_size; /* sum of Page.size in page_table */

    Pages() {
        cur_page = NULL;
        total_size = 0;
        page_table = new PtrVec<Page>();
    }

    ~Pages() {
        delete page_table;
    }

    int nb_pages() { return page_table->Count(); }

    Page *PageAt(int idx) {
        return page_table->At(idx);
    }

    Page *FindPage(int *offset_ptr, int *idx_ptr = NULL);

    void InvalidateCache() {
        cur_page = NULL;
    }

    int  LimitSize(int offset, int size);
    void Delete(int offset, int size);
    void ReadWrite(int offset, u8 *buf, int size, int do_write);
    int  Read(int offset, void *buf, int size);
    void InsertLowLevel(int offset, const u8 *buf, int size);
    void InsertFrom(int dest_offset, Pages *src_pages, int src_offset, int size);

    int  GetCharOffset(int offset, QECharset *charset);
    int  GotoChar(QECharset *charset, int pos);

    int  GetPos(CharsetDecodeState *charset_state, int *line_ptr, int *col_ptr, int offset);
    int  GotoPos(CharsetDecodeState *charset_state, int line1, int col1);

    int  NextChar(CharsetDecodeState *charset_state, int offset, int *next_offset);
    int  PrevChar(QECharset *charset, int offset, int *prev_offset);
};

#endif

