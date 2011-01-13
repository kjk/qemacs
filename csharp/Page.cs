using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;

namespace qemacs
{
    public class Page
    {
        public int size { get { return data.Length; } }
        public byte[] data;
        public bool read_only;      /* the page is read only */
        public bool valid_pos;      /* set if the nb_lines / col fields are up to date */
        public bool valid_char;     /* nb_chars is valid */
        public bool valid_colors;   /* color state is valid */

        public Page(byte[] data)
        {
            this.data = data;
        }

        public void InvalidateAttributes()
        {
            valid_pos = valid_char = valid_colors = false;
        }

        /* the following are needed to handle line / column computation */
        public int nb_lines; /* Number of '\n' in data */
        public int col;      /* Number of chars since the last '\n' */
        /* the following is needed for char offset computation */
        public int nb_chars;

        // TODO: take QECharset into account
        public static int get_chars(byte[] buf, int size)
        {
            int nb_chars = 0;
            int i = 0;
            while (i < size)
            {
                byte c = buf[i++];
                if ((c < 0x80) || (c >= 0xc0))
                    nb_chars++;
            }
            return nb_chars;
        }

        /* return the number of lines and column position for a buffer */
        // TODO: take CharsetDecodeState into account
        public static void get_pos(byte[] buf, int size, ref int line_ptr, ref int col_ptr)
        {
            int line = 0;
            int i = 0;
            int lp = 0;
            const byte nl = (byte)'\n';
            while (i < size)
            {
                if (nl == buf[i++])
                {
                    lp = i;
                    ++line;
                }
            }
            // TODO: take into account charset
            int col = size - lp;
            line_ptr = line;
            col_ptr = col;
        }

        // TODO: take QECharset into account
        public static int goto_char(byte[] buf, int pos)
        {
            // TODO: implement me
            return pos;
        }

        public void Update()
        {
            if (read_only)
            {
                byte[] new_data = new byte[data.Length];
                Buffer.BlockCopy(data, 0, new_data, 0, data.Length);
                data = new_data;
                read_only = false;
            }
            InvalidateAttributes();
        }
    }

    public class Pages
    {
        public const int MAX_PAGE_SIZE = 4096;

        public List<Page> page_table = new List<Page>();
        //public int nb_pages { get { return page_table.Count; } }

        /* page cache */
        public Page cur_page;
        public int cur_page_offset;
        public int cur_page_idx;

        public int total_size;

        public bool IsOffsetInCache(int offset)
        {
            return (null != cur_page) &&
                (offset >= cur_page_offset) &&
                (offset < (cur_page_offset + cur_page.size));
        }

        public int CalcTotalSize()
        {
            int n = 0;
            foreach (Page p in page_table)
                n += p.size;
            return n;
        }

        void VerifyTotalSize()
        {
            Debug.Assert(total_size == CalcTotalSize());
        }

        int PagesForSize(int size)
        {
            return (size + MAX_PAGE_SIZE - 1) / MAX_PAGE_SIZE;
        }

        /*
        public int FindPageIdx(Page p)
        {
            for (int i = 0; i < page_table.Count; i++)
            {
                if (page_table[i] == p)
                    return i;
            }
            return -1;
        }*/

        void CachePageAtOffset(int offset)
        {
            int page_idx = 0;
            int page_offset = 0;
            foreach (Page p in page_table)
            {
                if (offset < page_offset + p.size)
                    page_offset += p.size;
                else
                {
                    cur_page = p;
                    cur_page_offset = page_offset;
                    cur_page_idx = page_idx;
                    return;
                }
                ++page_idx;
            }
            Debug.Assert(false);
        }

        public Page FindPage(ref int offset, out int page_idx)
        {
            if (!IsOffsetInCache(offset))
                CachePageAtOffset(offset);

            offset -= cur_page_offset;
            page_idx = cur_page_idx;
            return cur_page;
        }

        public void InvalidateCache()
        {
            cur_page = null;
        }

        /*
        public Page PageAfter(Page p)
        {
            for (int i = 0; i < page_table.Count; i++)
            {
                if (page_table[i] == p)
                    return page_table[i + 1];
            }
            return null;
        }*/

