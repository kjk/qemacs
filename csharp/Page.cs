using System;

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

    public int          size; /* data size */ 
    public byte[]       data;
    public PageFlags    flags;
    /* the following are needed to handle line / column computation */
    public int          nb_lines; /* Number of '\n' in data */
    public int          col;      /* Number of chars since the last '\n' */
    /* the following is needed for char offset computation */
    public int          nb_chars;
}

