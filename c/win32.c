/*
 * MS Windows driver for QEmacs
 * Copyright (c) 2002 Fabrice Bellard.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "qe.h"

#include <limits.h> /* windows's WHEEL_PAGESCROLL needs this */
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <richedit.h>
#include <assert.h>

#define  DRAGQUERY_NUMFILES 0xFFFFFFFF
#define PROG_NAME "qemacs"
#define DEFAULT_FONT_NAME "Consolas"
#define DEFAULT_FONT_SIZE 8

#define DEFAULT_WIN_DX 80
#define DEFAULT_WIN_DY 55

#define BUF_STATIC_LEN 256

extern int main1(int argc, char **argv);
LRESULT CALLBACK qe_wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern QEDisplay    win32_dpy;

static HINSTANCE    g_hprevinst, _hInstance;
static int          font_xsize;

static int          url_exit_request = 0;

/* state of a single window */
typedef struct WinWindow {
    HWND    hwnd;
    HDC     hdc_orig;
    HDC     hdc_double_buffer;
    int     double_buffer;
    HDC     hdc;
    HBITMAP bmp_double_buf;
    int     bmp_dx;
    int     bmp_dy;
    HFONT   font;
    HMENU   hmenu;
} WinWindow;

typedef struct QEEventQ {
    QEEvent ev;
    struct QEEventQ *next;
} QEEventQ;
 
QEEventQ *  first_event = NULL;
QEEventQ *  last_event = NULL;
WinWindow   win_ctx = {0};

static char *strndup(char *str, int len)
{
    char *new_str = (char*)malloc(len+1);
    if (!new_str)
        return NULL;
    strncpy(new_str, str, len);
    new_str[len] = 0;
    return new_str;
}

static inline void bzero(void* buf, int len)
{
    ZeroMemory(buf, len);
}

/* convert list of space-delimited strings in 'str' into an array of strings,
   and add a NULL as the last element of the array. Return number of strings
   (without the terminating NULL) in 'str_arr_len_out'. if 'make_copy'
   is != 0, creates a copy of the original 'str', otherwise uses 'str' and
   modifies the string in-place */
char **str_to_str_array(char *str, int *str_arr_len_out, int make_copy)
{
    char *  p;
    char *  str_beg;
    int     count;
    char ** str_arr = NULL;
    int     phase;
    int     failed = 0;
    int     i;

    for (phase = 0; phase < 2; phase++)
    {
        p = str;
        count = 0;
        for (;;) {
            skip_spaces((const char **)&p);
            if (*p == '\0')
                break;
            str_beg = p;
            skip_non_spaces((const char**)&p);
            if (1 == phase)
            {
                if (make_copy)
                    str_arr[count] = strndup(str_beg, (int)(p-str_beg));
                else
                    str_arr[count] = str_beg;
            }
            count++;
            if (*p == '\0')
                break;
            if ((1 == phase) && !make_copy)
            {
                *p = '\0';
                ++p;
            }
        }

        if (0 == phase)
        {
            str_arr = (char **)malloc( (count + 1) * sizeof(char *) );
            if (!str_arr)
                return NULL;
            bzero(str_arr, (count + 1) * sizeof(char *));
        }
    }

    /* check if we failed to allocate string array */
    for (i = 0; i < count; i++)
    {
        if (!str_arr[i])
            failed = 1;
    }

    if (failed)
    {
        for (i = 0; i < count; i++)
        {
            free((void*)str_arr[i]);
        }
        free((void*)str_arr);
        return NULL;
    }

    *str_arr_len_out = count;
    return str_arr;
}

int rect_dx(RECT *r)
{
    int dx = r->right - r->left;
    assert(dx >= 0);
    return dx;
}

int rect_dy(RECT *r)
{
    int dy = r->bottom - r->top;
    assert(dy >= 0);
    return dy;
}

/* the main is there. We simulate a unix command line by parsing the
   windows command line */
int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrevInst, 
                   LPSTR lpszCmdLine, int nCmdShow)
{
   char **  argv;
   int      argc;
   char *   command_line;
   int      result;

   command_line = malloc(strlen(lpszCmdLine) + sizeof(PROG_NAME) + 1);
   if (!command_line)
       return 1;
   strcpy(command_line, PROG_NAME " ");
   strcat(command_line, lpszCmdLine);
   g_hprevinst = hPrevInst;
   _hInstance = hInstance;

    argv = str_to_str_array(command_line, &argc, 0);
    if (!argv)
        return 1;
    result = main1(argc, argv);
    free((void*)command_line);
    free((void*)argv);
    return result;
}

