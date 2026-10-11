/*
 * hexal_compiler.c - the Hexal compiler
 *
 * Translates Hexal (.hxl) source into C and builds a native Win32 (GDI32)
 * executable with GCC.  It is a blind translator: it knows no shapes, no
 * variable names and no game logic.  Everything visual lives in a Render
 * Queue at run time, addressed only by the Object IDs the Hexal program uses.
 *
 * Build the compiler (MinGW-w64 GCC on Windows):
 *     gcc hexal_compiler.c -o hexal_compiler.exe
 *
 * Use it:
 *     hexal_compiler program.hxl                 -> main.c + main.exe
 *     hexal_compiler program.hxl -o game.exe     -> game.c + game.exe
 *     hexal_compiler program.hxl --emit-only     -> only write the C file
 *     hexal_compiler program.hxl --console       -> keep a console window
 *
 * Basic I/O: print expr[, expr...]; println expr[, expr...]; input variable;
 * 2D physics: velocity ID VX VY; gravity ID ACCEL; friction ID FACTOR; physics ID;
 *             bounce ID1 ID2; collides("id1", "id2") is usable in conditions.
 * 3D commands (world Y is UP; the camera looks along +Z when yaw is 0):
 *     sky R G B;                       background colour
 *     fog DISTANCE;                    fade far blocks into the sky (0 = off)
 *     camera X Y Z;                    place the camera
 *     look YAW PITCH;  turn DYAW DPITCH;   degrees; +yaw = right, +pitch = up
 *     mouse_look [SENSITIVITY]; mouse_look off; capture mouse for FPS camera
 *     walk DIST forward|backward|left|right|up|down;   move the camera
 *     cube ID X Y Z SIZE R G B;        a true shaded cube (X Y Z = its centre)
 *     layer NAME X Y Z W D SIZE R G B; a W x D floor of cubes (auto IDs)
 *     remove ID;                       delete an object
 *     pick VAR;                        VAR = 1 if the crosshair is on a cube
 *     mine;                            remove the cube under the crosshair
 *     place R G B;                     build a cube on the face being looked at
 *     hud_2d ID X Y W H R G B;         flat rectangle drawn on top of the 3D
 *     when Key pressed X { ... }       fires once per press (not while held)
 *     Key names also include: mouse_left (click), mouse_right
 *   draw_3d (flat projected squares) still works exactly as before.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GNUC__
#define HX_FAIL_ATTR __attribute__((noreturn, format(printf, 3, 4)))
#define HX_EMIT_ATTR __attribute__((format(printf, 1, 2)))
#else
#define HX_FAIL_ATTR
#define HX_EMIT_ATTR
#endif

#define TOK_MAX 512            /* longest single token (strings included)   */
#define ARG_MAX 512            /* longest single command argument           */
#define NUM_MAX (ARG_MAX + 16) /* an argument wrapped in a C cast           */
#define EXPR_MAX 4096          /* longest expression / condition            */
#define PATH_MAX_LEN 1024
#define SYM_NAME_MAX 128
#define MAX_SYMS 2048
#define MAX_BLOCKS 512
#define MAX_STMT_TOKENS 256
#define MAX_INCLUDE_DEPTH 32
#define HX_ID_LIMIT 63

enum { TK_EOF = 0, TK_WORD, TK_STRING, TK_CHAR, TK_SYM };
enum { BK_PROGRAM = 0, BK_WRAP, BK_LOOP, BK_WHEN, BK_IF, BK_ELSE };
enum { ST_INT = 0, ST_FLOAT, ST_TEXT, ST_CHAR };

typedef struct {
    int type;
    int line;
    char text[TOK_MAX];
} Token;

typedef struct {
    char *buf;
    size_t len;
    size_t pos;
    int line;
    char path[PATH_MAX_LEN];
    int baseTop;   /* block depth when this file started (for '}' checks) */
    int hasPeek;
    Token peek;
} Lexer;

typedef struct {
    char name[SYM_NAME_MAX];
    int type;
} Symbol;

typedef struct {
    const char *name;
    const char *code;
} NameMap;

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

int hasLoop = 0;                       /* set when a 'loop' is translated */
static int hasWindow = 0;              /* set when 'Window' is translated */
static int programSeen = 0;
static int programClosed = 0;
static FILE *outFile = NULL;
static char outPath[PATH_MAX_LEN] = "";
static int blockKind[MAX_BLOCKS];
static int blockTop = 0;
static Symbol symTable[MAX_SYMS];
static int symCount = 0;
static char incStack[MAX_INCLUDE_DEPTH + 1][PATH_MAX_LEN];

static const NameMap keyNames[] = {
    {"space", "VK_SPACE"},   {"enter", "VK_RETURN"}, {"return", "VK_RETURN"},
    {"escape", "VK_ESCAPE"}, {"esc", "VK_ESCAPE"},   {"left", "VK_LEFT"},
    {"right", "VK_RIGHT"},   {"up", "VK_UP"},        {"down", "VK_DOWN"},
    {"shift", "VK_SHIFT"},   {"ctrl", "VK_CONTROL"}, {"control", "VK_CONTROL"},
    {"alt", "VK_MENU"},      {"tab", "VK_TAB"},      {"backspace", "VK_BACK"},
    {"mouse_left", "VK_LBUTTON"}, {"click", "VK_LBUTTON"}, {"mouse_right", "VK_RBUTTON"},
    {NULL, NULL}
};

static const NameMap dirNames[] = {
    {"up", "HX_UP"},           {"down", "HX_DOWN"},
    {"left", "HX_LEFT"},       {"right", "HX_RIGHT"},
    {"forward", "HX_FORWARD"}, {"backward", "HX_BACKWARD"},
    {NULL, NULL}
};

/* Fix: Removed duplicate 's' from static */
static const char *const reservedWords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "true", "false", "bool", "main",
    "Program", "Window", "window", "loop", "when", "move", "inject", "inf",
    "draw_2d", "draw_3d", "hex", "hexf", "text", "hext", "letter", "hexc",
    "and", "or", "not", "Key", "pressed", "camera", "look", "turn", "walk",
    "print", "println", "input", "velocity", "gravity", "physics", "friction", "bounce", "collides",
    "cube", "layer", "remove", "pick", "mine", "place", "hud_2d", "sky", "fog", "mouse_look", 
    "draw_text", "jump", NULL 
};


