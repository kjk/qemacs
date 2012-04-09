using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace qemacs
{
    /* high level buffer type handling */
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

        public string name { get { return "raw"; } }

        // TODO: in C return value indicates error (if < 0). Need to change to
        // exceptions
        public int LoadFile(EditBuffer b, Stream f, int offset)
        {
            byte[] buf = new byte[IOBUF_SIZE];
            for (; ; )
            {
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
}