#if 1
#define WIN_EDGE_LEFT 0
#define WIN_EDGE_RIGHT 0
#else
#define WIN_EDGE_LEFT 8
#define WIN_EDGE_RIGHT 8
#endif
#define WIN_EDGE_TOP 0
#define WIN_EDGE_BOTTOM 0

static int win_probe(void)
{
    return 1;
}

static void init_application(void)
{
    WNDCLASS wc;
    
    wc.style = 0;
    wc.lpfnWndProc = qe_wnd_proc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = _hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "qemacs";
    
    RegisterClass(&wc);
}

static void free_double_buffer_data(void)
{
    if (win_ctx.bmp_double_buf) {
        DeleteObject(win_ctx.bmp_double_buf);
        win_ctx.bmp_double_buf = NULL;
    }

    if (win_ctx.hdc_double_buffer) {
        DeleteDC(win_ctx.hdc_double_buffer);
        win_ctx.hdc_double_buffer = NULL;
    }
    win_ctx.hdc = win_ctx.hdc_orig;
    win_ctx.double_buffer = FALSE;
}
    
static void create_double_buffer_bitmap(HWND hwnd)
{
    RECT    r;
    QEmacsState *   qs = &qe_state;

    assert(hwnd == win_ctx.hwnd);

    free_double_buffer_data();

    win_ctx.hdc_double_buffer = CreateCompatibleDC(win_ctx.hdc_orig);
    if (!win_ctx.hdc_double_buffer)
        return;

    GetClientRect(win_ctx.hwnd, &r);
    win_ctx.bmp_dx = rect_dx(&r);
    win_ctx.bmp_dy = rect_dy(&r);

    qs->screen->width = rect_dx(&r) - (WIN_EDGE_LEFT + WIN_EDGE_RIGHT);
    qs->screen->height = rect_dy(&r) - (WIN_EDGE_TOP + WIN_EDGE_BOTTOM);

    win_ctx.bmp_double_buf = CreateCompatibleBitmap(win_ctx.hdc_orig, win_ctx.bmp_dx, win_ctx.bmp_dy);
    if (!win_ctx.bmp_double_buf) {
        free_double_buffer_data();
        return;
    }
    /* TODO: do I need this ? */    
    SelectObject(win_ctx.hdc_double_buffer, win_ctx.font);
    SelectObject(win_ctx.hdc_double_buffer, win_ctx.bmp_double_buf);
    win_ctx.hdc = win_ctx.hdc_double_buffer;
    win_ctx.double_buffer = TRUE;
}

static void create_double_buffer_bitmap_if_needed(HWND hwnd)
{
    RECT    r;
    assert(hwnd == win_ctx.hwnd);

    GetClientRect(win_ctx.hwnd, &r);
    if ((win_ctx.bmp_dx == rect_dx(&r)) && (win_ctx.bmp_dy == rect_dy(&r)))
        return;

    create_double_buffer_bitmap(hwnd);
}

static void show_double_buffer(void)
{
    if (!win_ctx.double_buffer)
        return;
    assert(win_ctx.hdc == win_ctx.hdc_double_buffer);
    BitBlt(win_ctx.hdc_orig, 0, 0, win_ctx.bmp_dx, win_ctx.bmp_dy, win_ctx.hdc, 0, 0, SRCCOPY);
}

#define SEP_ITEM "-----"

#define IDM_FILE_NEW                400
#define IDM_FILE_OPEN               401
#define IDM_FILE_CLOSE              402
#define IDM_FILE_EXIT               403
#define IDM_VIEW_TOGGLE_LINES       404

typedef struct MenuDef {
    const char *    title;
    int             id;
} MenuDef;

MenuDef menu_file[] = {
    { "&New",               IDM_FILE_NEW },
    { "&Open\tCtrl-O",      IDM_FILE_OPEN },
    { "&Close\tCtrl-W",     IDM_FILE_CLOSE },
    { "E&xit\tCtrl-Q",      IDM_FILE_EXIT }
};