/* The C runtime that is written at the top of every generated file. */
static const char *const HX_RUNTIME[] = {
    "/* ===== Hexal runtime (generated by hexal_compiler) ===== */",
    "static double hx_c_vy = 0.0;",
    "static double hx_c_gravity = -0.012;",
    "static int hx_c_on_ground = 1;",
             
     
           
    "#define WIN32_LEAN_AND_MEAN",
    "#include <windows.h>",
    "#include <stdio.h>",
    "#include <stdlib.h>",
    "#include <string.h>",
    "#include <stdbool.h>",
    "#include <math.h>",
    "",
    "#define HX_TEXT_MAX 256",
    "#define HX_ID_MAX 64",
    "#define HX_MAX_OBJECTS 4096",
    "#define HX_CUBE_BASE 64.0",
    "",
    "enum { HX_KIND_NONE = 0, HX_KIND_RECT2D = 1, HX_KIND_OBJ3D = 2, HX_KIND_CUBE = 3, HX_KIND_HUD2D = 4 };",
    "enum { HX_UP = 0, HX_DOWN, HX_LEFT, HX_RIGHT, HX_FORWARD, HX_BACKWARD };",
    "",

    "typedef struct {",
    "    char id[HX_ID_MAX];",
    "    int kind;",
    "    double x, y, z, w, h;",
    "    double vx, vy, gravity, friction;",
    "    int r, g, b;",
    "} HxObject;",
    "",

    "/* The Render Queue: every object lives here, addressed only by its ID. */",
    "static HxObject hx_queue[HX_MAX_OBJECTS];",
    "static int hx_count = 0;",
    "static HWND hx_hwnd = NULL;",
    "static HDC hx_memdc = NULL;",
    "static HBITMAP hx_bmp = NULL;",
    "static HBITMAP hx_oldbmp = NULL;",
    "static int hx_width = 640;",
    "static int hx_height = 480;",
    "static int hx_running = 1;",
    "",
    "static inline void hx_strset(char *dst, size_t cap, const char *src) {",
    "    size_t n;",
    "    if (cap == 0) {",
    "        return;",
    "    }",
    "    n = strlen(src);",
    "    if (n > cap - 1) {",
    "        n = cap - 1;",
    "    }",
    "    memmove(dst, src, n);",
    "    dst[n] = '\\0';",
    "}",
    "",
    "static inline int hx_clamp8(double v) {",
    "    if (!(v > 0.0)) {",
    "        return 0;",
    "    }",
    "    if (v > 255.0) {",
    "        return 255;",
    "    }",
    "    return (int)v;",
    "}",
    "",
    "static inline int hx_iround(double v) {",
    "    if (v != v) {",
    "        return 0;",
    "    }",
    "    if (v > 100000.0) {",
    "        return 100000;",
    "    }",
    "    if (v < -100000.0) {",
    "        return -100000;",
    "    }",
    "    return (int)(v < 0.0 ? v - 0.5 : v + 0.5);",
    "}",
    "",
    "static inline HxObject *hx_find(const char *id) {",
    "    int i;",
    "    for (i = 0; i < hx_count; i++) {",
    "        if (strcmp(hx_queue[i].id, id) == 0) {",
    "            return &hx_queue[i];",
    "        }",
    "    }",
    "    return NULL;",
    "}",
    "",
    "static inline HxObject *hx_get(const char *id) {",
    "    HxObject *o = hx_find(id);",
    "    if (o != NULL) {",
    "        return o;",
    "    }",
    "    if (hx_count >= HX_MAX_OBJECTS) {",
    "        return NULL;",
    "    }",
    "    o = &hx_queue[hx_count++];",
    "    memset(o, 0, sizeof(*o));",
    "    hx_strset(o->id, HX_ID_MAX, id);",
    "    o->friction = 1.0;",
    "    return o;",
    "}",
    "",
    "static inline void hx_draw2d(const char *id, double x, double y, double w, double h,",
    "                             double r, double g, double b) {",
    "    HxObject *o = hx_get(id);",
    "    if (o == NULL) {",
    "        return;",
    "    }",
    "    o->kind = HX_KIND_RECT2D;",
    "    o->x = x;",
    "    o->y = y;",
    "    o->z = 0.0;",
    "    o->w = w;",
    "    o->h = h;",
    "    o->r = hx_clamp8(r);",
    "    o->g = hx_clamp8(g);",
    "    o->b = hx_clamp8(b);",
    "}",
    "",
    "static inline void hx_draw3d(const char *id, double x, double y, double z,",
    "                             double r, double g, double b) {",
    "    HxObject *o = hx_get(id);",
    "    if (o == NULL) {",
    "        return;",
    "    }",
    "    o->kind = HX_KIND_OBJ3D;",
    "    o->x = x;",
    "    o->y = y;",
    "    o->z = z;",
    "    o->w = 0.0;",
    "    o->h = 0.0;",
    "    o->r = hx_clamp8(r);",
    "    o->g = hx_clamp8(g);",
    "    o->b = hx_clamp8(b);",
    "}",
    "",
    "static inline void hx_move(const char *id, double px, int dir) {",
    "    HxObject *o = hx_find(id);",
    "    if (o == NULL) {",
    "        return;",
    "    }",
    "    if (o->kind == HX_KIND_CUBE) {",
    "        switch (dir) {",
    "        case HX_UP:       o->y += px; break;",
    "        case HX_DOWN:     o->y -= px; break;",
    "        case HX_LEFT:     o->x -= px; break;",
    "        case HX_RIGHT:    o->x += px; break;",
    "        case HX_FORWARD:  o->z += px; break;",
    "        case HX_BACKWARD: o->z -= px; break;",
    "        default: break;",
    "        }",
    "        return;",
        "    }",
    "    switch (dir) {",
    "    case HX_UP:       o->y -= px; break;",
    "    case HX_DOWN:     o->y += px; break;",
    "    case HX_LEFT:     o->x -= px; break;",
    "    case HX_RIGHT:    o->x += px; break;",
    "    case HX_FORWARD:  o->z -= px; break;",
    "    case HX_BACKWARD: o->z += px; break;",
    "    default: break;",
    "    }",
    "}",
    "",
    "static inline void hx_velocity(const char *id, double vx, double vy) {",
    "    HxObject *o = hx_find(id);",
    "    if (o != NULL) { o->vx = vx; o->vy = vy; }",
    "}",
    "static inline void hx_gravity(const char *id, double g) {",
    "    HxObject *o = hx_find(id);",
    "    if (o != NULL) { o->gravity = g; }",
    "}",
    "static inline void hx_physics(const char *id) {",
    "    HxObject *o = hx_find(id);",
    "    if (o == NULL || (o->kind != HX_KIND_RECT2D && o->kind != HX_KIND_HUD2D)) return;",
    "    o->vy += o->gravity;",
    "    o->x += o->vx;",
    "    o->y += o->vy;",
    "    if (o->friction < 0.0) o->friction = 0.0;",
    "    if (o->friction > 1.0) o->friction = 1.0;",
    "    o->vx *= o->friction;",
    "    o->vy *= o->friction;",
    "}",
    "static inline int hx_collides(const char *a, const char *b) {",
    "    const HxObject *x = hx_find(a), *y = hx_find(b);",
    "    if (x == NULL || y == NULL || x->kind != HX_KIND_RECT2D || y->kind != HX_KIND_RECT2D) return 0;",
    "    return x->x < y->x + y->w && x->x + x->w > y->x &&",
    "           x->y < y->y + y->h && x->y + x->h > y->y;",
    "}",
    "static inline void hx_friction(const char *id, double factor) {",
    "    HxObject *o = hx_find(id); if (o != NULL) o->friction = factor < 0.0 ? 0.0 : (factor > 1.0 ? 1.0 : factor);",
    "}",
    "static inline void hx_bounce(const char *a, const char *b) {",
    "    HxObject *x = hx_find(a), *y = hx_find(b); double ox, oy;",
    "    if (x == NULL || y == NULL || x->kind != HX_KIND_RECT2D || y->kind != HX_KIND_RECT2D) return;",
    "    if (!(x->x < y->x + y->w && x->x + x->w > y->x && x->y < y->y + y->h && x->y + x->h > y->y)) return;",
    "    ox = fmin(x->x + x->w, y->x + y->w) - fmax(x->x, y->x);",
    "    oy = fmin(x->y + x->h, y->y + y->h) - fmax(x->y, y->y);",
    "    if (ox < oy) { double push = ox / 2.0; if (x->x < y->x) { x->x -= push; y->x += push; } else { x->x += push; y->x -= push; } if (x->vx > 0.0 && x->x < y->x) x->vx = -fabs(x->vx); else if (x->vx < 0.0 && x->x > y->x) x->vx = fabs(x->vx); y->vx = -x->vx; }",
    "    else { double push = oy / 2.0; if (x->y < y->y) { x->y -= push; y->y += push; } else { x->y += push; y->y -= push; } if (x->vy > 0.0 && x->y < y->y) x->vy = -fabs(x->vy); else if (x->vy < 0.0 && x->y > y->y) x->vy = fabs(x->vy); y->vy = -x->vy; }",
    "}",
    "static inline int hx_read_int(int *v) { int ok = scanf(\"%d\", v) == 1; int ch; while ((ch = getchar()) != '\\n' && ch != EOF) {} return ok; }",
    "static inline int hx_read_double(double *v) { int ok = scanf(\"%lf\", v) == 1; int ch; while ((ch = getchar()) != '\\n' && ch != EOF) {} return ok; }",
    "static inline int hx_read_char(char *v) { int ok = scanf(\" %c\", v) == 1; int ch; while ((ch = getchar()) != '\\n' && ch != EOF) {} return ok; }",
    "static inline int hx_read_text(char *v, size_t cap) {",
    "    size_t n;",
    "    if (cap == 0 || fgets(v, (int)cap, stdin) == NULL) return 0;",
    "    n = strcspn(v, \"\\r\\n\"); v[n] = 0; return 1;",
    "}",
    "",
    "/*  CAMERA PHYSICS VARIABLES: Cleaned from redefinition error */",
    "",

    "/*  ΠΡΟΣΘΗΚΗ: Μηχανή Σχεδίασης Κειμένου και 3D Άλματος Κάμερας */",
    "static inline void hx_draw_text(const char *txt, double x, double y, double r, double g, double b) {",
    "    if (hx_memdc == NULL) return;",
    "    SetBkMode(hx_memdc, TRANSPARENT);",
    "    SetTextColor(hx_memdc, RGB(hx_clamp8(r), hx_clamp8(g), hx_clamp8(b)));",
    "    TextOutA(hx_memdc, hx_iround(x), hx_iround(y), txt, (int)strlen(txt));",
    "}",
    "static inline void hx_jump(double power) {",
    "    if (hx_c_on_ground) { hx_c_vy = power; hx_c_on_ground = 0; }",
    "}",
    "",
    "static inline int hx_key(int vk) {",
    "    if (vk >= 'a' && vk <= 'z') {",
    "        vk -= 32;",
    "    }",
    "    if (hx_hwnd != NULL && GetForegroundWindow() != hx_hwnd) {",
    "        return 0;",
    "    }",
    "    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;",
    "}",
    "",
    "static LRESULT CALLBACK hx_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {",
    "    switch (msg) {",
    "    case WM_ERASEBKGND:",
    "        return 1;",
    "    case WM_PAINT: {",
    "        PAINTSTRUCT ps;",
    "        HDC dc = BeginPaint(hwnd, &ps);",
    "        if (hx_memdc != NULL) {",
    "            BitBlt(dc, 0, 0, hx_width, hx_height, hx_memdc, 0, 0, SRCCOPY);",
    "        }",
    "        EndPaint(hwnd, &ps);",
    "        return 0;",
    "    }",

    "    case WM_DESTROY:",
    "        hx_running = 0;",
    "        PostQuitMessage(0);",
    "        return 0;",
    "    default:",
    "        break;",
    "    }",
    "    return DefWindowProcA(hwnd, msg, wp, lp);",
    "}",
    "",
    "static inline void hx_window(const char *title, int w, int h) {",
    "    WNDCLASSEXA wc;",
    "    RECT rc;",
    "    DWORD style;",
    "    HDC wdc;",
    "    if (hx_hwnd != NULL) {",
    "        return;",
    "    }",
    "    if (w < 1) {",
    "        w = 1;",
    "    }",
    "    if (h < 1) {",
    "        h = 1;",
    "    }",
    "    hx_width = w;",
    "    hx_height = h;",
    "    memset(&wc, 0, sizeof(wc));",
    "    wc.cbSize = sizeof(wc);",
    "    wc.style = CS_HREDRAW | CS_VREDRAW;",
    "    wc.lpfnWndProc = hx_wndproc;",
    "    wc.hInstance = GetModuleHandleA(NULL);",
    "    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);",
    "    wc.lpszClassName = \"HexalWindowClass\";",
    "    RegisterClassExA(&wc);",
    "    style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;",
    "    rc.left = 0;",
    "    rc.top = 0;",
    "    rc.right = w;",
    "    rc.bottom = h;",
    "    AdjustWindowRect(&rc, style, FALSE);",
    "    hx_hwnd = CreateWindowExA(0, \"HexalWindowClass\", title, style,",
    "                              CW_USEDEFAULT, CW_USEDEFAULT,",
    "                              rc.right - rc.left, rc.bottom - rc.top,",
    "                              NULL, NULL, wc.hInstance, NULL);",
    "    if (hx_hwnd == NULL) {",
    "        hx_running = 0;",
    "        return;",
    "    }",
    "    wdc = GetDC(hx_hwnd);",
    "    hx_memdc = CreateCompatibleDC(wdc);",
    "    hx_bmp = CreateCompatibleBitmap(wdc, w, h);",
    "    hx_oldbmp = (HBITMAP)SelectObject(hx_memdc, hx_bmp);",
    "    ReleaseDC(hx_hwnd, wdc);",
    "    ShowWindow(hx_hwnd, SW_SHOWNORMAL);",
    "    UpdateWindow(hx_hwnd);",
    "}",
    "",
    "static inline void hx_mouse_update(void);",
    "static inline void hx_pump(void) {",
    "    MSG msg;",
    "    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {",
    "        if (msg.message == WM_QUIT) {",
    "            hx_running = 0;",
    "            break;",
    "        }",
    "        TranslateMessage(&msg);",
    "        DispatchMessageA(&msg);",
    "    }",
    "    hx_mouse_update();",
    "}",
    "",
    "static inline int hx_alive(void) {",
    "    return hx_running;",
    "}",
    "",
    "static inline void hx_fill(int x, int y, int w, int h, int r, int g, int b) {",
    "    RECT rc;",
    "    HBRUSH br;",
        "    rc.left = x;",
    "    rc.top = y;",
    "    rc.right = x + w;",
    "    rc.bottom = y + h;",
    "    br = CreateSolidBrush(RGB(r, g, b));",
    "    FillRect(hx_memdc, &rc, br);",
    "    DeleteObject(br);",
    "}",
    "",
    "/* ---- Hexal 3D engine: camera, true shaded cubes, picking, HUD ---- */",
    "#define HX_NEAR 0.05",
    "#define HX_REACH 8.0",
    "static double hx_cx = 0.0, hx_cy = 0.0, hx_cz = 0.0;",
             /* 🟢 ΔΙΟΡΘΩΘΗΚΕ: Προσθήκη κάθετης ταχύτητας κάμερας */
     /* 🟢 ΔΙΟΡΘΩΘΗΚΕ: Προσθήκη βαρύτητας κάμερας */
           /* 🟢 ΔΙΟΡΘΩΘΗΚΕ: Προσθήκη ελέγχου εδάφους κάμερας */
    "static double hx_yaw = 0.0, hx_pitch = 0.0;",
    "static int hx_sky_r = 0, hx_sky_g = 0, hx_sky_b = 0;",
    "static double hx_fogd = 0.0;",
    "static int hx_pick_idx = -1;",
    "static int hx_pick_nx = 0, hx_pick_ny = 0, hx_pick_nz = 0;",
    "static int hx_auto_id = 0;",
    "static unsigned char hx_prevkey[256];",
    "static double hx_dist2[HX_MAX_OBJECTS];",
    "",
    "static inline double hx_rad(double deg) {",
    "    return deg * 3.14159265358979323846 / 180.0;",
    "}",
    "",
    "static inline void hx_camera(double x, double y, double z) {",
    "    hx_cx = x;",
    "    hx_cy = y;",
    "    hx_cz = z;",
    "}",
    "",

    "static inline void hx_look(double yaw, double pitch) {",
    "    yaw = fmod(yaw, 360.0);",
    "    if (pitch > 89.0) {",
    "        pitch = 89.0;",
    "    }",
    "    if (pitch < -89.0) {",
    "        pitch = -89.0;",
    "    }",
    "    hx_yaw = yaw;",
    "    hx_pitch = pitch;",
    "}",
    "",
    "static inline void hx_turn(double dyaw, double dpitch) {",
    "    hx_look(hx_yaw + dyaw, hx_pitch + dpitch);",
    "}",
    "",
    "static int hx_mouse_look_enabled = 0;",
    "static int hx_mouse_look_primed = 0;",
    "static int hx_mouse_cursor_hidden = 0;",
    "static double hx_mouse_sensitivity = 0.15;",
    "static inline void hx_mouse_look(double sensitivity) {",
    "    int was_enabled = hx_mouse_look_enabled;",
    "    hx_mouse_look_enabled = sensitivity > 0.0;",
    "    if (sensitivity > 0.0) hx_mouse_sensitivity = sensitivity;",
    "    hx_mouse_look_primed = 0;",
    "    if (was_enabled && !hx_mouse_look_enabled) {",
    "        ClipCursor(NULL);",
    "        if (hx_mouse_cursor_hidden) { ShowCursor(TRUE); hx_mouse_cursor_hidden = 0; }",
    "    }",
    "}",
    "static inline void hx_mouse_update(void) {",
    "    RECT rc; POINT center, pos;",
    "    if (!hx_mouse_look_enabled || hx_hwnd == NULL) return;",
    "    if (GetForegroundWindow() != hx_hwnd) {",
    "        if (hx_mouse_look_primed) {",
    "            ClipCursor(NULL);",
    "            if (hx_mouse_cursor_hidden) { ShowCursor(TRUE); hx_mouse_cursor_hidden = 0; }",
    "            hx_mouse_look_primed = 0;",
    "        }",
    "        return;",
    "    }",
    "    if (!GetClientRect(hx_hwnd, &rc)) return;",
    "    center.x = (rc.left + rc.right) / 2; center.y = (rc.top + rc.bottom) / 2;",
    "    ClientToScreen(hx_hwnd, &center);",
    "    if (!hx_mouse_look_primed) {",
    "        POINT tl, br; tl.x = rc.left; tl.y = rc.top; br.x = rc.right; br.y = rc.bottom;",
    "        ClientToScreen(hx_hwnd, &tl); ClientToScreen(hx_hwnd, &br);",
    "        rc.left = tl.x; rc.top = tl.y; rc.right = br.x; rc.bottom = br.y;",
    "        ClipCursor(&rc);",
    "        if (!hx_mouse_cursor_hidden) { ShowCursor(FALSE); hx_mouse_cursor_hidden = 1; }",
    "        SetCursorPos(center.x, center.y); hx_mouse_look_primed = 1; return;",
    "    }",
    "    if (GetCursorPos(&pos)) {",
    "        int dx = pos.x - center.x, dy = pos.y - center.y;",
    "        if (dx != 0 || dy != 0) hx_turn(dx * hx_mouse_sensitivity, -dy * hx_mouse_sensitivity);",
    "    }",
    "    SetCursorPos(center.x, center.y);",
    "}",
    "",
    "static inline void hx_sky(double r, double g, double b) {",
    "    hx_sky_r = hx_clamp8(r);",
    "    hx_sky_g = hx_clamp8(g);",
    "    hx_sky_b = hx_clamp8(b);",
    "}",
    "",
    "static inline void hx_fog(double distance) {",
    "    hx_fogd = distance > 0.0 ? distance : 0.0;",
    "}",
    "",
    "/* Camera axes: f = forward, r = right, u = up (world Y is up). */",
    "static inline void hx_basis(double *f, double *r, double *u) {",
    "    double sy = sin(hx_rad(hx_yaw)), cy = cos(hx_rad(hx_yaw));",
    "    double sp = sin(hx_rad(hx_pitch)), cp = cos(hx_rad(hx_pitch));",
    "    f[0] = sy * cp;  f[1] = sp;  f[2] = cy * cp;",
    "    r[0] = cy;       r[1] = 0.0; r[2] = -sy;",
    "    u[0] = -sy * sp; u[1] = cp;  u[2] = -cy * sp;",
    "}",
    "",
    "/* Moves the camera relative to where it is facing (level with the ground). */",
    "static inline void hx_walk(double px, int dir) {",
    "    double sy = sin(hx_rad(hx_yaw)), cy = cos(hx_rad(hx_yaw));",
    "    switch (dir) {",
    "    case HX_FORWARD:  hx_cx += sy * px; hx_cz += cy * px; break;",
    "    case HX_BACKWARD: hx_cx -= sy * px; hx_cz -= cy * px; break;",
    "    case HX_RIGHT:    hx_cx += cy * px; hx_cz -= sy * px; break;",
    "    case HX_LEFT:     hx_cx -= cy * px; hx_cz += sy * px; break;",
    "    case HX_UP:       hx_cy += px; break;",
    "    case HX_DOWN:     hx_cy -= px; break;",
    "    default: break;",
    "    }",
    "}",
    "",
    "static inline void hx_cube(const char *id, double x, double y, double z, double size,",
    "                           double r, double g, double b) {",
    "    HxObject *o = hx_get(id);",
    "    if (o == NULL) {",
    "        return;",
    "    }",
    "    o->kind = HX_KIND_CUBE;",
    "    o->x = x;",
    "    o->y = y;",
    "    o->z = z;",
    "    o->w = size > 0.0 ? size : 1.0;",
    "    o->h = 0.0;",
    "    o->r = hx_clamp8(r);",
    "    o->g = hx_clamp8(g);",
    "    o->b = hx_clamp8(b);",
    "}",
    "",
    "static inline void hx_layer(const char *prefix, double x0, double y, double z0, int w, int d,",
    "                            double size, double r, double g, double b) {",
    "    char id[HX_ID_MAX];",
    "    int i, j;",
    "    if (w > 64) {",
    "        w = 64;",
    "    }",
    "    if (d > 64) {",
    "        d = 64;",
    "    }",
    "    for (i = 0; i < w; i++) {",
    "        for (j = 0; j < d; j++) {",
    "            snprintf(id, sizeof id, \"%.40s_%d_%d\", prefix, i, j);",
    "            hx_cube(id, x0 + i * size, y, z0 + j * size, size, r, g, b);",
    "        }",
    "    }",
    "}",
    "",
    "static inline void hx_hud2d(const char *id, double x, double y, double w, double h,",
    "                            double r, double g, double b) {",
    "    HxObject *o;",
    "    hx_draw2d(id, x, y, w, h, r, g, b);",
    "    o = hx_find(id);",
    "    if (o != NULL) {",
    "        o->kind = HX_KIND_HUD2D;",
    "    }",
    "}",
    "",
    "static inline void hx_remove(const char *id) {",
    "    HxObject *o = hx_find(id);",
    "    if (o != NULL) {",
    "        if (hx_pick_idx >= 0 && o == &hx_queue[hx_pick_idx]) {",
    "            hx_pick_idx = -1;",
    "        }",
    "        o->kind = HX_KIND_NONE;",
    "    }",
    "}",
    "",
    "/* Casts a ray from the camera through the middle of the screen. */",
    "static inline int hx_pick(void) {",
    "    double f[3], r[3], u[3];",
    "    double org[3];",
    "    double best = HX_REACH + 1.0;",
    "    int i, a, hit = -1, hitAxis = 0, hitSign = 0;",
    "    hx_basis(f, r, u);",
    "    org[0] = hx_cx;",
    "    org[1] = hx_cy;",
    "    org[2] = hx_cz;",
    "    for (i = 0; i < hx_count; i++) {",
    "        const HxObject *q = &hx_queue[i];",
    "        double c[3], half, tmin = -1e30, tmax = 1e30;",
    "        int axis = -1, sign = 0, ok = 1;",
    "        if (q->kind != HX_KIND_CUBE) {",
    "            continue;",
    "        }",
    "        c[0] = q->x;",
    "        c[1] = q->y;",
    "        c[2] = q->z;",
    "        half = q->w / 2.0;",
    "        for (a = 0; a < 3 && ok; a++) {",
    "            double lo = c[a] - half, hi = c[a] + half, d = f[a];",
    "            if (fabs(d) < 1e-9) {",
    "                if (org[a] < lo || org[a] > hi) {",
    "                    ok = 0;",
    "                }",
    "            } else {",
    "                double t1 = (lo - org[a]) / d, t2 = (hi - org[a]) / d, tt;",
    "                if (t1 > t2) {",
    "                    tt = t1;",
    "                    t1 = t2;",
    "                    t2 = tt;",
    "                }",
    "                if (t1 > tmin) {",
    "                    tmin = t1;",
    "                    axis = a;",
    "                    sign = d > 0.0 ? -1 : 1;",
    "                }",
    "                if (t2 < tmax) {",
    "                    tmax = t2;",
    "                }",
    "                if (tmin > tmax) {",
    "                    ok = 0;",
    "                }",
    "            }",
    "        }",
    "        if (ok && axis >= 0 && tmin >= 0.0 && tmin <= HX_REACH && tmin < best) {",
    "            best = tmin;",
    "            hit = i;",
    "            hitAxis = axis;",
    "            hitSign = sign;",
    "        }",
    "    }",
    "    hx_pick_idx = hit;",
    "    hx_pick_nx = hitAxis == 0 ? hitSign : 0;",
    "    hx_pick_ny = hitAxis == 1 ? hitSign : 0;",
    "    hx_pick_nz = hitAxis == 2 ? hitSign : 0;",
    "    return hit >= 0;",
    "}",
    "",
    "static inline void hx_mine(void) {",
    "    if (hx_pick_idx >= 0 && hx_queue[hx_pick_idx].kind == HX_KIND_CUBE) {",
    "        hx_queue[hx_pick_idx].kind = HX_KIND_NONE;",
    "    }",
    "    hx_pick_idx = -1;",
    "}",
    "",
    "static inline void hx_place(double r, double g, double b) {",
    "    const HxObject *p;",
    "    double s, nx, ny, nz;",
    "    char id[HX_ID_MAX];",
    "    int i;",
    "    if (hx_pick_idx < 0) {",
    "        return;",
    "    }",
    "    p = &hx_queue[hx_pick_idx];",
    "    if (p->kind != HX_KIND_CUBE) {",
    "        return;",
    "    }",
    "    s = p->w;",
    "    nx = p->x + hx_pick_nx * s;",
    "    ny = p->y + hx_pick_ny * s;",
    "    nz = p->z + hx_pick_nz * s;",
    "    if (fabs(hx_cx - nx) < s * 0.5 + 0.3 && fabs(hx_cy - ny) < s * 0.5 + 0.3 &&",
    "        fabs(hx_cz - nz) < s * 0.5 + 0.3) {",
    "        return; /* never build inside the camera */",
    "    }",
    "    for (i = 0; i < hx_count; i++) {",
    "        const HxObject *q = &hx_queue[i];",
    "        if (q->kind == HX_KIND_CUBE && fabs(q->x - nx) < s * 0.25 && fabs(q->y - ny) < s * 0.25 &&",
    "            fabs(q->z - nz) < s * 0.25) {",
    "            return; /* that spot is already taken */",
    "        }",
    "    }",
    "    snprintf(id, sizeof id, \"~blk%d\", hx_auto_id++);",
    "    hx_cube(id, nx, ny, nz, s, r, g, b);",
    "}",
    "",
    "/* 'Key pressed X': true once per press, not every frame while held. */",
    "static inline int hx_keypress(int vk) {",
    "    int cur, idx = vk;",
    "    if (idx >= 'a' && idx <= 'z') {",
    "        idx -= 32;",
    "    }",
    "    cur = hx_key(vk);",
    "    if (idx < 0 || idx > 255) {",
    "        return cur;",
    "    }",
    "    {",
    "        int fired = cur && !hx_prevkey[idx];",
    "        hx_prevkey[idx] = (unsigned char)(cur ? 1 : 0);",
    "        return fired;",
    "    }",
    "}",
    "",
    "static int hx_cmp_far(const void *a, const void *b) {",
    "    double da = hx_dist2[*(const int *)a], db = hx_dist2[*(const int *)b];",
    "    return (da < db) - (da > db); /* far cubes first */",
    "}",
    "",
    "/* One face of a cube: camera space, near-plane clip, perspective, fill. */",
    "static void hx_face(const double c[4][3], const double *f, const double *r, const double *u,",
    "                    int cr, int cg, int cb, int pr, int pg, int pb, int pw) {",
    "    double in[4][3], out[8][3];",
    "    POINT pts[8];",
    "    HBRUSH br;",
    "    HPEN pen;",
    "    HGDIOBJ ob, op;",
    "    double focal = hx_height * 0.9;",
    "    int i, n = 0;",
    "    for (i = 0; i < 4; i++) {",
    "        double dx = c[i][0] - hx_cx, dy = c[i][1] - hx_cy, dz = c[i][2] - hx_cz;",
    "        in[i][0] = dx * r[0] + dy * r[1] + dz * r[2];",
    "        in[i][1] = dx * u[0] + dy * u[1] + dz * u[2];",
    "        in[i][2] = dx * f[0] + dy * f[1] + dz * f[2];",
    "    }",
    "    for (i = 0; i < 4; i++) {",
    "        const double *a = in[i];",
    "        const double *b = in[(i + 1) % 4];",
    "        int ain = a[2] >= HX_NEAR, bin = b[2] >= HX_NEAR;",
    "        if (ain) {",
    "            out[n][0] = a[0];",
    "            out[n][1] = a[1];",
    "            out[n][2] = a[2];",
    "            n++;",
    "        }",
    "        if (ain != bin) {",
    "            double t = (HX_NEAR - a[2]) / (b[2] - a[2]);",
    "            out[n][0] = a[0] + (b[0] - a[0]) * t;",
    "            out[n][1] = a[1] + (b[1] - a[1]) * t;",
    "            out[n][2] = HX_NEAR;",
    "            n++;",
    "        }",
    "    }",
    "    if (n < 3) {",
    "        return;",
    "    }",
    "    for (i = 0; i < n; i++) {",
    "        pts[i].x = hx_iround(hx_width / 2.0 + out[i][0] / out[i][2] * focal);",
    "        pts[i].y = hx_iround(hx_height / 2.0 - out[i][1] / out[i][2] * focal);",
    "    }",
    "    br = CreateSolidBrush(RGB(cr, cg, cb));",
    "    pen = CreatePen(PS_SOLID, pw, RGB(pr, pg, pb));",
    "    ob = SelectObject(hx_memdc, br);",
    "    op = SelectObject(hx_memdc, pen);",
    "    Polygon(hx_memdc, pts, n);",
    "    SelectObject(hx_memdc, ob);",
    "    SelectObject(hx_memdc, op);",
    "    DeleteObject(br);",
    "    DeleteObject(pen);",
    "}",
    "",
    "static void hx_draw_cube(int idx, const double *f, const double *r, const double *u) {",
    "    const HxObject *o = &hx_queue[idx];",
    "    double h = o->w / 2.0;",
    "    double c[3];",
    "    double fogT = 0.0;",
    "    int axis, sg;",
    "    c[0] = o->x;",
    "    c[1] = o->y;",
    "    c[2] = o->z;",
    "    if (hx_fogd > 0.0) {",
    "        fogT = sqrt(hx_dist2[idx]) / hx_fogd;",
    "        if (fogT > 1.0) {",
    "            fogT = 1.0;",
    "        }",
    "        fogT *= 0.92;",
    "    }",
    "    for (axis = 0; axis < 3; axis++) {",
    "        for (sg = -1; sg <= 1; sg += 2) {",
    "            double n[3] = {0.0, 0.0, 0.0};",
    "            double corners[4][3];",
    "            double shade;",
    "            int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3, k;",
    "            int cr, cg, cb, pr, pg, pb, pw = 1;",
    "            n[axis] = (double)sg;",
    "            /* back faces: the camera must be outside this face */",
    "            if ((hx_cx - c[0]) * n[0] + (hx_cy - c[1]) * n[1] + (hx_cz - c[2]) * n[2] <= h) {",
    "                continue;",
    "            }",
    "            for (k = 0; k < 4; k++) {",
    "                double s1 = (k == 1 || k == 2) ? h : -h;",
    "                double s2 = (k >= 2) ? h : -h;",
    "                corners[k][axis] = c[axis] + sg * h;",
    "                corners[k][a1] = c[a1] + s1;",
    "                corners[k][a2] = c[a2] + s2;",
    "            }",
    "            if (axis == 1) {",
    "                shade = sg > 0 ? 1.0 : 0.5;",
    "            } else if (axis == 2) {",
    "                shade = 0.8;",
    "            } else {",
    "                shade = 0.6;",
    "            }",
    "            cr = hx_iround(o->r * shade + (hx_sky_r - o->r * shade) * fogT);",
    "            cg = hx_iround(o->g * shade + (hx_sky_g - o->g * shade) * fogT);",
    "            cb = hx_iround(o->b * shade + (hx_sky_b - o->b * shade) * fogT);",
    "            pr = hx_iround(cr * 0.7);",
    "            pg = hx_iround(cg * 0.7);",
    "            pb = hx_iround(cb * 0.7);",
    "            if (idx == hx_pick_idx) {",
    "                pr = 255;",
    "                pg = 255;",
    "                pb = 255;",
    "                pw = 2;",
    "            }",
    "            hx_face((const double (*)[3])corners, f, r, u, cr, cg, cb, pr, pg, pb, pw);",
    "        }",
    "    }",
    "}",
    "",
       "static inline void hx_render(void) {",
    "    int order[HX_MAX_OBJECTS];",
    "    int cubes[HX_MAX_OBJECTS];",
    "    int i, j, cur, n3 = 0, nc = 0;",
    "    double f[3], r[3], u[3];",
    "    RECT full;",
    "    HBRUSH bg;",
    "    HDC wdc;",
    "    if (hx_hwnd == NULL || hx_memdc == NULL) {",
    "        return;",
    "    }",
    "    full.left = 0;",
    "    full.top = 0;",
    "    full.right = hx_width;",
    "    full.bottom = hx_height;",
    "    bg = CreateSolidBrush(RGB(hx_sky_r, hx_sky_g, hx_sky_b));",
    "    FillRect(hx_memdc, &full, bg);",
    "    DeleteObject(bg);",
    "    hx_basis(f, r, u);",
    "",
    "    /* 🧠 3D CAMERA PHYSICS: Εφαρμογή βαρύτητας και ανίχνευση εδάφους */",
    "    if (hx_cy > 2.0) {",
    "        hx_c_vy += hx_c_gravity;",
    "        hx_c_on_ground = 0;",
    "    } else {",
    "        hx_cy = 2.0;",
    "        hx_c_vy = 0.0;",
    "        hx_c_on_ground = 1;",
    "    }",
    "    hx_cy += hx_c_vy;",
    "",
    "    for (i = 0; i < hx_count; i++) {",
    "        const HxObject *o = &hx_queue[i];",
    "        if (o->kind == HX_KIND_RECT2D) {",
    "            hx_fill(hx_iround(o->x), hx_iround(o->y), hx_iround(o->w), hx_iround(o->h),",
    "                    o->r, o->g, o->b);",
    "        } else if (o->kind == HX_KIND_OBJ3D) {",
    "            order[n3++] = i;",
    "        } else if (o->kind == HX_KIND_CUBE) {",
    "            double dx = o->x - hx_cx, dy = o->y - hx_cy, dz = o->z - hx_cz;",
    "            hx_dist2[i] = dx * dx + dy * dy + dz * dz;",
    "            if (dx * f[0] + dy * f[1] + dz * f[2] < -o->w * 1.8) {",
    "                continue; /* completely behind the camera */",
    "            }",
    "            cubes[nc++] = i;",
    "        }",
    "    }",
    "    qsort(cubes, (size_t)nc, sizeof(int), hx_cmp_far);",
    "    for (i = 0; i < nc; i++) {",
    "        hx_draw_cube(cubes[i], f, r, u);",
    "    }",
    "    for (i = 1; i < n3; i++) {",
    "        cur = order[i];",
    "        j = i - 1;",
    "        while (j >= 0 && hx_queue[order[j]].z < hx_queue[cur].z) {",
    "            order[j + 1] = order[j];",
    "            j--;",
    "        }",
    "        order[j + 1] = cur;",
    "    }",
    "    for (i = 0; i < n3; i++) {",
    "        const HxObject *o = &hx_queue[order[i]];",
    "        double s, sx, sy;",
    "        int side;",
    "        if (o->z <= 0.001) {",
    "            continue;",
    "        }",
    "        s = HX_CUBE_BASE / o->z;",
    "        sx = hx_width / 2.0 + o->x / o->z;",
    "        sy = hx_height / 2.0 + o->y / o->z;",
    "        side = hx_iround(s);",
    "        if (side < 1) {",
    "            side = 1;",
    "        }",
    "        hx_fill(hx_iround(sx - s / 2.0), hx_iround(sy - s / 2.0), side, side, o->r, o->g, o->b);",
    "    }",
    "    for (i = 0; i < hx_count; i++) {",
    "        const HxObject *o = &hx_queue[i];",
    "        if (o->kind == HX_KIND_HUD2D) {",
    "            hx_fill(hx_iround(o->x), hx_iround(o->y), hx_iround(o->w), hx_iround(o->h),",
    "                    o->r, o->g, o->b);",
    "        }",
    "    }",
    "    wdc = GetDC(hx_hwnd);",
    "    BitBlt(wdc, 0, 0, hx_width, hx_height, hx_memdc, 0, 0, SRCCOPY);",
    "    ReleaseDC(hx_hwnd, wdc);",
    "}",
    "",
    "static inline void hx_idle(void) {",
    "    while (hx_running) {",
    "        hx_pump();",
    "        hx_render();",
    "        Sleep(16);",
    "    }",
    "}",
    "",
    "static inline void hx_shutdown(void) {",
    "    ClipCursor(NULL);",
    "    if (hx_mouse_cursor_hidden) { ShowCursor(TRUE); hx_mouse_cursor_hidden = 0; }",
    "    if (hx_memdc != NULL) {",
    "        SelectObject(hx_memdc, hx_oldbmp);",
    "        DeleteObject(hx_bmp);",
    "        DeleteDC(hx_memdc);",
    "        hx_memdc = NULL;",
    "    }",
    "    if (hx_hwnd != NULL) {",
    "        DestroyWindow(hx_hwnd);",
    "        hx_hwnd = NULL;",
    "    }",
    "}",
    "/* ===== end of Hexal runtime ===== */",
};


