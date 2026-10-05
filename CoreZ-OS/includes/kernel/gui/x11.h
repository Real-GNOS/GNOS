#ifndef GUI_X11_H
#define GUI_X11_H

#include "kernel/gui/server.h"
#include <stdint.h>

#define X11_PROTO_MAJOR 11
#define X11_PROTO_MINOR 0

#define X11_REQ_CreateWindow 1
#define X11_REQ_ChangeWindowAttributes 2
#define X11_REQ_GetWindowAttributes 3
#define X11_REQ_DestroyWindow 4
#define X11_REQ_DestroySubwindows 5
#define X11_REQ_MapWindow 8
#define X11_REQ_UnmapWindow 10
#define X11_REQ_ConfigureWindow 12
#define X11_REQ_GetGeometry 14
#define X11_REQ_QueryTree 15
#define X11_REQ_InternAtom 16
#define X11_REQ_ChangeProperty 18
#define X11_REQ_DeleteProperty 19
#define X11_REQ_GetProperty 20
#define X11_REQ_ListProperties 21
#define X11_REQ_QueryPointer 38
#define X11_REQ_TranslateCoordinates 40
#define X11_REQ_GetInputFocus 43
#define X11_REQ_SetInputFocus 42
#define X11_REQ_OpenFont 45
#define X11_REQ_CloseFont 46
#define X11_REQ_QueryTextExtents 48
#define X11_REQ_CreatePixmap 53
#define X11_REQ_FreePixmap 54
#define X11_REQ_CreateGC 55
#define X11_REQ_ChangeGC 56
#define X11_REQ_CopyGC 57
#define X11_REQ_SetDashes 58
#define X11_REQ_SetClipRectangles 59
#define X11_REQ_FreeGC 60
#define X11_REQ_ClearArea 61
#define X11_REQ_CopyArea 62
#define X11_REQ_PolyPoint 64
#define X11_REQ_PolyLine 65
#define X11_REQ_PolySegment 66
#define X11_REQ_PolyRectangle 67
#define X11_REQ_PolyArc 68
#define X11_REQ_FillPoly 69
#define X11_REQ_PolyFillRectangle 70
#define X11_REQ_PolyFillArc 71
#define X11_REQ_PutImage 72
#define X11_REQ_GetImage 73
#define X11_REQ_ImageText8 76
#define X11_REQ_CreateColormap 78
#define X11_REQ_FreeColormap 79
#define X11_REQ_InstallColormap 81
#define X11_REQ_AllocColor 84
#define X11_REQ_AllocNamedColor 85
#define X11_REQ_QueryColors 91
#define X11_REQ_LookupColor 92
#define X11_REQ_QueryExtension 98
#define X11_REQ_ListExtensions 99
#define X11_REQ_Bell 104
#define X11_REQ_SetScreenSaver 107
#define X11_REQ_GetScreenSaver 108
#define X11_REQ_SetAccessControl 111
#define X11_REQ_SetCloseDownMode 115
#define X11_REQ_KillClient 116
#define X11_REQ_NoOperation 127

#define X11_ERR_Request 1
#define X11_ERR_Value 2
#define X11_ERR_Window 3
#define X11_ERR_Pixmap 4
#define X11_ERR_Atom 5
#define X11_ERR_Drawable 9
#define X11_ERR_Font 7
#define X11_ERR_Match 8
#define X11_ERR_Alloc 11
#define X11_ERR_Colormap 12
#define X11_ERR_GContext 13
#define X11_ERR_Length 16

#define X11_EV_KEY_PRESS 2
#define X11_EV_KEY_RELEASE 3
#define X11_EV_BUTTON_PRESS 4
#define X11_EV_BUTTON_RELEASE 5
#define X11_EV_MOTION_NOTIFY 6
#define X11_EV_EXPOSE 12
#define X11_EV_MAP_NOTIFY 19
#define X11_EV_CONFIGURE_NOTIFY 22
#define X11_EV_CLIENT_MESSAGE 33