MenuDef menu_view[] = {
    { "Show new lines",     IDM_VIEW_TOGGLE_LINES }
};

static int str_eq(const char *s1, const char *s2)
{
    return 0 == strcmp(s1, s2);
}

static HMENU menu_from_def(MenuDef menuDefs[], int menuItems)
{
    int i;
    HMENU m = CreateMenu();
    if (NULL == m) 
        return NULL;

    for (i=0; i < menuItems; i++) {
        MenuDef md = menuDefs[i];
        const char *title = md.title;
        if (!title)
            continue; // the menu item was dynamically removed
        if (str_eq(title, SEP_ITEM)) {
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            continue;
        }
        // TODO: utf8 -> WCHAR if necessary
        AppendMenu(m, MF_STRING, (UINT_PTR)md.id, title);
    }
    return m;
}

static HMENU create_menu()
{
    HMENU menu = CreateMenu();
    HMENU tmp = menu_from_def(menu_file, dimof(menu_file));
    AppendMenu(menu, MF_POPUP | MF_STRING, (UINT_PTR)tmp, "&File");
    tmp = menu_from_def(menu_view, dimof(menu_view));
    AppendMenu(menu, MF_POPUP | MF_STRING, (UINT_PTR)tmp, "&View");
    return menu;
}

static void set_menu_show_lines_states(HMENU menu, int show)
{
    UINT check = show ? MF_BYCOMMAND | MF_CHECKED : MF_BYCOMMAND | MF_UNCHECKED;
    CheckMenuItem(menu, IDM_VIEW_TOGGLE_LINES, check);
}