        public void ReadWrite(int offset, byte[] buf, int size, bool do_write)
        {
            int len, idx;
            Page p = FindPage(ref offset, out idx);
            int bufOffset = 0;
            while (size > 0)
            {
                len = Math.Min(p.size - offset, size);
                if (do_write)
                {
                    p.Update();
                    Buffer.BlockCopy(buf, bufOffset, p.data, offset, len);
                }
                else
                    Buffer.BlockCopy(p.data, offset, buf, bufOffset, len);
                bufOffset += len;
                size -= len;
                offset += len;
                if (offset >= p.size)
                {
                    ++idx;
                    p = page_table[idx];
                    offset = 0;
                }
            }
        }

        public void Delete(int offset, int size)
        {
            total_size -= size;
            int idx;
            Page p = FindPage(ref offset, out idx);
            while (size > 0)
            {
                int len = Math.Min(p.size - offset, size);
                if (len == p.size)
                {
                    page_table.RemoveAt(idx);
                    offset = 0;
                }
                else
                {
                    p.Update();
                    Buffer.BlockCopy(p.data, offset + len, p.data, offset, p.size - offset - len);
                    int new_size = p.size - len;
                    byte[] new_data = new byte[new_size];
                    Buffer.BlockCopy(p.data, 0, new_data, 0, new_size);
                    p.data = new_data;
                    offset += len;
                    if (offset >= p.size)
                    {
                        offset = 0;
                        p = page_table[++idx];
                    }
                }
                size -= len;
            }
            VerifyTotalSize();
        }

        byte[] PageDataFromBuf(byte[] buf, ref int size, ref int offset)
        {
            int len = Math.Min(size, MAX_PAGE_SIZE);
            byte[] data = new byte[len];
            Buffer.BlockCopy(buf, offset, data, 0, len);
            size -= len;
            offset += len;
            return data;
        }

        // TODO: get rid of size and use buf.Length instead?
        // Note: @size is the total size of buf
        Page[] PagesFromData(byte[] buf, int offset, int size)
        {
            size = size - offset;
            Debug.Assert(size > 0);
            int n = PagesForSize(size);
            Page[] pages = new Page[n];
            int cur_page = 0;
            while (size > 0)
            {
                pages[cur_page].data = PageDataFromBuf(buf, ref size, ref offset);
            }
            return pages;
        }

        // TODO: see if can get rid of @size and derive it from @buf.Length
        void Insert(int page_index, byte[] buf, int size)
        {
            // TODO: add the end of buf at the beginning of page at page_index
            // if that page can be extended
            page_table.InsertRange(page_index, PagesFromData(buf, 0, size));
        }

        // TODO: see if can get rid of @size and derive it from @buf.Length
        void AppendAtEnd(byte[] buf, int size)
        {
            // TODO: also use remaining space on the last page
            page_table.AddRange(PagesFromData(buf, 0, size));
        }

        Tuple<byte[], byte[]> SplitBuf(byte[] b, int offset)
        {
            int leftSize = offset;
            int rightSize = b.Length - leftSize;
            byte[] left = new byte[leftSize];
            byte[] right = new byte[rightSize];
            Buffer.BlockCopy(b, 0, left, 0, leftSize);
            Buffer.BlockCopy(b, offset, right, 0, rightSize);
            Debug.Assert(right[right.Length] == b[b.Length]);
            return new Tuple<byte[], byte[]>(left, right);
        }

        void SplitPage(int page_idx, int offset)
        {
            var bufs = SplitBuf(page_table[page_idx].data, offset);
            page_table[page_idx] = new Page(bufs.Item1); ;
            page_table.Insert(page_idx + 1, new Page(bufs.Item2));
        }

        void InsertLowLevel(int page_idx, int offset, byte[] buf, int size)
        {
            if (0 == offset)
            {
                Insert(page_idx, buf, size);
                return;
            }
            SplitPage(page_idx, offset);
            Insert(page_idx + 1, buf, size);
        }

        // TODO: see if can get rid of @size and derive it from @buf.Length
        public void InsertLowLevel(int offset, byte[] buf, int size)
        {
            Debug.Assert(offset <= total_size);
            if (offset == total_size)
                AppendAtEnd(buf, size);
            else
            {
                int idx;
                FindPage(ref offset, out idx);
                InsertLowLevel(idx, offset, buf, size);
                InvalidateCache();
            }
        }
    }
}