#define X11_CWBackPixmap 0x0001u
#define X11_CWBackPixel 0x0002u
#define X11_CWBorderPixmap 0x0004u
#define X11_CWBorderPixel 0x0008u
#define X11_CWBitGravity 0x0010u
#define X11_CWWinGravity 0x0020u
#define X11_CWBackingStore 0x0040u
#define X11_CWBackingPlanes 0x0080u
#define X11_CWBackingPixel 0x0100u
#define X11_CWOverrideRedirect 0x0200u
#define X11_CWSaveUnder 0x0400u
#define X11_CWEventMask 0x0800u
#define X11_CWDontPropagate 0x1000u
#define X11_CWColormap 0x2000u
#define X11_CWCursor 0x4000u

#define X11_GC_Function 0x000001u
#define X11_GC_PlaneMask 0x000002u
#define X11_GC_Foreground 0x000004u
#define X11_GC_Background 0x000008u
#define X11_GC_LineWidth 0x000010u
#define X11_GC_LineStyle 0x000020u
#define X11_GC_CapStyle 0x000040u
#define X11_GC_JoinStyle 0x000080u
#define X11_GC_FillStyle 0x000100u
#define X11_GC_FillRule 0x000200u
#define X11_GC_Tile 0x000400u
#define X11_GC_Stipple 0x000800u
#define X11_GC_ClipOrigin 0x002000u
#define X11_GC_ClipMask 0x004000u
#define X11_GC_DashOffset 0x008000u
#define X11_GC_DashList 0x010000u
#define X11_GC_ArcMode 0x020000u
#define X11_GC_SubwindowMode 0x040000u

#define X11_MAX_WINDOWS 8
#define X11_MAX_GCS 8
#define X11_MAX_PIXMAPS 8
#define X11_MAX_ATOMS 32
#define X11_MAX_PROPS 16
#define X11_MAX_CONNS 4
#define X11_REQ_BUF 8192
#define X11_OUT_BUF 16384
#define X11_ROOT_WINDOW 0x06000000u
#define X11_ROOT_VISUAL 0x00000021u
#define X11_DEFAULT_COLORMAP 0x06000020u

struct X11_PROP {
    uint32_t atom;
    uint32_t type;
    int used;
    uint8_t format;
    uint32_t nbytes;
    uint8_t data[64];
};

struct X11_WINDOW {
    uint32_t xid;
    int used;
    int mapped;
    int override_redirect;
    uint8_t depth, kind;
    int16_t x, y;
    uint16_t w, h, border;
    uint32_t bg_pixel;
    uint32_t bg_pixmap;
    uint32_t colormap;
    uint32_t event_mask;
    struct WL_SURFACE *s;
    struct WL_SHM_POOL *pool;
    struct X11_PROP props[X11_MAX_PROPS];
};

struct X11_GC {
    uint32_t gid;
    int used;
    uint32_t fg, bg;
    uint32_t line_width;
};

struct X11_PIXMAP {
    uint32_t pid;
    int used;
    uint32_t w, h;
    uint8_t depth;
    uint8_t *data;
    uint32_t size;
};

struct X11_CONN {
    int used;
    int dead;
    uint32_t seq;
    uint32_t client_id;

    uint8_t req[X11_REQ_BUF];
    uint32_t req_len;
    uint32_t req_need;

    uint8_t out[X11_OUT_BUF];
    uint32_t out_head, out_tail;
    struct SCHED_LOCK lock;

    struct X11_WINDOW win[X11_MAX_WINDOWS];
    struct X11_GC gc[X11_MAX_GCS];
    struct X11_PIXMAP pix[X11_MAX_PIXMAPS];
    struct {
        uint32_t atom;
        char name[24];
    } atoms[X11_MAX_ATOMS];
    int atom_n;
    
    uint32_t focus;
};

void x11_gateway_init(void);
struct X11_CONN *x11_conn_open(void);
void x11_conn_close(struct X11_CONN *c);
int x11_conn_feed(struct X11_CONN *c, const uint8_t *data, uint32_t len);
uint32_t x11_conn_drain(struct X11_CONN *c, uint8_t *buf, uint32_t len);
void x11_server_start(void);
void x11_notify_key(struct WL_SURFACE *s, int keycode, int pressed, int mods);
void x11_notify_button(struct WL_SURFACE *s, int x, int y, int button,
                       int pressed);
void x11_notify_motion(struct WL_SURFACE *s, int x, int y);

#endif