/* ------------------------------------------------------------------ */
/* Errors and small string helpers                                    */
/* ------------------------------------------------------------------ */

static void fail_at(const char *file, int line, const char *fmt, ...) HX_FAIL_ATTR;

static void fail_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (outFile != NULL) {
        fclose(outFile);
        outFile = NULL;
        if (outPath[0] != '\0') {
            remove(outPath);
        }
    }
    exit(1);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (cap == 0) {
        return;
    }
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int ci_equal(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static const char *map_lookup(const NameMap *table, const char *name) {
    int i;
    for (i = 0; table[i].name != NULL; i++) {
        if (ci_equal(table[i].name, name)) {
            return table[i].code;
        }
    }
    return NULL;
}

static void str_append(const Lexer *lx, int line, char *dst, size_t cap, const char *s) {
    size_t a = strlen(dst);
    size_t b = strlen(s);
    if (a + b + 1 > cap) {
        fail_at(lx->path, line, "expression is too long");
    }
    memcpy(dst + a, s, b + 1);
}

/* Appends s to dst, separated from earlier content by one space. */
static void piece(const Lexer *lx, int line, char *dst, size_t cap, const char *s) {
    if (dst[0] != '\0') {
        str_append(lx, line, dst, cap, " ");
    }
    str_append(lx, line, dst, cap, s);
}

static void make_literal(const Lexer *lx, int line, char *dst, size_t cap,
                         const char *quote, const char *text) {
    dst[0] = '\0';
    str_append(lx, line, dst, cap, quote);
    str_append(lx, line, dst, cap, text);
    str_append(lx, line, dst, cap, quote);
}

/* Appends one token, translated to C, to an expression under construction. */
static void tok_append(const Lexer *lx, char *dst, size_t cap, const Token *t) {
    char tmp[TOK_MAX + 8];
    const char *txt = t->text;
    switch (t->type) {
    case TK_STRING:
        make_literal(lx, t->line, tmp, sizeof tmp, "\"", txt);
        txt = tmp;
        break;
    case TK_CHAR:
        make_literal(lx, t->line, tmp, sizeof tmp, "'", txt);
        txt = tmp;
        break;
    case TK_WORD:
        if (strcmp(txt, "collides") == 0) {
            txt = "hx_collides";
        } else if (strcmp(txt, "and") == 0) {
            txt = "&&";
        } else if (strcmp(txt, "or") == 0) {
            txt = "||";
        } else if (strcmp(txt, "not") == 0) {
            txt = "!";
        }
        break;
    default:
        break;
    }
    piece(lx, t->line, dst, cap, txt);
}

static void join_tokens(const Lexer *lx, const Token *toks, int from, int to,
                        char *dst, size_t cap) {
    int i;
    dst[0] = '\0';
    for (i = from; i < to; i++) {
        tok_append(lx, dst, cap, &toks[i]);
    }
}

static int is_sym(const Token *t, const char *s) {
    return t->type == TK_SYM && strcmp(t->text, s) == 0;
}

/* ------------------------------------------------------------------ */
/* Symbol table                                                       */
/* ------------------------------------------------------------------ */

static int sym_lookup(const char *name) {
    int i;
    for (i = 0; i < symCount; i++) {
        if (strcmp(symTable[i].name, name) == 0) {
            return symTable[i].type;
        }
    }
    return -1;
}

static void sym_declare(const Lexer *lx, int line, const char *name, int type) {
    int i;
    for (i = 0; i < symCount; i++) {
        if (strcmp(symTable[i].name, name) == 0) {
            fail_at(lx->path, line, "variable '%s' is already declared", name);
        }
    }
    if (symCount >= MAX_SYMS) {
        fail_at(lx->path, line, "too many variables (limit %d)", MAX_SYMS);
    }
    copy_str(symTable[symCount].name, SYM_NAME_MAX, name);
    symTable[symCount].type = type;
    symCount++;
}

static void check_ident(const Lexer *lx, int line, const char *name) {
    size_t i;
    int r;
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        fail_at(lx->path, line, "'%s' is not a valid variable name", name);
    }
    for (i = 0; name[i] != '\0'; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            fail_at(lx->path, line, "'%s' is not a valid variable name", name);
        }
    }
    if (strlen(name) >= SYM_NAME_MAX) {
        fail_at(lx->path, line, "variable name '%s' is too long", name);
    }
    if (tolower((unsigned char)name[0]) == 'h' && tolower((unsigned char)name[1]) == 'x' &&
        name[2] == '_') {
        fail_at(lx->path, line, "names starting with 'hx_' are reserved for the runtime");
    }
    for (r = 0; reservedWords[r] != NULL; r++) {
        if (strcmp(reservedWords[r], name) == 0) {
            fail_at(lx->path, line, "'%s' is a reserved word and cannot be a variable name", name);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

static void emit(const char *fmt, ...) HX_EMIT_ATTR;

static void emit(const char *fmt, ...) {
    va_list ap;
    int i;
    for (i = 0; i < blockTop; i++) {
        fputs("    ", outFile);
    }
    va_start(ap, fmt);
    vfprintf(outFile, fmt, ap);
    va_end(ap);
}

static void emit_preamble(void) {
    size_t i;
    fputs("/* Generated by hexal_compiler - do not edit. */\n", outFile);
    for (i = 0; i < sizeof(HX_RUNTIME) / sizeof(HX_RUNTIME[0]); i++) {
        fputs(HX_RUNTIME[i], outFile);
        fputc('\n', outFile);
    }
    fputc('\n', outFile);
}

static void push_block(const Lexer *lx, int line, int kind) {
    if (blockTop >= MAX_BLOCKS) {
        fail_at(lx->path, line, "blocks are nested too deeply (limit %d)", MAX_BLOCKS);
    }
    blockKind[blockTop++] = kind;
}

/* A '}' in Hexal becomes exactly one '}' in C (plus loop/program epilogues). */
static void close_block(const Lexer *lx, int line) {
    int kind;
    if (blockTop <= lx->baseTop) {
        fail_at(lx->path, line, "unmatched '}'");
    }
    kind = blockKind[blockTop - 1];
    switch (kind) {
    case BK_LOOP:
        emit("hx_render();\n");
        emit("Sleep(16);\n");
        blockTop--;
        emit("}\n");
        break;
    case BK_PROGRAM:
        if (hasWindow && !hasLoop) {
            emit("hx_idle();\n");
        }
        emit("hx_shutdown();\n");
        emit("return 0;\n");
        blockTop--;
        emit("}\n");
        programClosed = 1;
        break;
    case BK_WRAP:
        blockTop--;
        break;
    default:
        blockTop--;
        emit("}\n");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Lexer                                                              */
/* ------------------------------------------------------------------ */

static void lex_open(Lexer *lx, const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    size_t got;
    if (f == NULL) {
        fail_at(path, 1, "cannot open file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fail_at(path, 1, "cannot read file");
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fail_at(path, 1, "cannot read file");
    }
    lx->buf = (char *)malloc((size_t)size + 1);
    if (lx->buf == NULL) {
        fclose(f);
        fail_at(path, 1, "out of memory");
    }
    got = fread(lx->buf, 1, (size_t)size, f);
    fclose(f);
    lx->buf[got] = '\0';
    lx->len = got;
    lx->pos = 0;
    lx->line = 1;
    lx->hasPeek = 0;
    lx->baseTop = blockTop;
    copy_str(lx->path, sizeof lx->path, path);
    if (got >= 3 && (unsigned char)lx->buf[0] == 0xEF && (unsigned char)lx->buf[1] == 0xBB &&
        (unsigned char)lx->buf[2] == 0xBF) {
        lx->pos = 3; /* skip UTF-8 byte order mark */
    }
}

static void tok_put(const Lexer *lx, Token *t, size_t *n, char c) {
    if (*n >= TOK_MAX - 1) {
        fail_at(lx->path, t->line, "token is too long (limit %d characters)", TOK_MAX - 1);
    }
    t->text[(*n)++] = c;
}

static void lex_raw(Lexer *lx, Token *t) {
    const char *s = lx->buf;
    size_t n = lx->len;
    size_t len = 0;
    char c;
    char quote;

    for (;;) {
        while (lx->pos < n && isspace((unsigned char)s[lx->pos])) {
            if (s[lx->pos] == '\n') {
                lx->line++;
            }
            lx->pos++;
        }
        if (lx->pos + 1 < n && s[lx->pos] == '/' && s[lx->pos + 1] == '/') {
            while (lx->pos < n && s[lx->pos] != '\n') {
                lx->pos++;
            }
            continue;
        }
        if (lx->pos + 1 < n && s[lx->pos] == '/' && s[lx->pos + 1] == '*') {
            int startLine = lx->line;
            lx->pos += 2;
            while (lx->pos < n && !(s[lx->pos] == '*' && lx->pos + 1 < n && s[lx->pos + 1] == '/')) {
                if (s[lx->pos] == '\n') {
                    lx->line++;
                }
                lx->pos++;
            }
            if (lx->pos >= n) {
                fail_at(lx->path, startLine, "unterminated /* comment");
            }
            lx->pos += 2;
            continue;
        }
        break;
    }

    t->line = lx->line;
    t->text[0] = '\0';
    if (lx->pos >= n) {
        t->type = TK_EOF;
        return;
    }
    c = s[lx->pos];

    if (c == '"' || c == '\'') {
        quote = c;
        t->type = (c == '"') ? TK_STRING : TK_CHAR;
        lx->pos++;
        for (;;) {
            if (lx->pos >= n || s[lx->pos] == '\n') {
                fail_at(lx->path, t->line, "unterminated %s literal",
                        quote == '"' ? "string" : "character");
            }
            c = s[lx->pos];
            if (c == quote) {
                lx->pos++;
                break;
            }
            if (c == '\\') {
                if (lx->pos + 1 >= n || s[lx->pos + 1] == '\n') {
                    fail_at(lx->path, t->line, "bad escape at end of literal");
                }
                tok_put(lx, t, &len, c);
                tok_put(lx, t, &len, s[lx->pos + 1]);
                lx->pos += 2;
                continue;
            }
            tok_put(lx, t, &len, c);
            lx->pos++;
        }
        t->text[len] = '\0';
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        t->type = TK_WORD;
        while (lx->pos < n && (isalnum((unsigned char)s[lx->pos]) || s[lx->pos] == '_')) {
            tok_put(lx, t, &len, s[lx->pos]);
            lx->pos++;
        }
        t->text[len] = '\0';
        return;
    }

    if (isdigit((unsigned char)c) ||
        (c == '.' && lx->pos + 1 < n && isdigit((unsigned char)s[lx->pos + 1]))) {
        int isHex = (c == '0' && lx->pos + 1 < n && (s[lx->pos + 1] == 'x' || s[lx->pos + 1] == 'X'));
        t->type = TK_WORD;
        while (lx->pos < n) {
            c = s[lx->pos];
            if (isalnum((unsigned char)c) || c == '.' || c == '_') {
                tok_put(lx, t, &len, c);
                lx->pos++;
                if (!isHex && (c == 'e' || c == 'E') && lx->pos + 1 < n &&
                    (s[lx->pos] == '+' || s[lx->pos] == '-') && isdigit((unsigned char)s[lx->pos + 1])) {
                    tok_put(lx, t, &len, s[lx->pos]);
                    lx->pos++;
                }
                continue;
            }
            break;
        }
        t->text[len] = '\0';
        return;
    }

    t->type = TK_SYM;
    if (lx->pos + 1 < n) {
        static const char *const twoChar[] = {"==", "!=", "<=", ">=", "&&", "||", "++", "--",
                                              "+=", "-=", "*=", "/=", "%=", "<<", ">>", NULL};
        int i;
        for (i = 0; twoChar[i] != NULL; i++) {
            if (s[lx->pos] == twoChar[i][0] && s[lx->pos + 1] == twoChar[i][1]) {
                t->text[0] = twoChar[i][0];
                t->text[1] = twoChar[i][1];
                t->text[2] = '\0';
                lx->pos += 2;
                return;
            }
        }
    }
    if (strchr("{}()[];,=+-*/%<>!&|^~?:.", c) == NULL) {
        fail_at(lx->path, t->line, "unexpected character '%c'", c);
    }
    t->text[0] = c;
    t->text[1] = '\0';
    lx->pos++;
}

static void lex_next(Lexer *lx, Token *t) {
    if (lx->hasPeek) {
        *t = lx->peek;
        lx->hasPeek = 0;
        return;
    }
    lex_raw(lx, t);
}

static void lex_peek(Lexer *lx, Token *t) {
    if (!lx->hasPeek) {
        lex_raw(lx, &lx->peek);
        lx->hasPeek = 1;
    }
    *t = lx->peek;
}

static void skip_semicolon(Lexer *lx) {
    Token t;
    lex_peek(lx, &t);
    if (is_sym(&t, ";")) {
        lex_next(lx, &t);
    }
}

/* Reads statement tokens up to ';' (consumed) or a closing '}' (left alone). */
static int gather(Lexer *lx, Token *buf, int start, int cap) {
    int n = start;
    Token t;
    for (;;) {
        lex_peek(lx, &t);
        if (t.type == TK_EOF) {
            fail_at(lx->path, t.line, "missing ';' before end of file");
        }
        if (is_sym(&t, ";")) {
            lex_next(lx, &t);
            break;
        }
        if (is_sym(&t, "}")) {
            break;
        }
        if (is_sym(&t, "{")) {
            fail_at(lx->path, t.line, "unexpected '{' inside a statement");
        }
        if (n >= cap) {
            fail_at(lx->path, t.line, "statement is too long");
        }
        lex_next(lx, &buf[n]);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Argument helpers for the graphics / window commands                */
/* ------------------------------------------------------------------ */

/* One argument: a word/number, a string, a signed number or a (group). */
static void read_arg(Lexer *lx, char *out, size_t cap, int *type, const char *what) {
    Token t, t2;
    int depth;
    out[0] = '\0';
    lex_next(lx, &t);
    if (t.type == TK_WORD || t.type == TK_STRING || t.type == TK_CHAR) {
        copy_str(out, cap, t.text);
        *type = t.type;
        return;
    }
    if (is_sym(&t, "-") || is_sym(&t, "+")) {
        lex_next(lx, &t2);
        if (t2.type != TK_WORD) {
            fail_at(lx->path, t2.line, "expected a number after '%s' (%s)", t.text, what);
        }
        str_append(lx, t.line, out, cap, t.text);
        str_append(lx, t.line, out, cap, t2.text);
        *type = TK_WORD;
        return;
    }
    if (is_sym(&t, "(")) {
        depth = 1;
        str_append(lx, t.line, out, cap, "(");
        for (;;) {
            lex_next(lx, &t2);
            if (t2.type == TK_EOF || is_sym(&t2, ";") || is_sym(&t2, "{") || is_sym(&t2, "}")) {
                fail_at(lx->path, t2.line, "unterminated '(' in %s", what);
            }
            if (is_sym(&t2, "(")) {
                depth++;
            }
            if (is_sym(&t2, ")")) {
                depth--;
                if (depth == 0) {
                    str_append(lx, t2.line, out, cap, ")");
                    break;
                }
            }
            tok_append(lx, out, cap, &t2);
        }
        *type = TK_WORD;
        return;
    }
    fail_at(lx->path, t.line, "expected %s", what);
}

/* Reads a numeric argument and wraps it in a C cast, e.g. (double)(x). */
static void read_num(Lexer *lx, char *out, size_t cap, const char *what, const char *ctype) {
    char raw[ARG_MAX];
    int type = TK_WORD;
    read_arg(lx, raw, sizeof raw, &type, what);
    if (type == TK_STRING) {
        fail_at(lx->path, lx->line, "%s must be a number, not a string", what);
    }
    out[0] = '\0';
    str_append(lx, lx->line, out, cap, "(");
    str_append(lx, lx->line, out, cap, ctype);
    str_append(lx, lx->line, out, cap, ")(");
    str_append(lx, lx->line, out, cap, raw);
    str_append(lx, lx->line, out, cap, ")");
}

/* Object IDs are names: 7, player, "hero one" - all become C strings. */
static void id_expr(const Lexer *lx, const char *id, int type, char *out, size_t cap) {
    if (id[0] == '(') {
        fail_at(lx->path, lx->line, "an object ID must be a name, a number or a string");
    }
    if (type == TK_WORD && sym_lookup(id) == ST_TEXT) {
        copy_str(out, cap, id);
        return;
    }
    if (strlen(id) > HX_ID_LIMIT) {
        fail_at(lx->path, lx->line, "object ID is too long (limit %d characters)", HX_ID_LIMIT);
    }
    make_literal(lx, lx->line, out, cap, "\"", id);
}

/* The value of a 'letter': 'W', "W", W, a letter variable or a char code. */
static void letter_literal(const Lexer *lx, const Token *t, char *out, size_t cap) {
    size_t len = strlen(t->text);
    int st;
    switch (t->type) {
    case TK_CHAR:
        make_literal(lx, t->line, out, cap, "'", t->text);
        return;
    case TK_STRING:
        if (len == 1 && t->text[0] == '\'') {
            copy_str(out, cap, "'\\''");
        } else if (len == 1 || t->text[0] == '\\') {
            make_literal(lx, t->line, out, cap, "'", t->text);
        } else {
            fail_at(lx->path, t->line, "a letter holds exactly one character");
        }
        return;
    case TK_WORD:
        st = sym_lookup(t->text);
        if (st == ST_TEXT) {
            fail_at(lx->path, t->line, "cannot store text variable '%s' in a letter", t->text);
        }
        if (st >= 0) {
            copy_str(out, cap, t->text);
        } else if (len == 1 && isalnum((unsigned char)t->text[0])) {
            make_literal(lx, t->line, out, cap, "'", t->text);
        } else if (isdigit((unsigned char)t->text[0])) {
            copy_str(out, cap, t->text);
        } else {
            fail_at(lx->path, t->line, "unknown variable '%s' (a letter holds one character)", t->text);
        }
        return;
    default:
        fail_at(lx->path, t->line, "expected a letter value");
    }
}

/* The value of a 'text': a string literal or another text variable. */
static void text_operand(const Lexer *lx, const Token *t, char *out, size_t cap) {
    if (t->type == TK_STRING) {
        make_literal(lx, t->line, out, cap, "\"", t->text);
        return;
    }
    if (t->type == TK_WORD && sym_lookup(t->text) == ST_TEXT) {
        copy_str(out, cap, t->text);
        return;
    }
    fail_at(lx->path, t->line, "a text value must be a \"string\" or a text variable");
}

static void resolve_key(const Lexer *lx, const Token *t, char *out, size_t cap) {
    const char *vk;
    char tmp[8];
    size_t len = strlen(t->text);
    if (t->type == TK_WORD) {
        int st = sym_lookup(t->text);
        if (st == ST_CHAR || st == ST_INT) {
            out[0] = '\0';
            str_append(lx, t->line, out, cap, "(int)");
            str_append(lx, t->line, out, cap, t->text);
            return;
        }
        if (st >= 0) {
            fail_at(lx->path, t->line, "'%s' cannot be used as a key", t->text);
        }
    }
    if (t->type != TK_SYM && len == 1 && isalnum((unsigned char)t->text[0])) {
        tmp[0] = '\'';
        tmp[1] = (char)toupper((unsigned char)t->text[0]);
        tmp[2] = '\'';
        tmp[3] = '\0';
        copy_str(out, cap, tmp);
        return;
    }
    vk = map_lookup(keyNames, t->text);
    if (vk == NULL) {
        fail_at(lx->path, t->line,
                "unknown key '%s' (use a letter/digit, Space, Enter, Escape, Left, Right, Up, Down, "
                "Shift, Ctrl, Alt, Tab or Backspace)", t->text);
    }
    copy_str(out, cap, vk);
}

/* ------------------------------------------------------------------ */
/* Statements                                                         */
/* ------------------------------------------------------------------ */

static void parse_file(const char *path, int depth);

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

static int is_absolute(const char *p) {
    return p[0] == '/' || p[0] == '\\' || (isalpha((unsigned char)p[0]) && p[1] == ':');
}

/* Directory part of a path, including the trailing separator ("" if none). */
static void dir_of(const char *path, char *dir, size_t cap) {
    const char *a = strrchr(path, '\\');
    const char *b = strrchr(path, '/');
    const char *cut = a;
    if (b != NULL && (cut == NULL || b > cut)) {
        cut = b;
    }
    if (cut == NULL) {
        dir[0] = '\0';
        return;
    }
    {
        size_t n = (size_t)(cut - path) + 1;
        if (n > cap - 1) {
            n = cap - 1;
        }
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
}

static void stmt_program(Lexer *lx, int line) {
    Token t;
    lex_next(lx, &t);
    if (t.type != TK_WORD) {
        fail_at(lx->path, t.line, "expected a program name after 'Program'");
    }
    if (blockTop == 0) {
        if (strcmp(t.text, "main") != 0) {
            fail_at(lx->path, t.line, "the entry block must be 'Program main {'");
        }
        if (programSeen) {
            fail_at(lx->path, line, "only one 'Program main' block is allowed");
        }
    }
    lex_next(lx, &t);
    if (!is_sym(&t, "{")) {
        fail_at(lx->path, t.line, "expected '{' after 'Program main'");
    }
    if (blockTop == 0) {
        emit_preamble();
        emit("int main(void) {\n");
        push_block(lx, line, BK_PROGRAM);
        programSeen = 1;
    } else {
        /* A Program wrapper inside an injected file is transparent. */
        push_block(lx, line, BK_WRAP);
    }
}

static void stmt_decl(Lexer *lx, Token *stmt, const char *kw, int kind) {
    Token t;
    char name[TOK_MAX];
    char val[EXPR_MAX];
    char operand[EXPR_MAX];
    int n = 0;
    int hasInit = 0;
    int i;

    lex_next(lx, &t);
    if (t.type != TK_WORD) {
        fail_at(lx->path, t.line, "expected a variable name after '%s'", kw);
    }
    copy_str(name, sizeof name, t.text);
    check_ident(lx, t.line, name);

    lex_peek(lx, &t);
    if (is_sym(&t, "=")) {
        lex_next(lx, &t);
        n = gather(lx, stmt, 0, MAX_STMT_TOKENS);
        hasInit = 1;
        if (n == 0) {
            fail_at(lx->path, t.line, "missing value after '=' in declaration of '%s'", name);
        }
    } else if (is_sym(&t, ";")) {
        lex_next(lx, &t);
    } else if (!is_sym(&t, "}")) {
        fail_at(lx->path, t.line, "expected '=' or ';' after '%s %s'", kw, name);
    }

    switch (kind) {
    case ST_INT:
    case ST_FLOAT:
        if (hasInit) {
            for (i = 0; i < n; i++) {
                if (stmt[i].type == TK_STRING) {
                    fail_at(lx->path, stmt[i].line, "'%s' holds numbers; use 'text' for strings", kw);
                }
            }
            join_tokens(lx, stmt, 0, n, val, sizeof val);
        } else {
            copy_str(val, sizeof val, kind == ST_INT ? "0" : "0.0");
        }
        emit("%s %s = %s;\n", kind == ST_INT ? "int" : "double", name, val);
        break;
    case ST_TEXT:
        if (hasInit) {
            if (n != 1) {
                fail_at(lx->path, stmt[0].line, "a text value must be a single \"string\" or text variable");
            }
            text_operand(lx, &stmt[0], operand, sizeof operand);
        } else {
            copy_str(operand, sizeof operand, "\"\"");
        }
        emit("char %s[HX_TEXT_MAX];\n", name);
        emit("hx_strset(%s, sizeof(%s), %s);\n", name, name, operand);
        break;
    default: /* ST_CHAR */
        if (!hasInit) {
            copy_str(val, sizeof val, "'\\0'");
        } else if (n == 1) {
            letter_literal(lx, &stmt[0], val, sizeof val);
        } else {
            for (i = 0; i < n; i++) {
                if (stmt[i].type == TK_STRING) {
                    fail_at(lx->path, stmt[i].line, "a letter holds exactly one character");
                }
            }
            join_tokens(lx, stmt, 0, n, operand, sizeof operand);
            val[0] = '\0';
            str_append(lx, lx->line, val, sizeof val, "(char)(");
            str_append(lx, lx->line, val, sizeof val, operand);
            str_append(lx, lx->line, val, sizeof val, ")");
        }
        emit("char %s = %s;\n", name, val);
        break;
    }
    sym_declare(lx, t.line, name, kind);
}

static void stmt_window(Lexer *lx) {
    char title[ARG_MAX];
    char titleC[ARG_MAX + 8];
    char w[NUM_MAX];
    char h[NUM_MAX];
    int tt = TK_WORD;

    read_arg(lx, title, sizeof title, &tt, "a window title (a \"string\")");
    if (tt == TK_STRING) {
        make_literal(lx, lx->line, titleC, sizeof titleC, "\"", title);
    } else if (tt == TK_WORD && sym_lookup(title) == ST_TEXT) {
        copy_str(titleC, sizeof titleC, title);
    } else {
        fail_at(lx->path, lx->line, "the window title must be a \"string\" or a text variable");
    }
    read_num(lx, w, sizeof w, "the window width", "int");
    read_num(lx, h, sizeof h, "the window height", "int");
    skip_semicolon(lx);
    emit("hx_window(%s, %s, %s);\n", titleC, w, h);
    hasWindow = 1;
}

static void stmt_loop(Lexer *lx, int line) {
    char varName[ARG_MAX];
    Token t;
    int vt = TK_WORD;

    read_arg(lx, varName, sizeof varName, &vt, "a variable after 'loop'");
    if (vt == TK_STRING || (vt == TK_WORD && sym_lookup(varName) == ST_TEXT)) {
        fail_at(lx->path, line, "'loop' needs a hex variable (true/false), not text");
    }
    lex_next(lx, &t);
    if (t.type == TK_WORD && strcmp(t.text, "inf") == 0) {
        lex_next(lx, &t);
    }
    if (!is_sym(&t, "{")) {
        fail_at(lx->path, t.line, "expected 'inf {' after the loop variable");
    }
    emit("while (%s) {\n", varName);
    push_block(lx, line, BK_LOOP);
    emit("hx_pump();\n");
    emit("if (!hx_alive()) break;\n");
    hasLoop = 1;
}

/* Condition up to '{' (consumed).  'Key == X' becomes hx_key(X). */
static void parse_condition(Lexer *lx, char *cond, size_t cap, int line) {
    Token t, op, key;
    char keyExpr[ARG_MAX];
    char call[ARG_MAX + 16];
    int count = 0;

    cond[0] = '\0';
    for (;;) {
        lex_next(lx, &t);
        if (t.type == TK_EOF) {
            fail_at(lx->path, line, "missing '{' after condition");
        }
        if (is_sym(&t, "{")) {
            break;
        }
        if (is_sym(&t, ";") || is_sym(&t, "}")) {
            fail_at(lx->path, t.line, "expected '{' after condition");
        }
        if (t.type == TK_WORD && strcmp(t.text, "Key") == 0) {
            lex_peek(lx, &op);
            if (op.type == TK_WORD && strcmp(op.text, "pressed") == 0) {
                lex_next(lx, &op);
                lex_next(lx, &key);
                if (key.type == TK_EOF || key.type == TK_SYM) {
                    fail_at(lx->path, key.line, "expected a key after 'Key pressed'");
                }
                resolve_key(lx, &key, keyExpr, sizeof keyExpr);
                call[0] = '\0';
                str_append(lx, key.line, call, sizeof call, "hx_keypress(");
                str_append(lx, key.line, call, sizeof call, keyExpr);
                str_append(lx, key.line, call, sizeof call, ")");
                piece(lx, key.line, cond, cap, call);
                count++;
                continue;
            }
            if (is_sym(&op, "==") || is_sym(&op, "!=")) {
                int negate = (op.text[0] == '!');
                lex_next(lx, &op);
                lex_next(lx, &key);
                if (key.type == TK_EOF || key.type == TK_SYM) {
                    fail_at(lx->path, key.line, "expected a key after 'Key %s'", op.text);
                }
                resolve_key(lx, &key, keyExpr, sizeof keyExpr);
                call[0] = '\0';
                str_append(lx, key.line, call, sizeof call, negate ? "!hx_key(" : "hx_key(");
                str_append(lx, key.line, call, sizeof call, keyExpr);
                str_append(lx, key.line, call, sizeof call, ")");
                piece(lx, key.line, cond, cap, call);
                count++;
                continue;
            }
        }
        tok_append(lx, cond, cap, &t);
        count++;
    }
    if (count == 0) {
        fail_at(lx->path, line, "empty condition");
    }
}

static void stmt_cond(Lexer *lx, const char *kw, int line) {
    char cond[EXPR_MAX];
    Token t;

    if (strcmp(kw, "else") == 0 || strcmp(kw, "otherwise") == 0) {
        lex_next(lx, &t);
        if (is_sym(&t, "{")) {
            emit("else {\n");
            push_block(lx, line, BK_ELSE);
            return;
        }
        if (t.type == TK_WORD && strcmp(t.text, "if") == 0) {
            parse_condition(lx, cond, sizeof cond, line);
            emit("else if (%s) {\n", cond);
            push_block(lx, line, BK_IF);
            return;
        }
        fail_at(lx->path, t.line, "expected '{' or 'if' after 'else'");
    }
    parse_condition(lx, cond, sizeof cond, line);
    emit("if (%s) {\n", cond);
    push_block(lx, line, strcmp(kw, "when") == 0 ? BK_WHEN : BK_IF);
}

static void stmt_move(Lexer *lx) {
    char id[ARG_MAX];
    char idC[ARG_MAX + 8];
    char px[NUM_MAX];
    Token dirTok;
    const char *dirCode;
    int tid = TK_WORD;

    read_arg(lx, id, sizeof id, &tid, "an object ID after 'move'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, px, sizeof px, "the move distance", "double");
    lex_next(lx, &dirTok);
    if (dirTok.type != TK_WORD) {
        fail_at(lx->path, dirTok.line, "expected a direction (up, down, left, right, forward, backward)");
    }
    dirCode = map_lookup(dirNames, dirTok.text);
    if (dirCode == NULL) {
        fail_at(lx->path, dirTok.line,
                "unknown direction '%s' (use up, down, left, right, forward or backward)", dirTok.text);
    }
    skip_semicolon(lx);
    emit("hx_move(%s, %s, %s);\n", idC, px, dirCode);
}

static void stmt_draw2d(Lexer *lx) {
    char id[ARG_MAX];
    char idC[ARG_MAX + 8];
    char x[NUM_MAX];
    char y[NUM_MAX];
    char w[NUM_MAX];
    char h[NUM_MAX];
    char r[NUM_MAX];
    char g[NUM_MAX];
    char b[NUM_MAX];
    int tid = TK_WORD;

    read_arg(lx, id, sizeof id, &tid, "an object ID after 'draw_2d'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, x, sizeof x, "X", "double");
    read_num(lx, y, sizeof y, "Y", "double");
    read_num(lx, w, sizeof w, "Width", "double");
    read_num(lx, h, sizeof h, "Height", "double");
    read_num(lx, r, sizeof r, "R", "double");
    read_num(lx, g, sizeof g, "G", "double");
    read_num(lx, b, sizeof b, "B", "double");
    skip_semicolon(lx);
    emit("hx_draw2d(%s, %s, %s, %s, %s, %s, %s, %s);\n", idC, x, y, w, h, r, g, b);
}

static void stmt_draw3d(Lexer *lx) {
    char id[ARG_MAX];
    char idC[ARG_MAX + 8];
    char x[NUM_MAX];
    char y[NUM_MAX];
    char z[NUM_MAX];
    char r[NUM_MAX];
    char g[NUM_MAX];
    char b[NUM_MAX];
    int tid = TK_WORD;

    read_arg(lx, id, sizeof id, &tid, "an object ID after 'draw_3d'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, x, sizeof x, "X", "double");
    read_num(lx, y, sizeof y, "Y", "double");
    read_num(lx, z, sizeof z, "Z", "double");
    read_num(lx, r, sizeof r, "R", "double");
    read_num(lx, g, sizeof g, "G", "double");
    read_num(lx, b, sizeof b, "B", "double");
    skip_semicolon(lx);
    emit("hx_draw3d(%s, %s, %s, %s, %s, %s, %s);\n", idC, x, y, z, r, g, b);
}

/* ------------------------------------------------------------------ */
/* 3D statements: camera, cubes, picking, HUD                         */
/* ------------------------------------------------------------------ */

static void stmt_camera(Lexer *lx) {
    char x[NUM_MAX], y[NUM_MAX], z[NUM_MAX];
    read_num(lx, x, sizeof x, "camera X", "double");
    read_num(lx, y, sizeof y, "camera Y (height)", "double");
    read_num(lx, z, sizeof z, "camera Z", "double");
    skip_semicolon(lx);
    emit("hx_camera(%s, %s, %s);\n", x, y, z);
}

static void stmt_look(Lexer *lx, const char *fn) {
    char yaw[NUM_MAX], pitch[NUM_MAX];
    read_num(lx, yaw, sizeof yaw, "yaw (degrees)", "double");
    read_num(lx, pitch, sizeof pitch, "pitch (degrees)", "double");
    skip_semicolon(lx);
    emit("%s(%s, %s);\n", fn, yaw, pitch);
}

static void stmt_walk(Lexer *lx) {
    char px[NUM_MAX];
    Token dirTok;
    const char *dirCode;
    read_num(lx, px, sizeof px, "the walk distance", "double");
    lex_next(lx, &dirTok);
    if (dirTok.type != TK_WORD) {
        fail_at(lx->path, dirTok.line, "expected a direction (up, down, left, right, forward, backward)");
    }
    dirCode = map_lookup(dirNames, dirTok.text);
    if (dirCode == NULL) {
        fail_at(lx->path, dirTok.line, "unknown direction '%s'", dirTok.text);
    }
    skip_semicolon(lx);
    emit("hx_walk(%s, %s);\n", px, dirCode);
}

static void stmt_cube(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8];
    char x[NUM_MAX], y[NUM_MAX], z[NUM_MAX], s[NUM_MAX], r[NUM_MAX], g[NUM_MAX], b[NUM_MAX];
    int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "an object ID after 'cube'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, x, sizeof x, "X", "double");
    read_num(lx, y, sizeof y, "Y", "double");
    read_num(lx, z, sizeof z, "Z", "double");
    read_num(lx, s, sizeof s, "Size", "double");
    read_num(lx, r, sizeof r, "R", "double");
    read_num(lx, g, sizeof g, "G", "double");
    read_num(lx, b, sizeof b, "B", "double");
    skip_semicolon(lx);
    emit("hx_cube(%s, %s, %s, %s, %s, %s, %s, %s);\n", idC, x, y, z, s, r, g, b);
}

static void stmt_layer(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8];
    char x[NUM_MAX], y[NUM_MAX], z[NUM_MAX], w[NUM_MAX], d[NUM_MAX];
    char s[NUM_MAX], r[NUM_MAX], g[NUM_MAX], b[NUM_MAX];
    int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "a name prefix after 'layer'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, x, sizeof x, "X", "double");
    read_num(lx, y, sizeof y, "Y", "double");
    read_num(lx, z, sizeof z, "Z", "double");
    read_num(lx, w, sizeof w, "the width in blocks", "int");
    read_num(lx, d, sizeof d, "the depth in blocks", "int");
    read_num(lx, s, sizeof s, "Size", "double");
    read_num(lx, r, sizeof r, "R", "double");
    read_num(lx, g, sizeof g, "G", "double");
    read_num(lx, b, sizeof b, "B", "double");
    skip_semicolon(lx);
    emit("hx_layer(%s, %s, %s, %s, %s, %s, %s, %s, %s, %s);\n", idC, x, y, z, w, d, s, r, g, b);
}

static void stmt_remove(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8];
    int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "an object ID after 'remove'");
    id_expr(lx, id, tid, idC, sizeof idC);
    skip_semicolon(lx);
    emit("hx_remove(%s);\n", idC);
}

static void stmt_pick(Lexer *lx) {
    char name[ARG_MAX];
    int tt = TK_WORD;
    read_arg(lx, name, sizeof name, &tt, "a hex variable after 'pick'");
    if (tt != TK_WORD || sym_lookup(name) != ST_INT) {
        fail_at(lx->path, lx->line, "'pick' stores 1 or 0 in a hex variable; declare one first (hex hit = 0;)");
    }
    skip_semicolon(lx);
    emit("%s = hx_pick();\n", name);
}

static void stmt_place(Lexer *lx) {
    char r[NUM_MAX], g[NUM_MAX], b[NUM_MAX];
    read_num(lx, r, sizeof r, "R", "double");
    read_num(lx, g, sizeof g, "G", "double");
    read_num(lx, b, sizeof b, "B", "double");
    skip_semicolon(lx);
    emit("hx_place(%s, %s, %s);\n", r, g, b);
}

static void stmt_draw_text(Lexer *lx) {
    char txt[ARG_MAX], txtC[ARG_MAX + 8];
    char x[NUM_MAX], y[NUM_MAX], r[NUM_MAX], g[NUM_MAX], b[NUM_MAX];
    int tt = TK_WORD;

    read_arg(lx, txt, sizeof txt, &tt, "text or variable after 'draw_text'");
    read_num(lx, x, sizeof x, "X coordinate", "double");
    read_num(lx, y, sizeof y, "Y coordinate", "double");
    read_num(lx, r, sizeof r, "R color", "double");
    read_num(lx, g, sizeof g, "G color", "double");
    read_num(lx, b, sizeof b, "B color", "double");
    skip_semicolon(lx);

    if (tt == TK_STRING) {
        make_literal(lx, lx->line, txtC, sizeof txtC, "\"", txt);
        emit("hx_draw_text(%s, %s, %s, %s, %s, %s);\n", txtC, x, y, r, g, b);
    } else {
        int st = sym_lookup(txt);
        if (st == ST_TEXT) {
            emit("hx_draw_text(%s, %s, %s, %s, %s, %s);\n", txt, x, y, r, g, b);
        } else if (st == ST_INT || st == ST_FLOAT) {
            /* Low-level Magic: Φτιάχνουμε έναν δυναμικό buffer στη C, 
               μετατρέπουμε live τον αριθμό σε string και τον σχεδιάζουμε! */
            emit("{\n");
            emit("    char _buf[64];\n");
            if (st == ST_INT) {
                emit("    snprintf(_buf, sizeof(_buf), \"%%d\", %s);\n", txt);
            } else {
                emit("    snprintf(_buf, sizeof(_buf), \"%%g\", %s);\n", txt);
            }
            emit("    hx_draw_text(_buf, %s, %s, %s, %s, %s);\n", x, y, r, g, b);
            emit("}\n");
        } else {
            fail_at(lx->path, lx->line, "unknown variable '%s' in draw_text", txt);
        }
    }
}

static void stmt_hud2d(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8];
    char x[NUM_MAX], y[NUM_MAX], w[NUM_MAX], h[NUM_MAX], r[NUM_MAX], g[NUM_MAX], b[NUM_MAX];
    int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "an object ID after 'hud_2d'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, x, sizeof x, "X", "double");
    read_num(lx, y, sizeof y, "Y", "double");
    read_num(lx, w, sizeof w, "Width", "double");
    read_num(lx, h, sizeof h, "Height", "double");
    read_num(lx, r, sizeof r, "R", "double");
    read_num(lx, g, sizeof g, "G", "double");
    read_num(lx, b, sizeof b, "B", "double");
    skip_semicolon(lx);
    emit("hx_hud2d(%s, %s, %s, %s, %s, %s, %s, %s);\n", idC, x, y, w, h, r, g, b);
}

static void stmt_sky(Lexer *lx) {
    char r[NUM_MAX], g[NUM_MAX], b[NUM_MAX];
    read_num(lx, r, sizeof r, "R", "double");
    read_num(lx, g, sizeof g, "G", "double");
    read_num(lx, b, sizeof b, "B", "double");
    skip_semicolon(lx);
    emit("hx_sky(%s, %s, %s);\n", r, g, b);
}

static void stmt_fog(Lexer *lx) {
    char d[NUM_MAX];
    read_num(lx, d, sizeof d, "the fog distance (0 = off)", "double");
    skip_semicolon(lx);
    emit("hx_fog(%s);\n", d);
}

static void stmt_inject(Lexer *lx, int depth) {
    Token t;
    char injFile[TOK_MAX];
    char full[PATH_MAX_LEN];
    char dirPath[PATH_MAX_LEN];
    char safe[TOK_MAX];
    size_t i;

    lex_next(lx, &t);
    if (t.type != TK_STRING) {
        fail_at(lx->path, t.line, "inject expects a quoted file name, e.g. inject \"Player.hxl\";");
    }
    copy_str(injFile, sizeof injFile, t.text);
    skip_semicolon(lx);

    if (is_absolute(injFile)) {
        copy_str(full, sizeof full, injFile);
    } else {
        dir_of(lx->path, dirPath, sizeof dirPath);
        if (strlen(dirPath) + strlen(injFile) + 1 > sizeof full) {
            fail_at(lx->path, t.line, "inject path is too long");
        }
        copy_str(full, sizeof full, dirPath);
        memcpy(full + strlen(full), injFile, strlen(injFile) + 1);
        if (!file_exists(full)) {
            copy_str(full, sizeof full, injFile); /* fall back to the working directory */
        }
    }
    if (!file_exists(full)) {
        fail_at(lx->path, t.line, "cannot find injected file \"%s\"", injFile);
    }

    copy_str(safe, sizeof safe, injFile);
    for (i = 0; safe[i] != '\0'; i++) {
        if (safe[i] == '*') {
            safe[i] = '_';
        }
    }
    emit("/* inject: %s */\n", safe);
    parse_file(full, depth + 1);
}

/* print/println accept comma-separated string, character, text, or numeric expressions. */
static void stmt_print(Lexer *lx, Token *stmt, int newline) {
    int n = gather(lx, stmt, 0, MAX_STMT_TOKENS);
    int start = 0, i;
    if (n == 0) {
        if (newline) emit("putchar('\\n');\n");
        return;
    }
    for (i = 0; i <= n; i++) {
        if (i == n || is_sym(&stmt[i], ",")) {
            char expr[EXPR_MAX], literal[EXPR_MAX + 8];
            int count = i - start;
            if (count <= 0) fail_at(lx->path, stmt[start < n ? start : 0].line, "empty argument in print");
            if (count == 1 && stmt[start].type == TK_STRING) {
                make_literal(lx, stmt[start].line, literal, sizeof literal, "\"", stmt[start].text);
                emit("printf(\"%%s\", %s);\n", literal);
            } else if (count == 1 && stmt[start].type == TK_CHAR) {
                char ch[ARG_MAX];
                letter_literal(lx, &stmt[start], ch, sizeof ch);
                emit("printf(\"%%c\", %s);\n", ch);
            } else if (count == 1 && stmt[start].type == TK_WORD && sym_lookup(stmt[start].text) == ST_TEXT) {
                emit("printf(\"%%s\", %s);\n", stmt[start].text);
            } else if (count == 1 && stmt[start].type == TK_WORD && sym_lookup(stmt[start].text) == ST_CHAR) {
                emit("printf(\"%%c\", %s);\n", stmt[start].text);
            } else {
                join_tokens(lx, stmt, start, i, expr, sizeof expr);
                emit("printf(\"%%g\", (double)(%s));\n", expr);
            }
            start = i + 1;
        }
    }
    if (newline) emit("putchar('\\n');\n");
}

static void stmt_input(Lexer *lx) {
    Token t, end;
    int type;
    lex_next(lx, &t);
    if (t.type != TK_WORD) fail_at(lx->path, t.line, "expected a variable name after 'input'");
    type = sym_lookup(t.text);
    if (type < 0) fail_at(lx->path, t.line, "unknown variable '%s' in input", t.text);
    lex_next(lx, &end);
    if (!is_sym(&end, ";")) fail_at(lx->path, end.line, "expected ';' after input variable");
    switch (type) {
    case ST_INT: emit("if (!hx_read_int(&%s)) fprintf(stderr, \"Hexal: invalid integer input for %s\\n\");\n", t.text, t.text); break;
    case ST_FLOAT: emit("if (!hx_read_double(&%s)) fprintf(stderr, \"Hexal: invalid number input for %s\\n\");\n", t.text, t.text); break;
    case ST_CHAR: emit("if (!hx_read_char(&%s)) fprintf(stderr, \"Hexal: invalid character input for %s\\n\");\n", t.text, t.text); break;
    case ST_TEXT: emit("if (!hx_read_text(%s, sizeof(%s))) fprintf(stderr, \"Hexal: could not read text input\\n\");\n", t.text, t.text); break;
    default: fail_at(lx->path, t.line, "unsupported input type");
    }
}

static void stmt_mouse_look(Lexer *lx) {
    Token t;
    char sensitivity[NUM_MAX];
    lex_peek(lx, &t);
    if (is_sym(&t, ";") || is_sym(&t, "}")) {
        skip_semicolon(lx);
        emit("hx_mouse_look(0.15);\n");
        return;
    }
    lex_next(lx, &t);
    if (t.type == TK_WORD && strcmp(t.text, "off") == 0) {
        lex_next(lx, &t);
        if (!is_sym(&t, ";")) fail_at(lx->path, t.line, "expected ';' after 'mouse_look off'");
        emit("hx_mouse_look(0.0);\n");
        return;
    }
    if (t.type != TK_WORD && t.type != TK_SYM) fail_at(lx->path, t.line, "expected sensitivity or 'off' after 'mouse_look'");
    sensitivity[0] = '\0';
    str_append(lx, t.line, sensitivity, sizeof sensitivity, "(double)(");
    str_append(lx, t.line, sensitivity, sizeof sensitivity, t.text);
    str_append(lx, t.line, sensitivity, sizeof sensitivity, ")");
    skip_semicolon(lx);
    emit("hx_mouse_look(%s);\n", sensitivity);
}

static void stmt_velocity(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8], vx[NUM_MAX], vy[NUM_MAX]; int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "an object ID after 'velocity");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, vx, sizeof vx, "X velocity", "double");
    read_num(lx, vy, sizeof vy, "Y velocity", "double");
    skip_semicolon(lx); emit("hx_velocity(%s, %s, %s);\n", idC, vx, vy);
}
static void stmt_gravity(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8], g[NUM_MAX]; int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "an object ID after 'gravity'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, g, sizeof g, "gravity acceleration", "double");
    skip_semicolon(lx); emit("hx_gravity(%s, %s);\n", idC, g);
}
static void stmt_friction(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8], factor[NUM_MAX]; int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "an object ID after 'friction'");
    id_expr(lx, id, tid, idC, sizeof idC);
    read_num(lx, factor, sizeof factor, "friction factor", "double");
    skip_semicolon(lx); emit("hx_friction(%s, %s);\n", idC, factor);
}
static void stmt_bounce(Lexer *lx) {
    char a[ARG_MAX], b[ARG_MAX], ac[ARG_MAX + 8], bc[ARG_MAX + 8]; int at = TK_WORD, bt = TK_WORD;
    read_arg(lx, a, sizeof a, &at, "the first object ID after 'bounce'");
    id_expr(lx, a, at, ac, sizeof ac);
    read_arg(lx, b, sizeof b, &bt, "the second object ID after 'bounce'");
    id_expr(lx, b, bt, bc, sizeof bc);
    skip_semicolon(lx); emit("hx_bounce(%s, %s);\n", ac, bc);
}

