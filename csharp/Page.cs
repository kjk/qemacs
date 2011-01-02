using System;
using System.Collections.Generic;
using System.IO;

[Flags]
public enum PageFlags
{
    PG_READ_ONLY    = 0x1, /* the page is read only */
    PG_VALID_POS    = 0x2, /* set if the nb_lines / col fields are up to date */
    PG_VALID_CHAR   = 0x4, /* nb_chars is valid */
    PG_VALID_COLORS = 0x8  /* color state is valid */
}

public class Page 
{
    public int          size { get { return data.Length; } } 
    public byte[]       data;
    public PageFlags    flags;
    /* the following are needed to handle line / column computation */
    public int          nb_lines; /* Number of '\n' in data */
    public int          col;      /* Number of chars since the last '\n' */
    /* the following is needed for char offset computation */
    public int          nb_chars;
}

/* high level buffer type handling */
// TODO: make it an interface?
public interface IEditBufferDataType 
{
    string name { get; } /* name of buffer data type (text, image, ...) */
    int Load(EditBuffer b, Stream f);
    int Save(EditBuffer b, string filename);
    void Close(EditBuffer b);
}

public class EditBufferDataTypeRaw : IEditBufferDataType
{
    const int IOBUF_SIZE = 32768;

    public EditBufferDataTypeRaw()
    {
    }

    public string name { get { return "raw"; }}

    // TODO: in C return value indicates error (if < 0). Need to change to
    // exceptions
    public int LoadFile(EditBuffer b, Stream f, int offset)
    {
        byte[] buf = new byte[IOBUF_SIZE];
        for (;;) {
            int len = f.Read(buf, 0, buf.Length);
            if (len == 0)
                break;
            b.Insert(offset, buf, len);
            offset += len;
        }
        return 0;
    }

    // TODO: in C, uses mmap if file bigger than an mmap threshold
    public int Load(EditBuffer b, Stream f)
    {
         return LoadFile(b, f, 0);
    }

    public int Save(EditBuffer b, string filename)
    {
        return 0;
    }

    public void Close(EditBuffer b)
    {
        // nothing to do
    }
}

public class EditBuffer
{
    const int MAX_PAGE_SIZE = 4096;

    public List<IEditBufferDataType> DataTypes = new List<IEditBufferDataType>();
    int total_size; /* total size of the buffer */

    /* Page *page_table;
    int nb_pages; */
    List<Page> page_table = new List<Page>();
    int nb_pages { get { return page_table.Count; } }

    /* page cache */
    Page cur_page = null;
    int cur_offset;

    public EditBuffer()
    {
        RegisterDataType(new EditBufferDataTypeRaw());
    }

    public void RegisterDataType(IEditBufferDataType dt)
    {
        DataTypes.Add(dt);
    }

    /* find a page at a given offset */
    Page find_page(ref int offset_ptr)
    {
        Page p = null;
        int offset;

        offset = offset_ptr;
        if ((cur_page != null) && (offset >= cur_offset) && 
            (offset < cur_offset + cur_page.size)) {
            /* use the cache */
            offset_ptr -= cur_offset;
            return cur_page;
        } else {
            int page_no = 0;
            p = page_table[page_no];
            while (offset >= p.size) {
                offset -= p.size;
                page_no += 1;
                p = page_table[page_no];
            }
            cur_page = p;
            cur_offset = offset_ptr - offset;
            offset_ptr = offset;
            return p;
        }
    }
    
    int PageIndex(Page p)
    {
        return page_table.IndexOf(p);
    }

    /* prepare a page to be written */
    void update_page(Page p)
    {
        /* if the page is read only, copy it */
        if ((p.flags & PageFlags.PG_READ_ONLY) == PageFlags.PG_READ_ONLY) {
            byte[] new_data = new byte[p.size];
            Buffer.BlockCopy(p.data, 0, new_data, 0, p.size);
            p.data = new_data;
            p.flags &= ~PageFlags.PG_READ_ONLY;
        }
        p.flags &= ~(PageFlags.PG_VALID_POS | PageFlags.PG_VALID_CHAR | PageFlags.PG_VALID_COLORS);
    }

    /* internal function for insertion : 'buf' of size 'size' at the
       beginning of the page at page_index */
    void Insert1(int page_index, byte[] buf, int size)
    {
        int len, n;
        Page p = null;

        if (page_index < nb_pages) {
            p = page_table[page_index];
            len = MAX_PAGE_SIZE - p.size;
            if (len > size)
                len = size;
            if (len > 0) {
                update_page(p);
#if NOT_DEFINED
                p->data = realloc(p->data, p->size + len);
                memmove(p->data + len, p->data, p->size);
                memcpy(p->data, buf + size - len, len);
                size -= len;
                p->size += len;
#endif
            }
        }

        /* now add new pages if necessary */
        n = (size + MAX_PAGE_SIZE - 1) / MAX_PAGE_SIZE;
        if (n > 0) {
#if NOT_DEFINED
            for (int i=0; i<n; i++) {
                page_table.Add(new Page());
            }

            b->nb_pages += n;
            b->page_table = realloc(b->page_table, b->nb_pages * sizeof(Page));
            p = b->page_table + page_index;
            memmove(p + n, p,
                    sizeof(Page) * (b->nb_pages - n - page_index));
            while (size > 0) {
                len = size;
                if (len > MAX_PAGE_SIZE)
                    len = MAX_PAGE_SIZE;
                p->size = len;
                p->data = malloc(len);
                p->flags = 0;
                memcpy(p->data, buf, len);
                buf += len;
                size -= len;
                p++;
            }
#endif
        }
    }

    /* We must have : 0 <= offset <= b->total_size */
    void InsertLowlevel(int offset, byte[] buf, int size)
    {
        int len, len_out, page_index;
        Page p = null;

        total_size += size;

#if NOT_ENABLED
        /* find the correct page */
        p = page_table[0];
        if (offset > 0) {
            offset--;
            p = find_page(ref offset);
            offset++;

            /* compute what we can insert in current page */
            len = MAX_PAGE_SIZE - offset;
            if (len > size)
                len = size;
            /* number of bytes to put in next pages */
            len_out = p.size + len - MAX_PAGE_SIZE;
            page_index = PageIndex(p);
            if (len_out > 0)
                eb_insert1(b, page_index + 1, 
                           p->data + p->size - len_out, len_out);
            else
                len_out = 0;

            /* now we can insert in current page */
            if (len > 0) {
                p = page_table[page_index];
                update_page(p);
                int new_size = size + p.size + len - len_out;
                byte[] new_data = new byte[new_size];
                
                p->data = realloc(p->data, p->size);
                memmove(p->data + offset + len, 
                        p->data + offset, p->size - (offset + len));
                memcpy(p->data + offset, buf, len);
                buf += len;
                size -= len;
            }
        } else {
            page_index = -1;
        }
#endif

#if NOT_ENABLED
        /* insert the remaining data in the next pages */
        if (size > 0)
            eb_insert1(b, page_index + 1, buf, size);
#endif

        /* the page cache is no longer valid */
        cur_page = null;
    }
    
    /* Insert 'size' bytes from 'buf' into 'b' at offset 'offset'. We must
       have : 0 <= offset <= b->total_size */
    public void Insert(int offset, byte[] buf, int size)
    {

//        eb_addlog(b, LOGOP_INSERT, offset, size);
        InsertLowlevel(offset, buf, size);
        /* the page cache is no longer valid */
//        b->cur_page = NULL;
    }
}