static int win_init(QEditScreen *s, int w, int h)
{
    int         xsize, ysize, font_ysize;
    TEXTMETRIC  tm;
    HDC         hdc;
    HWND        desktop_hwnd;
    int         font_size_win;
    HFONT       font_prev;
    
    if (!g_hprevinst) 
        init_application();

    memcpy(&s->dpy, &win32_dpy, sizeof(QEDisplay));

    s->private_data = NULL;
    s->media = CSS_MEDIA_SCREEN;

    /* get font metric for window size */
    desktop_hwnd = GetDesktopWindow();
    hdc = GetDC(desktop_hwnd);

    font_size_win = -MulDiv(DEFAULT_FONT_SIZE, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    win_ctx.font = CreateFont(font_size_win, 0, 0, 0, 0 , 0, 0, 0, 0, 0, 0, 0, 
                              DEFAULT_PITCH, DEFAULT_FONT_NAME);

    win_ctx.bmp_dx = 0;
    win_ctx.bmp_dy = 0;

    font_prev = SelectObject(hdc, win_ctx.font);
    GetTextMetrics(hdc, &tm);
    SelectObject(hdc, font_prev);
    ReleaseDC(desktop_hwnd, hdc);

    font_xsize = tm.tmAveCharWidth;
    font_ysize = tm.tmHeight;

    xsize = DEFAULT_WIN_DX * font_xsize;
    ysize = DEFAULT_WIN_DY * font_ysize;

    s->width = xsize;
    s->height = ysize;
    s->charset = &charset_utf8;

    s->clip_x1 = 0;
    s->clip_y1 = 0;
    s->clip_x2 = s->width;
    s->clip_y2 = s->height;

    xsize += WIN_EDGE_LEFT + WIN_EDGE_RIGHT;
    ysize += WIN_EDGE_TOP + WIN_EDGE_BOTTOM;

    win_ctx.hwnd = CreateWindow("qemacs", "qemacs", WS_OVERLAPPEDWINDOW, 
                             0, 0, xsize, ysize, NULL, NULL, _hInstance, NULL);
    win_ctx.hdc_orig = GetDC(win_ctx.hwnd);
    /* SetBkColor(win_ctx.hdc_orig, RGB(255,0,0)); */
    win_ctx.hmenu = create_menu();
    SetMenu(win_ctx.hwnd, win_ctx.hmenu);
    SelectObject(win_ctx.hdc_orig, win_ctx.font);

    /*    SetWindowPos (win_ctx.hwnd, NULL, 0, 0, xsize, ysize, SWP_NOMOVE); */
    DragAcceptFiles(win_ctx.hwnd, TRUE);
    ShowWindow(win_ctx.hwnd, SW_SHOW);
    UpdateWindow(win_ctx.hwnd);
    return 0;
}

static void win_close(QEditScreen *s)
{
    DragAcceptFiles(win_ctx.hwnd, FALSE);
    free_double_buffer_data();
    ReleaseDC(win_ctx.hwnd, win_ctx.hdc_orig);
    DestroyWindow(win_ctx.hwnd);
    DeleteObject(win_ctx.font);
}

static void win_flush(QEditScreen *s)
{
    show_double_buffer();
}

static int win_is_user_input_pending(QEditScreen *s)
{
    /* XXX: do it */
    return 0;
}

static int is_expose_or_update_event(QEEvent *ev)
{
    if (QE_EXPOSE_EVENT == ev->type)
        return 1;
    if (QE_UPDATE_EVENT == ev->type)
        return 1;
    return 0;
}

static void push_event(QEEvent *ev)
{
    QEEventQ *e;
    QEEventQ *cur_event;

    /* don't push multiple expose/update events since they're redundant */
    if (is_expose_or_update_event(ev))
    {
        for (cur_event = first_event; cur_event != NULL; cur_event = cur_event->next)
        {
                if (QE_EXPOSE_EVENT == cur_event->ev.type)
                    return;
                if ((QE_UPDATE_EVENT == cur_event->ev.type) && (QE_UPDATE_EVENT == ev->type))
                    return;
        }
    }

    e = malloc(sizeof(QEEventQ));
    if (!e)
        return;
    e->ev = *ev;
    e->next = NULL;
    if (!last_event)
        first_event = e;
    else
        last_event->next = e;
    last_event = e;
}

static void push_key(int key)
{
    QEEvent ev;
    ev.type = QE_KEY_EVENT;
    ev.key_event.key = key;
    push_event(&ev);
}

static void queue_expose(void)
{
    QEEvent ev;
    ev.type = QE_EXPOSE_EVENT;
    push_event(&ev);
}

void on_paint(HWND hwnd)
{
    QEmacsState *   qs = &qe_state;
    PAINTSTRUCT     ps;
    HDC             saved_hdc;
    QEEvent         ev1, *ev = &ev1;

    assert(hwnd == win_ctx.hwnd);

    create_double_buffer_bitmap_if_needed(hwnd);
    BeginPaint(win_ctx.hwnd, &ps);
    saved_hdc = win_ctx.hdc;
    SelectObject(win_ctx.hdc, win_ctx.font);

    ev->expose_event.type = QE_EXPOSE_EVENT;
    qe_handle_event(ev);
    show_double_buffer();
    EndPaint(win_ctx.hwnd, &ps);
    win_ctx.hdc = saved_hdc;
}

static void win_invalidate(HWND hwnd)
{
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
}

void on_size(WPARAM wParam, LPARAM lParam)
{
    if (wParam == SIZE_MINIMIZED)
        return;
    win_invalidate(win_ctx.hwnd);
}

void on_activate(void)
{
    win_invalidate(win_ctx.hwnd);
}

static void on_drop_files(HDROP hDrop)
{
    int         i;
    char        filename[MAX_PATH];
    const int   files_count = DragQueryFile(hDrop, DRAGQUERY_NUMFILES, 0, 0);
    for (i = 0; i < files_count; i++)
    {
        DragQueryFile(hDrop, i, filename, MAX_PATH);
        /* TODO: not sure about the qe_state.active_window */
        do_load(qe_state.active_window, filename);
    }
    DragFinish(hDrop);
    if (files_count > 0)
        queue_expose();
}

extern void do_toggle_line_numbers(EditState *s);

static int ignore_wchar_msg = 0;


LRESULT CALLBACK qe_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    QEEvent         ev;
    UINT            linesPerScroll;
    static int      wheelDelta = 0;   // Wheel delta from scroll
    int             linesToScroll;
    int             cmdid;

    switch (msg) {
    case WM_CREATE:
        /* NOTE: must store them here to avoid problems in main */
        win_ctx.hwnd = hwnd;
        return 0;
        
        /* key handling */
    case WM_CHAR:
        if (!ignore_wchar_msg) {
            push_key(wParam);
        } else {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        break;

    case WM_ERASEBKGND:
        return TRUE;

    case WM_MOUSEWHEEL:
        /* ignore datazoom */
        if (wParam & MK_SHIFT)
            return DefWindowProc(hwnd, msg, wParam, lParam);

        /* ignore zoom */
        if (wParam & MK_CONTROL)
            return DefWindowProc(hwnd, msg, wParam, lParam);

        wheelDelta -= (short)HIWORD(wParam);

        if (!SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &linesPerScroll, 0)) {
            linesPerScroll = 3;    /* default value */
        }

        if (0 == linesPerScroll)
            return 0;

        if (abs(wheelDelta) >= WHEEL_DELTA)
        {
            /* TODO: a hack. It seems that 2/-2 in do_scroll_up_down means
            scrolling by a page. Not a very good interface */
            if (linesPerScroll == WHEEL_PAGESCROLL)
            {
                do_scroll_up_down(qe_state.active_window, wheelDelta > 0 ? 2 : -2);
                queue_expose();
                return 0;
            }

            linesToScroll = linesPerScroll;
            if (0 == linesToScroll)
                linesToScroll = 1;
            linesToScroll *= (wheelDelta / WHEEL_DELTA);
            if (wheelDelta >= 0)
                wheelDelta = wheelDelta % WHEEL_DELTA;
            else
                wheelDelta = - (-wheelDelta % WHEEL_DELTA);

            /* TODO: another hack: skip 2/-2 */
            if (2 == linesToScroll)
                ++linesToScroll;
            else if (-2 == linesToScroll)
                --linesToScroll;
            
            do_scroll_up_down(qe_state.active_window, linesToScroll);
            queue_expose();
        }
        break;

    case WM_SYSCHAR:
        if (!ignore_wchar_msg) {
            int key;
            key = wParam;
            if (key >= ' ' && key <= '~') {
                key = KEY_META(' ') + key - ' ';
                push_key(key);
                break;
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);

    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        {
            unsigned int scan;
            int ctrl, shift, alt, key;
            
            ctrl = (GetKeyState(VK_CONTROL) & 0x8000);
            shift = (GetKeyState(VK_SHIFT) & 0x8000);
            alt = (GetKeyState(VK_MENU) & 0x8000);
            
            ignore_wchar_msg = 0;
            
            scan = (unsigned int) ((lParam >> 16) & 0x1FF);
            switch (scan) {
            case 0x00E:
                ignore_wchar_msg = 1;
                push_key(KEY_DEL);
                break;
            case 0x039: /* space */
                ignore_wchar_msg = 1;
                if (!ctrl) 
                    push_key(KEY_SPC);
                else
                    push_key(KEY_CTRL('@'));
                break;
            case 0x147:                        /* home */
                push_key(KEY_HOME);
                break;
            case 0x148:                /* UP */
                if (shift)
                    push_key(KEY_SHIFT_UP);
                else
                    push_key(KEY_UP);
                break;
            case 0x149:                /* PGUP */
                push_key(KEY_PAGEUP);
                break;
            case 0x14B:                /* LEFT */
                if (shift)
                    push_key(KEY_SHIFT_LEFT);
                else
                    push_key(KEY_LEFT);
                break;
            case 0x14D:                /* RIGHT */
                if (shift)
                    push_key(KEY_SHIFT_RIGHT);
                else
                    push_key(KEY_RIGHT);
                break;
            case 0x14F:                /* END */
                push_key(KEY_END);
                break;
            case 0x150:                /* DOWN */
                if (shift)
                    push_key(KEY_SHIFT_DOWN);
                else
                    push_key(KEY_DOWN);
                break;
            case 0x151:                /* PGDN */
                push_key(KEY_PAGEDOWN);
                break;
            case 0x153:                /* DEL */
                push_key(KEY_DELETE);
                break;
            case 0x152:                /* INSERT */
                push_key(KEY_INSERT);
                break;
            case 0x3b:                 /* F1 */
            case 0x3c:
            case 0x3d:
            case 0x3e:
            case 0x3f:
            case 0x40:
            case 0x41:
            case 0x42:
            case 0x43:
            case 0x44:
            case 0x57:
            case 0x58:                 /* F12 */
                key = scan - 0x3b;
                if (key > 9)
                    key -= 0x12;
                key += KEY_F1;
                /* we leave Alt-F4 to close the window */
                if (alt && key == KEY_F4)
                    return DefWindowProc(hwnd, msg, wParam, lParam);
                push_key(key);
                break;
                  
            default: 
                return DefWindowProc(hwnd, msg, wParam, lParam);
            }
        }
        break;

    case WM_KEYUP:
        ignore_wchar_msg = 0;
        break;
          
    case WM_SYSKEYUP:
        ignore_wchar_msg = 0;
        return DefWindowProc(hwnd, msg, wParam, lParam);

    case WM_ACTIVATE:
        on_activate();
        break;

    case WM_SIZE:
        on_size(wParam, lParam);
        break;

    case WM_PAINT:
        on_paint(hwnd);
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    case WM_SETFOCUS:
        ShowCaret(win_ctx.hwnd);
        break;

    case WM_KILLFOCUS:
        HideCaret(win_ctx.hwnd);
        break;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        ev.button_event.type = QE_BUTTON_PRESS_EVENT;
        ev.button_event.button = QE_BUTTON_LEFT;
        if (WM_RBUTTONDOWN == msg) 
            ev.button_event.button = QE_BUTTON_RIGHT;
        ev.button_event.x = GET_X_LPARAM(lParam);
        ev.button_event.y = GET_Y_LPARAM(lParam);
        push_event(&ev);
        break;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        ev.button_event.type = QE_BUTTON_RELEASE_EVENT;
        ev.button_event.button = QE_BUTTON_LEFT;
        if (WM_RBUTTONUP == msg) 
            ev.button_event.button = QE_BUTTON_RIGHT;
        ev.button_event.x = GET_X_LPARAM(lParam);
        ev.button_event.y = GET_Y_LPARAM(lParam);
        push_event(&ev);
        break;

    case WM_MOUSEMOVE:
        ev.motion_event.type = QE_MOTION_EVENT;
        ev.motion_event.x = GET_X_LPARAM(lParam);
        ev.motion_event.y = GET_Y_LPARAM(lParam);
        push_event(&ev);
        break;

    case WM_DROPFILES:
        on_drop_files((HDROP)wParam);
        break;

    case WM_COMMAND:
        cmdid = LOWORD(wParam);
        switch (cmdid)
        {
            case IDM_FILE_NEW:
                //on_file_new();
                break;
            case IDM_FILE_OPEN:
                //on_file_open();
                break;
            case IDM_FILE_CLOSE:
                // on_file_close();
                break;
            case IDM_FILE_EXIT:
                SendMessage(hwnd, WM_CLOSE, 0, 0);                
                break;
            case IDM_VIEW_TOGGLE_LINES:
                do_toggle_line_numbers(qe_state.active_window);
                win_invalidate(win_ctx.hwnd);
                set_menu_show_lines_states(win_ctx.hmenu, qe_state.active_window->line_numbers);
                break;

            default:
                return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void win_fill_rectangle(QEditScreen *s,
                               int x1, int y1, int w, int h, QEColor color)
{
    RECT        rc;
    HBRUSH      hbr;
    COLORREF    col;

    if (QECOLOR_XOR == color)
    {
        SetRect(&rc, x1, y1, x1 + w, y1 + h);
        InvertRect(win_ctx.hdc, &rc);
        return;
    }

    col = RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
    SetRect(&rc, x1, y1, x1 + w, y1 + h);
    hbr = CreateSolidBrush(col);
    FillRect(win_ctx.hdc, &rc, hbr);
    DeleteObject(hbr);
}

static QEFont *win_open_font(QEditScreen *s, int style, int size)
{
    QEFont *    font;
    TEXTMETRIC  tm;

    font = (QEFont*)malloc(sizeof(QEFont));
    if (!font)
        return NULL;
    GetTextMetrics(win_ctx.hdc, &tm);
    font->refcount = 0;
    font->ascent = tm.tmAscent;
    font->descent = tm.tmDescent;
    font->private_data = NULL;
    return font;
}

static void win_close_font(QEditScreen *s, QEFont *font)
{
    free(font);
}

static void win_text_metrics(QEditScreen *s, QEFont *font, 
                             QECharMetrics *metrics,
                             const unsigned int *str, int len)
{
    int     i;
    WORD    bufStatic[BUF_STATIC_LEN];
    WORD *  buf = bufStatic;
    HDC     hdc = win_ctx.hdc;
    SIZE    txtSize;
    BOOL    fOk;

    metrics->font_ascent = font->ascent;
    metrics->font_descent = font->descent;
    metrics->width = 0;

    if (len > BUF_STATIC_LEN) {
        buf = (WORD*)malloc(len * sizeof(WORD));
        if (!buf)
            return;
    }
    for (i = 0; i < len; i++)
        buf[i] = str[i];

    fOk = GetTextExtentPoint32W(hdc, buf, len, &txtSize);
    assert(fOk);
    metrics->width = txtSize.cx;
    if (buf != bufStatic)
        free((void*)buf);
}

static inline COLORREF qecolor_to_wincolor(QEColor color)
{
    COLORREF win_color;
    win_color = RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
    return win_color;
}

/* TODO: it ignores 'font' and always uses default font */
static void win_draw_text(QEditScreen *s, QEFont *font,
                          int x, int y, const unsigned int *str, int len,
                          QEColor color)
{
    int     i;
    WORD    bufStatic[BUF_STATIC_LEN];
    WORD *  buf = bufStatic;
    COLORREF col;

    if (len > BUF_STATIC_LEN) {
        buf = (WORD*)malloc(len * sizeof(WORD));
        if (!buf)
            return;
    }
    for (i = 0; i < len; i++)
        buf[i] = str[i];
    buf[len] = 0;
    col = qecolor_to_wincolor(color);
    SetTextColor(win_ctx.hdc, col);
    SetBkMode(win_ctx.hdc, TRANSPARENT);
    TextOutW(win_ctx.hdc, x, y - font->ascent, buf, len);
    if (buf != bufStatic)
        free((void*)buf);
}

static void win_set_clip(QEditScreen *s,
                         int x, int y, int w, int h)
{
    /* nothing to do */
}

/* called during do_yank (i.e. copy or cut in windows GUI terms) after selection
   has been copied to a new yank buffer. We now need to copy the selection to
   the clipboard so that it's available to other apps and so that we can
   get it in win32_selection_request. Called from do_kill_region. */
static void win32_selection_activate(QEditScreen *s)
{
    QEmacsState * qs = &qe_state; /* TODO: is this how am I supposed to get it ? */
    EditBuffer *  yank_buf;
    HGLOBAL       h;
    char *        data;

    yank_buf = qs->yank_buffers[qs->yank_current];
    if (!yank_buf)
        return;

    if (!OpenClipboard(win_ctx.hwnd))
        return ;

    EmptyClipboard();

    /* TODO: assumes the buffer is ascii which is not necessary true. Should be
       much smarter and convert the content of yank_buf to Unicode, based on
       the encoding in yank_buf */
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, eb_total_size(yank_buf)+1);
    if (!h)
        goto exit;

    data = (char*)GlobalLock(h);
    eb_read(yank_buf, 0, (void*)data, eb_total_size(yank_buf));
    GlobalUnlock(h);
    SetClipboardData(CF_TEXT, h);

exit:
    CloseClipboard();
}

/* request the selection from the GUI and put it in a new yank buffer if needed.
   This is paste in windows terminology. Called from do_yank */
static void win32_selection_request(QEditScreen *s)
{
    EditBuffer *b;
    HGLOBAL     h;
    void *      clip_txt;
    char *      p;
    int         h_len, clip_len;

    if (!OpenClipboard(win_ctx.hwnd))
        return;

    h = GetClipboardData(CF_TEXT);
    if (!h)
    {
        /* TODO: try other formats as well, since it seems to be common
           that a selection is there, but not in CF_TEXT */
        log_nl("Win clipboard has no CF_TEXT");
        goto exit;
    }

    clip_txt = GlobalLock(h);
    if (!clip_txt)
        goto exit;

    h_len = GlobalSize(h);

    /* it looks like just believing the handle len causes to insert 2
       additional NULLs so I calculate the real size of the clipboard string */
    p = (char*) clip_txt;
    clip_len = 0;
    while (*p && clip_len < h_len)
    {
        ++clip_len;
        ++p;
    }
    b = new_yank_buffer();
    if (!b)
        goto exit;

    eb_write(b, 0, clip_txt, clip_len);
    GlobalUnlock(h);

exit:
    CloseClipboard();
}

void win_cursor_at(QEditScreen *s, int x1, int y1, int w, int h)
{
    static int     prev_curs_h = -1;
    static int     prev_curs_w = -1;

    w = 2;
    if ((prev_curs_h != h) || (prev_curs_w != w)) {
        if (-1 != prev_curs_h)
            DestroyCaret();
        CreateCaret(win_ctx.hwnd, (HBITMAP) NULL, w, h);
        prev_curs_h = h;
        prev_curs_w = w;
    }
    SetCaretPos(x1, y1);
}

static QEDisplay win32_dpy = {
    "win32",
    win_probe,
    win_init,
    win_close,
    // TODO: disabled win_cursor_at because it disappears
    // when quickly moving the cursor
    NULL, // win_cursor_at,
    win_flush,
    win_is_user_input_pending,
    win_fill_rectangle,
    win_open_font,
    win_close_font,
    win_text_metrics,
    win_draw_text,
    win_set_clip,
    win32_selection_activate,
    win32_selection_request,
    NULL, /* dpy_invalidate */
    NULL, /* dpy_bmp_alloc */
    NULL, /* dpy_bmp_free */
    NULL, /* dpy_bmp_draw */
    NULL, /* dpy_bmp_lock */
    NULL, /* dpy_bmp_unlock */
    NULL, /* dpy_full_screen */
    NULL /* next */
};

char cur_font_name[LF_FACESIZE]; 
int  cur_font_size;

void do_set_font(void)
{
    CHOOSEFONT  cf; 
    LOGFONT     lf; 
    BOOL        font_chosen;
    TEXTMETRIC  tm;

    bzero((void*)&lf, sizeof(lf));

    GetTextFace(win_ctx.hdc, LF_FACESIZE, lf.lfFaceName);
    GetTextMetrics(win_ctx.hdc, &tm);
    lf.lfHeight = tm.tmHeight;
 
    cf.lStructSize = sizeof(CHOOSEFONT); 
    cf.hwndOwner = (HWND)NULL; 
    cf.hDC = (HDC)NULL; 
    cf.lpLogFont = &lf; 
    cf.iPointSize = 0; 
    cf.Flags = CF_SCREENFONTS; 
    cf.rgbColors = RGB(0,0,0); 
    cf.lCustData = 0L; 
    cf.lpfnHook = (LPCFHOOKPROC)NULL; 
    cf.lpTemplateName = (LPSTR)NULL; 
    cf.hInstance = (HINSTANCE) NULL; 
    cf.lpszStyle = (LPSTR)NULL; 
    cf.nFontType = SCREEN_FONTTYPE; 
    cf.nSizeMin = 0; 
    cf.nSizeMax = 0; 
 
    font_chosen = ChooseFont(&cf);
    if (!font_chosen)
        return;

    /* TODO: probably need to update all existing QEFont with correct ascent/descent.
       This requires a completely different approach */
    win_ctx.font = CreateFontIndirect(cf.lpLogFont);
    SelectObject(win_ctx.hdc, (HGDIOBJ)win_ctx.font);
    win_invalidate(win_ctx.hwnd);
}

CmdDef win32_commands[] = {
    CMD0( KEY_NONE, KEY_NONE, "set-font", do_set_font)
    CMD_DEF_END,
};

int win32_init(void)
{
    qe_register_cmd_table(win32_commands, NULL);
    return qe_register_display(&win32_dpy);
}

void register_bottom_half(void (*cb)(void *opaque), void *opaque)
{
    assert(0);
}

/* return the next event in the queue in 'ev' */
static int get_next_event(QEEvent *ev)
{
    MSG         msg;
    QEEventQ *  e;

    for (;;) {
        /* check if events queued */
        if (first_event != NULL) {
            e = first_event;
            *ev = e->ev;
            first_event = e->next;
            if (!first_event)
                last_event = NULL;
            free(e);
            break;
        }

        /* check if message queued */
        if (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            url_exit();
            return 0;
        }
    }
    return 1;
}

void url_block(void)
{
    QEEvent ev;
    if (get_next_event(&ev))
        qe_handle_event(&ev);
}

void url_main_loop(void (*init)(void *opaque), void *opaque)
{
    init(opaque);
    for (;;) {
        if (url_exit_request)
            break;
        url_block();
    }
}

/* exit from url loop */
void url_exit(void)
{
    url_exit_request = 1;
}

qe_module_init(win32_init);