static void stmt_physics(Lexer *lx) {
    char id[ARG_MAX], idC[ARG_MAX + 8]; int tid = TK_WORD;
    read_arg(lx, id, sizeof id, &tid, "an object ID after 'physics'");
    id_expr(lx, id, tid, idC, sizeof idC);
    skip_semicolon(lx); emit("hx_physics(%s);\n", idC);
}

/* Anything else: assignments, ++, break, ... translated token by token. */
static void stmt_generic(Lexer *lx, Token *stmt) {
    char expr[EXPR_MAX];
    char operand[EXPR_MAX];
    int n = gather(lx, stmt, 1, MAX_STMT_TOKENS);
    int st = (stmt[0].type == TK_WORD) ? sym_lookup(stmt[0].text) : -1;
    int i;

    if (st == ST_TEXT) {
        if (n != 3 || !is_sym(&stmt[1], "=")) {
            fail_at(lx->path, stmt[0].line,
                    "text variable '%s' only supports: %s = \"string\"; or %s = otherText;",
                    stmt[0].text, stmt[0].text, stmt[0].text);
        }
        text_operand(lx, &stmt[2], operand, sizeof operand);
        emit("hx_strset(%s, sizeof(%s), %s);\n", stmt[0].text, stmt[0].text, operand);
        return;
    }
    if (st == ST_CHAR && n == 3 && is_sym(&stmt[1], "=")) {
        letter_literal(lx, &stmt[2], operand, sizeof operand);
        emit("%s = %s;\n", stmt[0].text, operand);
        return;
    }
    if (st >= 0) {
        for (i = 0; i < n; i++) {
            if (stmt[i].type == TK_STRING) {
                fail_at(lx->path, stmt[i].line, "'%s' holds a number; strings only fit in 'text' variables",
                        stmt[0].text);
            }
        }
    }
    join_tokens(lx, stmt, 0, n, expr, sizeof expr);
    emit("%s;\n", expr);
}

