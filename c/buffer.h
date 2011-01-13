#ifndef BUFFER_H__
#define BUFFER_H__

/* begin to mmap files from this size */
#define MIN_MMAP_SIZE (1024*1024)

#define NB_LOGS_MAX 50

#include "pages.h"

typedef struct Pages {
    Page *page_table;
    int nb_pages;
    /* page cache */
    Page *cur_page;
    int cur_offset;

    int total_size; /* sum of Page.size in page_table */
} Pages;

#define DIR_LTR 0
#define DIR_RTL 1

typedef int DirType;

enum LogOperation {
    LOGOP_FREE = 0,
    LOGOP_WRITE,
    LOGOP_INSERT,
    LOGOP_DELETE
};

struct EditBuffer;

/* each buffer modification can be catched with this callback */
typedef void (*EditBufferCallback)(struct EditBuffer *,
                                   void *opaque,
                                   enum LogOperation op,
                                   int offset,
                                   int size);

typedef struct EditBufferCallbackList {
    void *opaque;
    EditBufferCallback callback;
    struct EditBufferCallbackList *next;
} EditBufferCallbackList;

/* buffer flags */
#define BF_SAVELOG   0x0001  /* activate buffer logging */
#define BF_SYSTEM    0x0002  /* buffer system, cannot be seen by the user */
#define BF_READONLY  0x0004  /* read only buffer */
#define BF_PREVIEW   0x0008  /* used in dired mode to mark previewed files */
#define BF_LOADING   0x0010  /* buffer is being loaded */
#define BF_SAVING    0x0020  /* buffer is being saved */
#define BF_DIRED     0x0100  /* buffer is interactive dired */

typedef struct EditBuffer {
    Pages pages;

    int mark;       /* current mark (moved with text) */
    int modified;

    /* if the file is kept open because it is mapped, its handle is there */
#ifdef WIN32
    HANDLE file_handle;
    HANDLE file_mapping;
#else
    int file_handle;
#endif
    int flags;

    /* buffer data type (default is raw) */
    struct EditBufferDataType *data_type;
    void *data; /* associated buffer data, used if data_type != raw_data */
    
    /* charset handling */
    CharsetDecodeState charset_state;
    QECharset *charset;

    /* undo system */
    int save_log;    /* if true, each buffer operation is loged */
    int log_new_index, log_current;
    struct EditBuffer *log_buffer;
    int nb_logs;

    /* modification callbacks */
    EditBufferCallbackList *first_callback;
    
    /* asynchronous loading/saving support */
    struct BufferIOState *io_state;
    
    /* used during loading */
    int probed;

    /* buffer polling & private data */
    void *priv_data;
    /* called when deleting the buffer */
    void (*close)(struct EditBuffer *);

    /* saved data from the last opened mode, needed to restore mode */
    /* CG: should instead keep a pointer to last window using this
     * buffer, even if no longer on screen
     */
    struct ModeSavedData *saved_data; 

    struct EditBuffer *next; /* next editbuffer in qe_state buffer list */
    char name[256];     /* buffer name */
    char filename[MAX_FILENAME_SIZE]; /* file name */
} EditBuffer;

struct ModeProbeData;

/* high level buffer type handling */
typedef struct EditBufferDataType {
    const char *name; /* name of buffer data type (text, image, ...) */
    int (*buffer_load)(EditBuffer *b, FILE *f);
    int (*buffer_save)(EditBuffer *b, const char *filename);
    void (*buffer_close)(EditBuffer *b);
    struct EditBufferDataType *next;
} EditBufferDataType;

extern EditBuffer *trace_buffer;

void eb_init(void);
int eb_read(EditBuffer *b, int offset, void *buf, int size);
void eb_write(EditBuffer *b, int offset, void *buf, int size);
void eb_insert_buffer(EditBuffer *dest, int dest_offset, 
                      EditBuffer *src, int src_offset, 
                      int size);
void eb_insert(EditBuffer *b, int offset, const void *buf, int size);
void eb_append(EditBuffer *b, const void *buf, int size);
void eb_delete(EditBuffer *b, int offset, int size);
void eb_log_reset(EditBuffer *b);
EditBuffer *eb_new(const char *name, int flags);
void eb_free(EditBuffer *b);
EditBuffer *eb_find(const char *name);
EditBuffer *eb_find_file(const char *filename);

void eb_set_charset(EditBuffer *b, QECharset *charset);
int eb_nextc(EditBuffer *b, int offset, int *next_offset);
int eb_prevc(EditBuffer *b, int offset, int *prev_offset);
int eb_goto_pos(EditBuffer *b, int line1, int col1);
int eb_get_pos(EditBuffer *b, int *line_ptr, int *col_ptr, int offset);
int eb_goto_char(EditBuffer *b, int pos);
int eb_get_char_offset(EditBuffer *b, int offset);
void do_undo(struct EditState *s);

int raw_load_buffer1(EditBuffer *b, FILE *f, int offset);
int save_buffer(EditBuffer *b);
void set_buffer_name(EditBuffer *b, const char *name1);
void set_filename(EditBuffer *b, const char *filename);
int eb_add_callback(EditBuffer *b, EditBufferCallback cb,
                    void *opaque);
void eb_free_callback(EditBuffer *b, EditBufferCallback cb,
                      void *opaque);
void eb_offset_callback(EditBuffer *b,
                        void *opaque,
                        enum LogOperation op,
                        int offset,
                        int size);
void eb_printf(EditBuffer *b, const char *fmt, ...);
void eb_line_pad(EditBuffer *b, int n);
int eb_get_str(EditBuffer *b, char *buf, int buf_size);
int eb_get_line(EditBuffer *b, unsigned int *buf, int buf_size,
                int *offset_ptr);
int eb_get_strline(EditBuffer *b, char *buf, int buf_size,
                   int *offset_ptr);
int eb_goto_bol(EditBuffer *b, int offset);
int eb_is_empty_line(EditBuffer *b, int offset);
int eb_is_empty_from_to(EditBuffer *b, int offset_start, int offset_end);
int eb_next_line(EditBuffer *b, int offset);

void eb_register_data_type(EditBufferDataType *bdt);
EditBufferDataType *eb_probe_data_type(const char *filename, int mode,
                                       u8 *buf, int buf_size);
void eb_set_data_type(EditBuffer *b, EditBufferDataType *bdt);
void eb_invalidate_raw_data(EditBuffer *b);
extern EditBufferDataType raw_data_type;

static inline int eb_total_size(EditBuffer *b) {
    return b->pages.total_size;
}

#endif

