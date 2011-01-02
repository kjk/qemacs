using System;
using System.Collections.Generic;
using System.IO;

public class Page 
{
    [Flags]
    public enum PageFlags
    {
        PG_READ_ONLY    = 0x1, /* the page is read only */
        PG_VALID_POS    = 0x2, /* set if the nb_lines / col fields are up to date */
        PG_VALID_CHAR   = 0x4, /* nb_chars is valid */
        PG_VALID_COLORS = 0x8  /* color state is valid */
    }

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
public class EditBufferDataType 
{
    public string name; /* name of buffer data type (text, image, ...) */
    public virtual int Load(EditBuffer b, Stream f) { return 0; }
    public virtual int Save(EditBuffer b, string filename) { return 0;}
    public virtual void Close(EditBuffer b) {}
}

public class EditBufferDataTypeRaw : EditBufferDataType
{
    const int IOBUF_SIZE = 32768;

    public EditBufferDataTypeRaw()
    {
        name = "raw";
    }

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
    public override int Load(EditBuffer b, Stream f)
    {
         return LoadFile(b, f, 0);
    }

    public override int Save(EditBuffer b, string filename)
    {
        return 0;
    }

    public override void Close(EditBuffer b)
    {
        // nothing to do
    }
}

public class EditBuffer
{
    public List<EditBufferDataType> DataTypes = new List<EditBufferDataType>();

    public EditBuffer()
    {
        RegisterDataType(new EditBufferDataTypeRaw());
    }

    public void RegisterDataType(EditBufferDataType dt)
    {
        DataTypes.Add(dt);
    }
    
    /* Insert 'size' bytes from 'buf' into 'b' at offset 'offset'. We must
       have : 0 <= offset <= b->total_size */
    public void Insert(int offset, byte[] buf, int size)
    {

//        eb_addlog(b, LOGOP_INSERT, offset, size);
//        eb_insert_lowlevel(b, offset, buf, size);
        /* the page cache is no longer valid */
//        b->cur_page = NULL;
    }
}