static void parse_file(const char *path, int depth) {
    Lexer lx;
    Token tk;
    Token *stmt;
    char word[TOK_MAX];
    int i;

    if (depth > MAX_INCLUDE_DEPTH) {
        fail_at(path, 1, "inject nesting is too deep (limit %d)", MAX_INCLUDE_DEPTH);
    }
    for (i = 0; i < depth; i++) {
        if (strcmp(incStack[i], path) == 0) {
            fail_at(path, 1, "circular inject of \"%s\"", path);
        }
    }
    copy_str(incStack[depth], PATH_MAX_LEN, path);

    lex_open(&lx, path);
    stmt = (Token *)malloc(sizeof(Token) * MAX_STMT_TOKENS);
    if (stmt == NULL) {
        fail_at(path, 1, "out of memory");
    }

    for (;;) {
        lex_next(&lx, &tk);
        if (tk.type == TK_EOF) {
            break;
        }
        if (tk.type == TK_SYM) {
            if (strcmp(tk.text, ";") == 0) {
                continue;
            }
            if (strcmp(tk.text, "}") == 0) {
                close_block(&lx, tk.line);
                continue;
            }
            fail_at(lx.path, tk.line, "unexpected '%s'", tk.text);
        }
        if (tk.type != TK_WORD) {
            fail_at(lx.path, tk.line, "a statement cannot start with a literal");
        }
        copy_str(word, sizeof word, tk.text);

        if (programClosed && blockTop == 0) {
            fail_at(lx.path, tk.line, "unexpected code after the end of 'Program main'");
        }
        if (blockTop == 0 && strcmp(word, "Program") != 0) {
            fail_at(lx.path, tk.line, "code must be inside 'Program main { ... }'");
        }

        if (strcmp(word, "Program") == 0) {
            stmt_program(&lx, tk.line);
        } else if (strcmp(word, "hex") == 0) {
            stmt_decl(&lx, stmt, word, ST_INT);
        } else if (strcmp(word, "hexf") == 0) {
            stmt_decl(&lx, stmt, word, ST_FLOAT);
        } else if (strcmp(word, "text") == 0 || strcmp(word, "hext") == 0) {
            stmt_decl(&lx, stmt, word, ST_TEXT);
        } else if (strcmp(word, "letter") == 0 || strcmp(word, "hexc") == 0) {
            stmt_decl(&lx, stmt, word, ST_CHAR);
        } else if (strcmp(word, "Window") == 0 || strcmp(word, "window") == 0) {
            stmt_window(&lx);
        } else if (strcmp(word, "loop") == 0) {
            stmt_loop(&lx, tk.line);
        } else if (strcmp(word, "while") == 0) { /* <-- ΠΡΟΣΘΗΚΗ: Το νέο σου while loop */
            char cond[EXPR_MAX];
            parse_condition(&lx, cond, sizeof cond, tk.line);
            emit("while (%s) {\n", cond);
            push_block(&lx, tk.line, hasWindow ? BK_LOOP : BK_IF);
            if (hasWindow) {
                emit("hx_pump();\nif (!hx_alive()) break;\n");
            }
        } else if (strcmp(word, "when") == 0 || strcmp(word, "if") == 0 ||
                   strcmp(word, "else") == 0 || strcmp(word, "otherwise") == 0) { /* <-- ΕΝΗΜΕΡΩΣΗ: Προστέθηκε το otherwise */
            stmt_cond(&lx, word, tk.line);
        } else if (strcmp(word, "print") == 0 || strcmp(word, "println") == 0) {
            stmt_print(&lx, stmt, strcmp(word, "println") == 0);
        } else if (strcmp(word, "input") == 0) {
            stmt_input(&lx);
        } else if (strcmp(word, "velocity") == 0) {
            stmt_velocity(&lx);
        } else if (strcmp(word, "gravity") == 0) {
            stmt_gravity(&lx);
        } else if (strcmp(word, "physics") == 0) {
            stmt_physics(&lx);
        } else if (strcmp(word, "friction") == 0) {
            stmt_friction(&lx);
        } else if (strcmp(word, "bounce") == 0) {
            stmt_bounce(&lx);
        } else if (strcmp(word, "move") == 0) {
            stmt_move(&lx);
        } else if (strcmp(word, "draw_2d") == 0) {
            stmt_draw2d(&lx);
        } else if (strcmp(word, "draw_3d") == 0) {
            stmt_draw3d(&lx);
        } else if (strcmp(word, "camera") == 0) {
            stmt_camera(&lx);
        } else if (strcmp(word, "look") == 0) {
            stmt_look(&lx, "hx_look");
        } else if (strcmp(word, "turn") == 0) {
            stmt_look(&lx, "hx_turn");
        } else if (strcmp(word, "walk") == 0) {
            stmt_walk(&lx);
        } else if (strcmp(word, "mouse_look") == 0) {
            stmt_mouse_look(&lx);
        } else if (strcmp(word, "cube") == 0) {
           stmt_cube(&lx);
        } else if (strcmp(word, "layer") == 0) {
            stmt_layer(&lx);
        } else if (strcmp(word, "remove") == 0) {
            stmt_remove(&lx);
        } else if (strcmp(word, "pick") == 0) {
            stmt_pick(&lx);
        } else if (strcmp(word, "mine") == 0) {
            skip_semicolon(&lx);
            emit("hx_mine();\n");
        } else if (strcmp(word, "place") == 0) {
            stmt_place(&lx);
        } else if (strcmp(word, "draw_text") == 0) {
            stmt_draw_text(&lx);
        } else if (strcmp(word, "jump") == 0) { 
            skip_semicolon(&lx);
            emit("hx_jump(0.22);\n");
        } else if (strcmp(word, "hud_2d") == 0) { 
            stmt_hud2d(&lx);                      
        } else if (strcmp(word, "sky") == 0) {
            stmt_sky(&lx);
        } else if (strcmp(word, "fog") == 0) {
            stmt_fog(&lx);
        } else if (strcmp(word, "inject") == 0) {
            stmt_inject(&lx, depth);
        } else {
            stmt[0] = tk;
            stmt_generic(&lx, stmt);
        }
    }

    if (blockTop != lx.baseTop) {
        fail_at(lx.path, lx.line, "missing '}' - a block opened in this file is never closed");
    }
    if (depth == 0 && !programSeen) {
        fail_at(lx.path, 1, "no 'Program main { ... }' block found");
    }
    free(stmt);
    free(lx.buf);
}

/* ------------------------------------------------------------------ */
/* Driver                                                             */
/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
            "Hexal compiler\n"
            "usage: hexal_compiler [input.hxl] [-o output.exe] [--emit-only] [--console]\n"
            "  input.hxl    Hexal source (default: main.hxl)\n"
            "  -o file      output executable (default: main.exe); the C file uses the same name\n"
            "  --emit-only  write the C file but do not run GCC\n"
            "  --console    do not pass -mwindows (keeps a console window)\n");
}

int main(int argc, char **argv) {
    const char *inPath = NULL;
    char exePath[PATH_MAX_LEN] = "main.exe";
    char cPath[PATH_MAX_LEN];
    char compileCmd[4 * PATH_MAX_LEN];
    int emitOnly = 0;
    int consoleMode = 0;
    int i;
    int n;
    size_t len;


    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            copy_str(exePath, sizeof exePath, argv[++i]);
        } else if (strcmp(argv[i], "--emit-only") == 0) {
            emitOnly = 1;
        } else if (strcmp(argv[i], "--console") == 0) {
            consoleMode = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 2;
        } else if (inPath == NULL) {
            inPath = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (inPath == NULL) {
        inPath = "main.hxl";
    }

    copy_str(cPath, sizeof cPath, exePath);
    len = strlen(cPath);
    if (len > 4 && ci_equal(cPath + len - 4, ".exe")) {
        cPath[len - 4] = '\0';
    }
    if (strlen(cPath) + 3 > sizeof cPath) {
        fprintf(stderr, "output name is too long\n");
        return 2;
    }
    memcpy(cPath + strlen(cPath), ".c", 3);
    if (strcmp(cPath, inPath) == 0) {
        fprintf(stderr, "the input file and the generated C file would be the same file\n");
        return 2;
    }

    copy_str(outPath, sizeof outPath, cPath);
    outFile = fopen(cPath, "w");
    if (outFile == NULL) {
        fprintf(stderr, "cannot create '%s'\n", cPath);
        return 1;
    }

    parse_file(inPath, 0);
    if (fclose(outFile) != 0) {
        outFile = NULL;
        fprintf(stderr, "error while writing '%s'\n", cPath);
        return 1;
    }
    outFile = NULL;
    printf("Hexal: translated '%s' -> '%s'\n", inPath, cPath);
    if (emitOnly) {
        return 0;
    }

    n = snprintf(compileCmd, sizeof compileCmd, "gcc -O2 \"%s\" -o \"%s\" -lgdi32 -luser32 -lm%s",
                 cPath, exePath, consoleMode ? "" : " -mwindows");
    if (n < 0 || (size_t)n >= sizeof compileCmd) {
        fprintf(stderr, "compile command is too long\n");
        return 1;
    }
    if (system(compileCmd) != 0) {
        fprintf(stderr, "Hexal: GCC failed while building '%s'\n", cPath);
        return 1;
    }
    printf("Hexal: built '%s'%s\n", exePath,
           hasLoop ? "" : " (no loop in the program: the window stays open until closed)");

    /* 🚀 GLOBAL RUN FIXED: Καθαρή, ασφαλής εκτέλεση του τοπικού αρχείου χωρίς % tokens! */
    printf("Hexal: Launching game...\n");
    char finalRunCmd[PATH_MAX_LEN + 16];
    snprintf(finalRunCmd, sizeof finalRunCmd, ".\\%s", exePath);
    system(finalRunCmd); 

    return 0;
}