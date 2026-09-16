/*
 * TurboPDF.tpd -- TurboPrint-compatible printer driver (PrinterSegment)
 * that outputs PDF via libHaru (hpdf.library) to stdout.
 *
 * Pure C -- no assembly.  The PrinterSegment + PrinterExtendedData
 * structs are defined locally with __attribute__((packed)) to match
 * the exact byte layout that printer.device expects on both m68k and
 * PPC (MorphOS).
 *
 * Three data paths:
 *   - Text mode: printable characters arrive via ped_ConvFunc (V34+);
 *     lines accumulate and are emitted as PDF pages, sized to the
 *     paper selection from Preferences (US Letter, A4/A5..A0, etc.).
 *   - PRD_TPEXTDUMPRPORT (TurboPrint, via DoSpecial)
 *   - PRD_DUMPRPORT (standard, via Render)
 * The two graphics paths convert the source bitmap to RGB24 and embed
 * the image in the PDF entirely through libHaru.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <intuition/preferences.h>

#include <string.h>

#include <hpdf.h>

#include "turboprint.h"

#define STR_(s) #s
#define STR(s)  STR_(s)

#define DRIVER_VERSION   34   /* V34+: device calls ped_ConvFunc for text */
#define DRIVER_REVISION  1

/* ANSI text command codes (canonical devices/printer.h, aRIS..aRAW).
 * The Commands table is indexed by these, so they must match the
 * printer.device constants exactly. */
#define aRIS    0
#define aSGR0   5
#define aSGR3   6
#define aSGR23  7
#define aSGR4   8
#define aSGR24  9
#define aSGR1   10
#define aSGR22  11
#define aRAW    76

/* Paper-size / pitch / spacing codes (intuition/preferences.h).
 * Local fallbacks so the driver still builds on toolchains whose
 * Preferences header predates the European sizes. */
#ifndef US_LETTER
#define US_LETTER   0x00
#define US_LEGAL    0x10
#define N_TRACTOR   0x20
#define W_TRACTOR   0x30
#define CUSTOM      0x40
#define EURO_A0     0x50
#define EURO_A1     0x60
#define EURO_A2     0x70
#define EURO_A3     0x80
#define EURO_A4     0x90
#define EURO_A5     0xA0
#define EURO_A6     0xB0
#define EURO_A7     0xC0
#define EURO_A8     0xD0
#endif
#ifndef PICA
#define PICA        0x000
#define ELITE       0x400
#define FINE        0x800
#endif
#ifndef SIX_LPI
#define SIX_LPI     0x000
#define EIGHT_LPI   0x200
#endif

/* ------------------------------------------------------------------
 *  Packed structs -- match the exact m68k byte layout from
 *  devices/prtbase.h.  We define local copies with __attribute__
 *  ((packed)) so the layout is correct on PPC (MorphOS) too.
 * ------------------------------------------------------------------ */

/* Printer class / color-class constants */
#define PPCF_GFX  1
#define PPCF_COLOR  2
#define PCC_BW    1

struct PrinterExtendedData {
    STRPTR   ped_PrinterName;
    APTR     ped_Init;
    APTR     ped_Expunge;
    APTR     ped_Open;
    APTR     ped_Close;
    UBYTE    ped_PrinterClass;
    UBYTE    ped_ColorClass;
    UBYTE    ped_MaxColumns;
    UBYTE    ped_NumCharSets;
    UWORD    ped_NumRows;
    ULONG    ped_MaxXDots;
    ULONG    ped_MaxYDots;
    UWORD    ped_XDotsInch;
    UWORD    ped_YDotsInch;
    APTR     ped_Commands;
    APTR     ped_DoSpecial;
    APTR     ped_Render;
    LONG     ped_TimeoutSecs;
    APTR     ped_8BitChars;
    LONG     ped_PrintMode;
    APTR     ped_ConvFunc;
} __attribute__((packed));

struct PrinterSegment {
    ULONG    ps_NextSegment;       /* BPTR, 0 = last segment  */
    ULONG    ps_runAlert;          /* moveq #0,d0; rts        */
    UWORD    ps_Version;
    UWORD    ps_Revision;
    struct PrinterExtendedData ps_PED;
} __attribute__((packed));

/* ------------------------------------------------------------------
 *  Forward declarations
 * ------------------------------------------------------------------ */

static int   __attribute__((used)) ped_init(struct PrinterData *pd);
static void  __attribute__((used)) ped_expunge(void);
static int   __attribute__((used)) ped_open(struct IORequest *ior);
static void  __attribute__((used)) ped_close(struct IORequest *ior);
static LONG  __attribute__((used)) ped_dospecial(UWORD *command,
                                                 UBYTE *out,
                                                 BYTE *pl_curline,
                                                 BYTE *pl_spacing,
                                                 BYTE *pl_crlf,
                                                 UBYTE *params);
static int   __attribute__((used)) ped_render(struct RastPort *rp,
                                              ULONG c, ULONG x,
                                              ULONG y, ULONG status);
static LONG  ped_convfunc(UBYTE *buf, UBYTE c, LONG crlf_flag);
static void  style_set(UBYTE code);

/* ------------------------------------------------------------------
 *  PrinterSegment header -- MUST be the very first thing in the code
 *  hunk.  __attribute__((section(".text"))) places it in the code
 *  section; combined with -nostartfiles it becomes the first word
 *  of the LoadSeg'd module.
 * ------------------------------------------------------------------ */

static const STRPTR sg_name  = "TurboPDF";

/* 8-bit character set (ped_8BitChars / ped_NumCharSets): high bytes
 * 0x80-0x9F (C1 controls) are not printable -- map to a space; the
 * Latin-1 range 0xA0-0xFF keeps its glyph.  printer.device uses this
 * table for codes >= 128; ped_convfunc applies the same mapping. */
static const UBYTE sg_8bit[128] = {
    ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
    ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF,
};

/* Printer escape-command table, indexed by ANSI command code aRIS(0) ..
 * aRAW(76).  The 0xFF entries route style changes (SGR) to ped_DoSpecial;
 * everything else stays NULL (ignored -- newline/formfeed handling lives
 * in ped_ConvFunc).  The table must hold aRAW+1 entries because
 * printer.device indexes it with the parsed command number.
 */
#define TEXT_NUM_CMDS  77
static const STRPTR sg_cmds[TEXT_NUM_CMDS] = {
    [aSGR0]  = "\377",   /* all attributes off       */
    [aSGR1]  = "\377",   /* bold                      */
    [aSGR22] = "\377",   /* normal intensity          */
    [aSGR3]  = "\377",   /* italic                    */
    [aSGR23] = "\377",   /* italic off                */
    [aSGR4]  = "\377",   /* underline                 */
    [aSGR24] = "\377",   /* underline off             */
};

const struct PrinterSegment sg __attribute__((used, section(".text"))) = {
    .ps_NextSegment = 0,
    .ps_runAlert    = 0x70004E75UL,   /* moveq #0,d0 ; rts */
    .ps_Version     = DRIVER_VERSION,
    .ps_Revision    = DRIVER_REVISION,
    .ps_PED = {
        .ped_PrinterName  = sg_name,
        .ped_Init         = ped_init,
        .ped_Expunge      = ped_expunge,
        .ped_Open         = ped_open,
        .ped_Close        = ped_close,
        .ped_PrinterClass = PPCF_GFX | PPCF_COLOR,
        .ped_ColorClass   = PCC_BW,
        .ped_MaxColumns   = 80,
        .ped_NumCharSets  = 1,
        .ped_NumRows      = 1,
        .ped_MaxXDots     = 0,
        .ped_MaxYDots     = 0,
        .ped_XDotsInch    = 300,
        .ped_YDotsInch    = 300,
        .ped_Commands     = (APTR)sg_cmds,
        .ped_DoSpecial    = ped_dospecial,
        .ped_Render       = ped_render,
        .ped_TimeoutSecs  = 120L,
        .ped_8BitChars    = (APTR)sg_8bit,
        .ped_PrintMode    = 0,
        .ped_ConvFunc     = ped_convfunc,
    }
};

/* ------------------------------------------------------------------
 *  Per-job state
 * ------------------------------------------------------------------ */

static HPDF_Doc            g_doc;        /* current PDF document   */
static struct IORequest   *g_req;        /* current IORequest      */
static int                 g_npages;     /* pages in current doc   */

/* Per-page band accumulation (Render path) */
static UBYTE              *g_rowbuf;     /* accumulated RGB24 rows */
static ULONG               g_rowbufsz;   /* allocated bytes        */
static ULONG               g_rowstride;  /* bytes per row (w * 3)  */
static ULONG               g_nrows;      /* rows accumulated so far*/

/* Text-mode accumulation (ped_ConvFunc path) */
#define TP_MAXLINES  256                 /* max lines per text page  */
#define TP_MAXCOL    256                 /* chars per line           */
#define TP_PAGE_W    612.0f              /* default (US Letter, pt)  */
#define TP_PAGE_H    792.0f
#define TP_MARGIN     36.0f
#ifndef TP_FONTSIZE
#define TP_FONTSIZE   10.0f
#endif
#define TP_LINESTEP   12.0f              /* 6 lines per inch         */

#define TP_ST_NORMAL     0x00
#define TP_ST_BOLD       0x01
#define TP_ST_ITALIC     0x02
#define TP_ST_UNDERLINE  0x04

#define TP_COL_UNSET     0xFF            /* color index: default   */

struct TCell {
    char  ch;                            /* character code         */
    UBYTE st;                            /* TP_ST_* attributes     */
    UBYTE lk;                            /* link id (0 = none)     */
    UBYTE fg;                            /* color idx, UNSET=default */
    UBYTE bg;                            /* color idx, UNSET=default */
};

static struct TCell  g_txt[TP_MAXLINES][TP_MAXCOL];
static int           g_txtlen[TP_MAXLINES]; /* chars per stored line */
static int           g_txtsln;              /* current line index    */
static int           g_txtcol;              /* current column count  */
static int           g_txtactive;          /* any text this page     */
static int           g_txtcr;               /* CR pending (CRLF pair) */
static UBYTE         g_txtstyle;            /* current TP_ST_* attrs  */
static UBYTE         g_fg, g_bg;            /* current fg/bg color idx */

/* Per-page color registry: cells hold a UBYTE index (unset 0xFF).
 * Reset in text_reset() along with the text buffer. */
#define TP_MAXCOLORS  256
static UBYTE g_colors[TP_MAXCOLORS][3];
static int   g_ncolors;

/* Optional 8-bit charset table (ped_8BitChars), applied to codes
 * >= 0x80 in text_char(). */
static const UBYTE *g_charmap;
static HPDF_REAL    g_fontsize;          /* point size for job      */

/* Hyperlinks: OSC-8 regions (explicit) and auto-detected URLs are
 * emitted as PDF URI link annotations. */
#define TP_MAXLINKS  64                  /* links per text page     */
#define TP_LINKLEN   256                 /* max OSC-8 URI length    */

#define OSC_IDLE     0                   /* osc-8 parser states     */
#define OSC_ESC      1
#define OSC_CMD      2                   /* expecting "8"            */
#define OSC_P1       3                   /* between 1st/2nd ';'      */
#define OSC_URI      4                   /* collecting URI           */
#define OSC_ESCEND   5                   /* seen ESC while in URI    */
#define OSC_CSI      6                   /* ANSI CSI parameter       */

#define TP_CSIMAX    32                  /* max CSI parameter string */

static char  *g_links[TP_MAXLINKS];      /* pool of link URIs        */
static int    g_nlinks;                  /* entries in g_links       */
static int    g_curlink;                 /* active link id (1..n)    */
static int    g_osc;                     /* osc-8 parser state       */
static char   g_oscuri[TP_LINKLEN];      /* osc-8 URI being read    */
static int    g_osclen;
static char   g_csibuf[TP_CSIMAX];       /* CSI parameter string     */
static int    g_csil;

/* Text-page geometry derived from Preferences (see pdf_page_setup) */
static HPDF_REAL     g_page_w;             /* page size, points     */
static HPDF_REAL     g_page_h;
static HPDF_REAL     g_left;               /* text area margins     */
static HPDF_REAL     g_right;
static HPDF_REAL     g_line_step;          /* line pitch, points    */
static int           g_max_lines;          /* lines before eject */

/* ------------------------------------------------------------------
 *  PDF helpers
 * ------------------------------------------------------------------ */

/* Write the accumulated PDF to stdout via dos.library Write(). */
static int pdf_output(void)
{
    HPDF_STATUS st;
    HPDF_UINT32 sz, got;
    HPDF_BYTE  *buf;
    BPTR        out;

    st = HPDF_SaveToStream(g_doc);
    if (st != HPDF_OK) return -1;

    sz = HPDF_GetStreamSize(g_doc);
    if (sz == 0) return 0;                  /* empty -- nothing to do */

    buf = AllocVec(sz, MEMF_ANY);
    if (!buf) return -1;

    got = sz;
    st  = HPDF_GetContents(g_doc, buf, &got);
    if (st == HPDF_OK) {
        out = Output();
        if (out) Write(out, buf, (LONG)got);
    }
    FreeVec(buf);
    return (st == HPDF_OK) ? 0 : -1;
}

/* Add a raw RGB24 buffer to the current PDF as a new page.
 *  w, h  - image dimensions in pixels
 *  rgb   - RGB24 pixel data (top-down, w*h*3 bytes) */
static int pdf_add_rgb(ULONG w, ULONG h, const HPDF_BYTE *rgb)
{
    HPDF_Page  pg;
    HPDF_Image im;
    HPDF_REAL  pw, ph;

    if (!g_doc) return -1;

    im = HPDF_LoadRawImageFromMem(g_doc, rgb,
                                  (HPDF_UINT)w, (HPDF_UINT)h,
                                  HPDF_CS_DEVICE_RGB, 8);
    if (!im) return -1;

    pg = HPDF_AddPage(g_doc);
    if (!pg) return -1;

    pw = (HPDF_REAL)w;
    ph = (HPDF_REAL)h;
    HPDF_Page_SetWidth(pg, pw);
    HPDF_Page_SetHeight(pg, ph);
    HPDF_Page_DrawImage(pg, im, 0, 0, pw, ph);

    g_npages++;
    return 0;
}

/* ------------------------------------------------------------------
 *  Text helpers -- accumulate style-tagged character cells into a
 *  page-sized line array, emit text pages via libHaru.
 * ------------------------------------------------------------------ */

static int pdf_add_text(void);

/* Map the current printer Preferences to the PDF text-page geometry:
 * paper size, printable-area margins (chars * pitch width) and line
 * pitch.  Everything in points; falls back to US Letter when the
 * selected size is unknown. */
static void pdf_page_setup(struct PrinterData *pd)
{
    static const struct {
        UWORD    id;
        HPDF_REAL w, h;
    } sg_papers[] = {
        { US_LETTER,  612.0f,   792.0f  },
        { US_LEGAL,   612.0f,   1008.0f },
        { N_TRACTOR,  684.0f,   792.0f  },
        { W_TRACTOR,  1070.0f,  792.0f  },
        { EURO_A0,    2384.0f,  3370.0f },
        { EURO_A1,    1684.0f,  2384.0f },
        { EURO_A2,    1191.0f,  1684.0f },
        { EURO_A3,    842.0f,   1191.0f },
        { EURO_A4,    595.0f,   842.0f  },
        { EURO_A5,    420.0f,   595.0f  },
        { EURO_A6,    298.0f,   420.0f  },
        { EURO_A7,    210.0f,   298.0f  },
        { EURO_A8,    148.0f,   210.0f  },
        { 0,          0.0f,     0.0f    }
    };
    struct Preferences *pr;
    HPDF_REAL          cw;
    int                i;

    g_page_w    = TP_PAGE_W;
    g_page_h    = TP_PAGE_H;
    g_left      = TP_MARGIN;
    g_right     = TP_PAGE_W - TP_MARGIN;
    g_line_step = TP_LINESTEP;
    g_max_lines = TP_MAXLINES;
    g_fontsize  = TP_FONTSIZE;

    pr = &pd->pd_Preferences;

    for (i = 0; i < (int)(sizeof sg_papers / sizeof sg_papers[0]) - 1; i++) {
        if (sg_papers[i].id == pr->PaperSize) {
            g_page_w = sg_papers[i].w;
            g_page_h = sg_papers[i].h;
            break;
        }
    }

    /* Char width at pitch, and a matching point size so a finer pitch
     * prints smaller glyphs (PICA keeps the TP_FONTSIZE default). */
    switch (pr->PrintPitch & 0xC00) {
    case ELITE: cw = 72.0f / 12.0f; g_fontsize =  8.5f; break;
    case FINE:  cw = 72.0f / 17.0f; g_fontsize =  7.0f; break;
    case PICA:
    default:    cw = 72.0f / 10.0f; g_fontsize = TP_FONTSIZE; break;
    }

    g_left  = (HPDF_REAL)pr->PrintLeftMargin  * cw;
    g_right = (HPDF_REAL)pr->PrintRightMargin * cw;

    if (g_left  < 0)            g_left  = 0;             /* clamp */
    if (g_left  > g_page_w / 2) g_left  = g_page_w / 2;
    if (g_right < g_page_w / 2) g_right = g_page_w / 2;
    if (g_right > g_page_w)     g_right = g_page_w;
    if (g_right <= g_left)      g_right = g_left + g_page_w / 2;

    if (pr->PrintSpacing == EIGHT_LPI)
        g_line_step = 9.0f;               /* 8 lines per inch */

    g_max_lines = (int)((g_page_h - 2 * TP_MARGIN) / g_line_step);
    if (g_max_lines < 1)                    g_max_lines = 1;
    if (g_max_lines > TP_MAXLINES)          g_max_lines = TP_MAXLINES;
}

static void text_reset(void)
{
    g_txtsln    = 0;
    g_txtcol    = 0;
    g_txtactive = 0;
    g_txtcr     = 0;
    g_txtstyle  = TP_ST_NORMAL;
    g_fg        = TP_COL_UNSET;
    g_bg        = TP_COL_UNSET;
    g_ncolors   = 0;
    g_osc       = OSC_IDLE;
    g_osclen    = 0;
    g_csil      = 0;
}

/* Pick the base-14 Helvetica variant for the given attributes. */
static HPDF_Font text_font(HPDF_Doc doc, UBYTE st)
{
    const char *name = "Helvetica";

    if (st & TP_ST_BOLD) {
        name = (st & TP_ST_ITALIC) ? "Helvetica-BoldOblique"
                                   : "Helvetica-Bold";
    } else if (st & TP_ST_ITALIC) {
        name = "Helvetica-Oblique";
    }
    return HPDF_GetFont(doc, name, NULL);
}

/* Lower-case ASCII (Latin-1 safe: only A-Z mapped). */
static UBYTE text_low(UBYTE c)
{
    if (c >= 'A' && c <= 'Z')
        return (UBYTE)(c + 32);
    return c;
}

/* Start of an OSC-8 link: [esc]8;p1;p2<st> opens a link, an empty
 * [esc]8;;<st> closes the current one. */
static void link_finish(void)
{
    char *blk;

    if (g_osclen == 0) {                  /* empty URI -> close */
        g_curlink = 0;
        return;
    }
    g_oscuri[g_osclen] = '\0';
    if (g_nlinks >= TP_MAXLINKS) {
        g_curlink = 0;
        return;
    }

    blk = AllocVec(g_osclen + 1, MEMF_ANY);
    if (!blk) {
        g_curlink = 0;
        return;
    }
    memcpy(blk, g_oscuri, g_osclen + 1);
    g_links[g_nlinks] = blk;
    g_curlink = g_nlinks + 1;
    g_nlinks++;
}

static void link_pool_free(void)
{
    int k;

    for (k = 0; k < g_nlinks; k++)
        if (g_links[k]) FreeVec(g_links[k]);
    g_nlinks = 0;
}

/* Auto-detect a URL at line offset s.  Returns the token length in
 * cells (0 = no URL here).  Candidates are http/https/ftp:// and www.
 * starting at a word boundary; the token ends at whitespace, an
 * explicit link, or a bracket/quote character, with trailing sentence
 * punctuation trimmed. */
static int text_url_end(int line, int len, int s)
{
    static const char *pfx[] = { "http://", "https://", "ftp://", "www." };
    UBYTE              ch;
    int                pi, k, e;

    if (s > 0 && s < len) {               /* must follow a boundary   */
        ch = (UBYTE)g_txt[line][s - 1].ch;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '#' || ch == '_' || ch == '-')
            return 0;
    }

    for (pi = 0; pi < 4; pi++) {
        k = 0;
        while (pfx[pi][k] && s + k < len &&
               text_low((UBYTE)g_txt[line][s + k].ch) == (UBYTE)pfx[pi][k])
            k++;
        if (pfx[pi][k] == '\0')
            break;
    }
    if (pi == 4)
        return 0;

    e = s + (int)strlen(pfx[pi]);
    while (e < len) {
        ch = (UBYTE)g_txt[line][e].ch;
        if (g_txt[line][e].lk)            /* explicit link interrupts */
            break;
        if (ch <= ' ')                    /* whitespace ends token    */
            break;
        if (ch == '"' || ch == '\'' || ch == '<' || ch == '>' ||
            ch == '{' || ch == '}' || ch == '[' || ch == ']' ||
            ch == '(' || ch == ')' || ch == '|' || ch == '`')
            break;
        e++;
    }
    while (e - 1 > s &&                      /* trim trailing punct   */
           (g_txt[line][e - 1].ch == '.' || g_txt[line][e - 1].ch == ',' ||
            g_txt[line][e - 1].ch == ';' || g_txt[line][e - 1].ch == ':' ||
            g_txt[line][e - 1].ch == '!' || g_txt[line][e - 1].ch == '?' ||
            g_txt[line][e - 1].ch == '\'' || g_txt[line][e - 1].ch == '"'))
        e--;

    return (e - s > 5) ? e - s : 0;
}

/* Emit the accumulated lines as PDF text pages, splitting each line
 * into runs that share style + explicit link id.  Auto-detected URLs
 * (cells with no explicit link) and OSC-8 regions become URI link
 * annotations. */
static int pdf_add_text(void)
{
    HPDF_Page      pg;
    HPDF_REAL      y, x, w, asc, desc;
    HPDF_REAL      cumx[TP_MAXCOL];
    HPDF_Font      fnt;
    HPDF_BYTE      cbuf;
    char           runbuf[TP_MAXCOL + 1];
    char           uri[TP_MAXCOL + 1];
    int            i, j, n, len, s, k, is_url;

    if (!g_doc) {
        link_pool_free();
        return 0;
    }
    if (!g_txtactive) {
        if (g_curlink) {                  /* dangling open link       */
            link_pool_free();
            g_curlink = 0;
        }
        return 0;
    }

    pg = HPDF_AddPage(g_doc);
    if (!pg) {
        link_pool_free();
        return -1;
    }
    HPDF_Page_SetWidth(pg, g_page_w);
    HPDF_Page_SetHeight(pg, g_page_h);

    y = g_page_h - TP_MARGIN;
    for (i = 0; i < g_txtsln && y > TP_MARGIN; i++, y -= g_line_step) {
        len = g_txtlen[i];
        if (len <= 0) continue;

        /* Measure per-cell advances so link rects match the glyphs. */
        x = g_left;
        for (j = 0; j < len; j++) {
            cumx[j] = x;
            cbuf    = (HPDF_BYTE)(UBYTE)g_txt[i][j].ch;
            fnt     = text_font(g_doc, g_txt[i][j].st);
            if (!fnt) continue;
            x += (HPDF_REAL)HPDF_Font_TextWidth(fnt, &cbuf, 1).width *
                 g_fontsize / 1000.0f;
        }
        cumx[len] = x;                    /* right edge incl. last cell */

        /* Draw runs (split on style, link, or color). */
        j = 0;
        while (j < len && cumx[j] < g_right) {
            UBYTE st = g_txt[i][j].st;
            UBYTE lk = g_txt[i][j].lk;
            UBYTE fg = g_txt[i][j].fg;
            UBYTE bg = g_txt[i][j].bg;

            n = 0;
            while (j + n < len &&
                   g_txt[i][j + n].st == st &&
                   g_txt[i][j + n].lk == lk &&
                   g_txt[i][j + n].fg == fg &&
                   g_txt[i][j + n].bg == bg) {
                runbuf[n] = g_txt[i][j + n].ch;
                n++;
            }
            runbuf[n] = '\0';

            fnt = text_font(g_doc, st);
            if (fnt) {
                /* Background: a filled rectangle under the run. */
                if (bg != TP_COL_UNSET) {
                    HPDF_Page_SetRGBFill(pg,
                        (HPDF_REAL)g_colors[bg][0] / 255.0f,
                        (HPDF_REAL)g_colors[bg][1] / 255.0f,
                        (HPDF_REAL)g_colors[bg][2] / 255.0f);
                    asc  = (HPDF_REAL)HPDF_Font_GetAscent(fnt)  *
                           g_fontsize / 1000.0f;
                    desc = (HPDF_REAL)-HPDF_Font_GetDescent(fnt) *
                           g_fontsize / 1000.0f;
                    if (cumx[j + n] > g_right) cumx[j + n] = g_right;
                    HPDF_Page_Rectangle(pg, cumx[j], y - desc,
                                        cumx[j + n] - cumx[j], asc + desc);
                    HPDF_Page_Fill(pg);
                }

                HPDF_Page_BeginText(pg);
                HPDF_Page_SetFontAndSize(pg, fnt, g_fontsize);
                if (fg != TP_COL_UNSET) {
                    HPDF_Page_SetRGBFill(pg,
                        (HPDF_REAL)g_colors[fg][0] / 255.0f,
                        (HPDF_REAL)g_colors[fg][1] / 255.0f,
                        (HPDF_REAL)g_colors[fg][2] / 255.0f);
                } else {
                    HPDF_Page_SetRGBFill(pg, 0, 0, 0);
                }
                HPDF_Page_MoveTextPos(pg, cumx[j], y);
                HPDF_Page_ShowText(pg, runbuf);
                HPDF_Page_EndText(pg);

                if (st & TP_ST_UNDERLINE) {
                    HPDF_Page_SetRGBStroke(pg,
                        (HPDF_REAL)(fg != TP_COL_UNSET ? g_colors[fg][0]
                                                       : 0) / 255.0f,
                        (HPDF_REAL)(fg != TP_COL_UNSET ? g_colors[fg][1]
                                                       : 0) / 255.0f,
                        (HPDF_REAL)(fg != TP_COL_UNSET ? g_colors[fg][2]
                                                       : 0) / 255.0f);
                    HPDF_Page_SetLineWidth(pg, 0.5f);
                    HPDF_Page_MoveTo(pg, cumx[j], y - g_fontsize * 0.15f);
                    HPDF_Page_LineTo(pg, cumx[j + n], y - g_fontsize * 0.15f);
                    HPDF_Page_Stroke(pg);
                }
            }

            /* Explicit OSC-8 region: one URI annotation per run. */
            if (lk && lk <= g_nlinks && fnt) {
                HPDF_Rect r;

                asc  = (HPDF_REAL)HPDF_Font_GetAscent(fnt)  *
                       g_fontsize / 1000.0f;
                desc = (HPDF_REAL)-HPDF_Font_GetDescent(fnt) *
                       g_fontsize / 1000.0f;
                if (cumx[j + n] > g_right) cumx[j + n] = g_right;
                r.left   = cumx[j];
                r.bottom = y - desc;
                r.right  = cumx[j + n];
                r.top    = y + asc;
                HPDF_Page_CreateURILinkAnnot(pg, r, g_links[lk - 1]);
            }

            j += n;
        }

        /* Auto-detect URLs on unlinked cells. */
        s = 0;
        while (s < len && cumx[s] < g_right) {
            if (g_txt[i][s].lk) {
                s++;
                continue;
            }
            is_url = 0;
            if ((k = text_url_end(i, len, s)) > 5) {
                for (int q = 0; q < k; q++)
                    uri[q] = g_txt[i][s + q].ch;
                uri[k] = '\0';
                is_url = 1;
            }
            if (is_url) {
                fnt = text_font(g_doc, g_txt[i][s].st);
                if (fnt) {
                    HPDF_Rect r;

                    asc  = (HPDF_REAL)HPDF_Font_GetAscent(fnt)  *
                           g_fontsize / 1000.0f;
                    desc = (HPDF_REAL)-HPDF_Font_GetDescent(fnt) *
                           g_fontsize / 1000.0f;
                    x = cumx[s];
                    w = cumx[s + k];
                    if (w > g_right) w = g_right;
                    r.left   = x;
                    r.bottom = y - desc;
                    r.right  = w;
                    r.top    = y + asc;
                    HPDF_Page_CreateURILinkAnnot(pg, r, uri);
                }
                s += k;
            } else {
                s++;
            }
        }
    }

    g_npages++;

    /* Page-scoped link pool: an OSC-8 link still open carries over to
     * the next text page so regions spanning a formfeed survive. */
    if (g_curlink && g_links[g_curlink - 1]) {
        char *keep = g_links[g_curlink - 1];
        link_pool_free();
        g_links[0] = keep;
        g_nlinks   = 1;
        g_curlink  = 1;
    } else {
        link_pool_free();
        g_curlink = 0;
    }
    return 0;
}

static void text_newline(void)
{
    if (g_txtsln >= g_max_lines) {        /* page capacity -> eject  */
        pdf_add_text();
        text_reset();
    }
    g_txtlen[g_txtsln] = g_txtcol;
    g_txtsln++;
    g_txtcol = 0;
    g_txtactive = 1;
}

static int text_char(UBYTE c)
{
    switch (c) {

    case '\014':                           /* formfeed: eject page   */
        pdf_add_text();
        text_reset();
        return 1;

    case '\r':                             /* CR                     */
        g_txtcr = 1;
        text_newline();
        return 1;

    case '\n':                             /* LF (ignore after CR)   */
        if (g_txtcr) {
            g_txtcr = 0;
            return 1;
        }
        text_newline();
        return 1;

    case '\t':                             /* expand to next tab stop */
        g_txtactive = 1;
        do {
            if (g_txtcol >= TP_MAXCOL - 1) break;
            g_txt[g_txtsln][g_txtcol].ch = ' ';
            g_txt[g_txtsln][g_txtcol].st = g_txtstyle;
            g_txt[g_txtsln][g_txtcol].lk = g_curlink;
            g_txt[g_txtsln][g_txtcol].fg = g_fg;
            g_txt[g_txtsln][g_txtcol].bg = g_bg;
            g_txtcol++;
        } while (g_txtcol % 8);
        return 1;

    default:
        if (c < 0x20)                      /* swallow other controls */
            return 1;
        if (g_charmap && c >= 0x80)        /* 8-bit charset table */
            c = g_charmap[c - 0x80];
        if (g_txtcol < TP_MAXCOL - 1) {
            g_txt[g_txtsln][g_txtcol].ch = (char)c;
            g_txt[g_txtsln][g_txtcol].st = g_txtstyle;
            g_txt[g_txtsln][g_txtcol].lk = g_curlink;
            g_txt[g_txtsln][g_txtcol].fg = g_fg;
            g_txt[g_txtsln][g_txtcol].bg = g_bg;
            g_txtcol++;
            g_txtactive = 1;
        }
        return 1;
    }
}

/* Flush any pending text page (e.g. on device close). */
static void text_flush(void)
{
    pdf_add_text();
    text_reset();
}

/* ANSI SGR palette (index 0..15: 30-37 / 90-97 base colors). */
static const UBYTE sg_ansi[16][3] = {
    {   0,   0,   0 },                /*  0 black      */
    { 205,   0,   0 },                /*  1 red        */
    {   0, 205,   0 },                /*  2 green      */
    { 205, 205,   0 },                /*  3 yellow     */
    {   0,   0, 238 },                /*  4 blue       */
    { 205,   0, 205 },                /*  5 magenta    */
    {   0, 205, 205 },                /*  6 cyan       */
    { 229, 229, 229 },                /*  7 white      */
    { 127, 127, 127 },                /*  8 brightblack*/
    { 255,   0,   0 },                /*  9 bright red */
    {   0, 255,   0 },                /* 10 bright grn */
    { 255, 255,   0 },                /* 11 bright ylw */
    {  92,  92, 255 },                /* 12 bright blue*/
    { 255,   0, 255 },                /* 13 bright mag */
    {   0, 255, 255 },                /* 14 bright cyn */
    { 255, 255, 255 },                /* 15 bright whit*/
};

/* Map an xterm 256-color index to RGB (16 ANSI + 6x6x6 cube + 24 gray). */
static void xterm_rgb(int n, UBYTE *r, UBYTE *g, UBYTE *b)
{
    int v;

    if (n < 0) n = 0;
    if (n > 255) n = 255;
    if (n < 16) {
        *r = sg_ansi[n][0];
        *g = sg_ansi[n][1];
        *b = sg_ansi[n][2];
    } else if (n < 232) {
        v   = n - 16;
        *r  = (UBYTE)((v / 36)      ? 55 + (v / 36)      * 40 : 0);
        *g  = (UBYTE)(((v / 6) % 6) ? 55 + ((v / 6) % 6) * 40 : 0);
        *b  = (UBYTE)((v % 6)       ? 55 + (v % 6)       * 40 : 0);
    } else {
        v   = 8 + (n - 232) * 10;
        *r  = *g = *b = (UBYTE)v;
    }
}

/* Register an RGB colour and return its per-page index (UBYTE).
 * Best-effort: returns TP_COL_UNSET when the pool is exhausted. */
static UBYTE color_reg(UBYTE r, UBYTE g, UBYTE b)
{
    int i;

    for (i = 0; i < g_ncolors; i++)
        if (g_colors[i][0] == r && g_colors[i][1] == g &&
            g_colors[i][2] == b)
            return (UBYTE)i;
    if (g_ncolors >= TP_MAXCOLORS)
        return TP_COL_UNSET;
    g_colors[g_ncolors][0] = r;
    g_colors[g_ncolors][1] = g;
    g_colors[g_ncolors][2] = b;
    return (UBYTE)(g_ncolors++);
}

static void color_fg_rgb(UBYTE r, UBYTE g, UBYTE b)
{ g_fg = color_reg(r, g, b); }

static void color_bg_rgb(UBYTE r, UBYTE g, UBYTE b)
{ g_bg = color_reg(r, g, b); }

static void color_fg_n(unsigned n)
{
    UBYTE r, g, b;

    xterm_rgb((int)n, &r, &g, &b);
    g_fg = color_reg(r, g, b);
}

static void color_bg_n(unsigned n)
{
    UBYTE r, g, b;

    xterm_rgb((int)n, &r, &g, &b);
    g_bg = color_reg(r, g, b);
}

/* Apply one SGR/ANSI style command code (aSGR0..aSGR24 / aRIS).
 * Shared by ped_DoSpecial (7-bit commands from printer.device) and the
 * 8-bit CSI parser in ped_convfunc. */
static void style_set(UBYTE code)
{
    switch (code) {
    case aRIS:
    case aSGR0:                          /* attributes off */
        g_txtstyle = TP_ST_NORMAL;
        g_fg       = TP_COL_UNSET;
        g_bg       = TP_COL_UNSET;
        break;
    case aSGR1:                          /* bold */
        g_txtstyle |= TP_ST_BOLD;
        break;
    case aSGR22:                         /* normal intensity */
        g_txtstyle &= ~TP_ST_BOLD;
        break;
    case aSGR3:                          /* italic */
        g_txtstyle |= TP_ST_ITALIC;
        break;
    case aSGR23:                         /* italic off */
        g_txtstyle &= ~TP_ST_ITALIC;
        break;
    case aSGR4:                          /* underline */
        g_txtstyle |= TP_ST_UNDERLINE;
        break;
    case aSGR24:                         /* underline off */
        g_txtstyle &= ~TP_ST_UNDERLINE;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------
 *  Driver entry points
 * ------------------------------------------------------------------ */

static int ped_convfunc(UBYTE *buf, UBYTE c, LONG crlf_flag)
{
    (void)buf;
    (void)crlf_flag;

    switch (g_osc) {

    case OSC_IDLE:
        if (c == 0x1B) {                  /* ESC -> 7-bit CSI/OSC  */
            g_osc = OSC_ESC;
            g_osclen = 0;
            return 0;
        }
        if (c == 0x9B) {                  /* 8-bit CSI (ECMA-48)    */
            g_osc = OSC_CSI;
            g_csil = 0;
            return 0;
        }
        if (c == 0x9D) {                  /* 8-bit OSC              */
            g_osc = OSC_CMD;
            g_osclen = 0;
            return 0;
        }
        if (c == 0x9C)                    /* 8-bit ST: ignore       */
            return 0;
        g_osclen = 0;
        text_char(c);
        return 0;                         /* handled: nothing to emit */

    case OSC_ESC:                         /* expecting '[' or ']'    */
        if (c == '[') {
            g_osc = OSC_CSI;
            g_csil = 0;
        } else if (c == ']') {
            g_osc = OSC_CMD;
        } else {
            g_osc = OSC_IDLE;             /* unknown escape: drop   */
        }
        return 0;

    case OSC_CMD:                         /* expecting "8"            */
        if (c == ';')
            g_osc = OSC_P1;
        else if (c < '0' || c > '9')
            g_osc = OSC_IDLE;         /* not OSC-8: drop          */
        /* digits keep us in OSC_CMD */
        return 0;

    case OSC_P1:                          /* skip id param until ';'  */
        if (c == ';') {
            g_osc = OSC_URI;
            g_osclen = 0;
        } else if (c == 0x07 || c == 0x9C) {
            g_osc = OSC_IDLE;             /* empty OSC: ignore        */
        } else if (c == 0x1B) {
            g_osc = OSC_ESCEND;
        }
        return 0;

    case OSC_URI:                         /* collecting URI           */
        if (c == 0x07 || c == 0x9C) {     /* BEL / 8-bit ST          */
            link_finish();
            g_osc = OSC_IDLE;
        } else if (c == 0x1B) {           /* ESC... ST terminates     */
            g_osc = OSC_ESCEND;
        } else if (g_osclen < TP_LINKLEN - 1) {
            g_oscuri[g_osclen++] = (char)c;
        }
        return 0;

    case OSC_ESCEND:                      /* expect '\' (ST)          */
        if (c == '\\')
            link_finish();
        g_osc = OSC_IDLE;
        return 0;

    case OSC_CSI:                         /* ANSI CSI parameters      */
        if ((c >= '0' && c <= '9') || c == ';' || c == ':') {
            if (g_csil < TP_CSIMAX - 1)
                g_csibuf[g_csil++] = (char)c;
            return 0;
        }
        if (c == 'm') {                   /* SGR: styles + colors  */
            int   vals[TP_CSIMAX / 2 + 1], nv = 0, v = -1;
            char *p;

            g_csibuf[g_csil] = '\0';
            for (p = g_csibuf; *p; p++) {
                if (*p >= '0' && *p <= '9') {
                    if (v < 0) v = 0;
                    v = v * 10 + (*p - '0');
                } else if (*p == ';' || *p == ':') {
                    if (v >= 0 && nv < (int)(sizeof vals / sizeof vals[0]))
                        vals[nv++] = v;
                    v = -1;
                }
            }
            if (v >= 0 && nv < (int)(sizeof vals / sizeof vals[0]))
                vals[nv++] = v;

            for (int i = 0; i < nv; i++) {
                switch (vals[i]) {
                case 0:  style_set(aSGR0);  break;
                case 1:  style_set(aSGR1);  break;
                case 3:  style_set(aSGR3);  break;
                case 4:  style_set(aSGR4);  break;
                case 22: style_set(aSGR22); break;
                case 23: style_set(aSGR23); break;
                case 24: style_set(aSGR24); break;
                case 39: g_fg = TP_COL_UNSET;               break;
                case 49: g_bg = TP_COL_UNSET;               break;
                case 38:                      /* 256-color / RGB fg */
                    if (i + 1 < nv && vals[i + 1] == 5 && i + 2 < nv) {
                        color_fg_n((unsigned)vals[i + 2]);
                        i += 2;
                    } else if (i + 1 < nv && vals[i + 1] == 2 &&
                               i + 4 < nv) {
                        color_fg_rgb((UBYTE)vals[i + 2],
                                     (UBYTE)vals[i + 3],
                                     (UBYTE)vals[i + 4]);
                        i += 4;
                    }
                    break;
                case 48:                      /* 256-color / RGB bg */
                    if (i + 1 < nv && vals[i + 1] == 5 && i + 2 < nv) {
                        color_bg_n((unsigned)vals[i + 2]);
                        i += 2;
                    } else if (i + 1 < nv && vals[i + 1] == 2 &&
                               i + 4 < nv) {
                        color_bg_rgb((UBYTE)vals[i + 2],
                                     (UBYTE)vals[i + 3],
                                     (UBYTE)vals[i + 4]);
                        i += 4;
                    }
                    break;
                default:                      /* ANSI base/bright */
                    if (vals[i] >= 30 && vals[i] <= 37)
                        color_fg_n((unsigned)(vals[i] - 30));
                    else if (vals[i] >= 90 && vals[i] <= 97)
                        color_fg_n((unsigned)(vals[i] - 90 + 8));
                    else if (vals[i] >= 40 && vals[i] <= 47)
                        color_bg_n((unsigned)(vals[i] - 40));
                    else if (vals[i] >= 100 && vals[i] <= 107)
                        color_bg_n((unsigned)(vals[i] - 100 + 8));
                    break;
                }
            }
        }
        g_osc = OSC_IDLE;
        return 0;

    default:
        g_osc = OSC_IDLE;
        return 0;
    }
}

static struct PrinterData *g_pd;          /* current PrinterData  */

static int ped_init(struct PrinterData *pd)
{
    g_pd       = pd;
    g_doc      = NULL;
    g_req      = NULL;
    g_npages   = 0;
    g_charmap  = (const UBYTE *)sg.ps_PED.ped_8BitChars;
    pdf_page_setup(pd);
    text_reset();
    return 0;
}

static void ped_expunge(void)
{
    if (g_doc) {
        text_flush();
        if (g_npages > 0) pdf_output();
        HPDF_FreeDocAll(g_doc);
    }
    g_doc      = NULL;
    g_req      = NULL;
    g_npages   = 0;
    text_reset();
}

static int ped_open(struct IORequest *ior)
{
    g_req = ior;

    g_doc = HPDF_New(NULL, NULL);
    if (!g_doc) return -1;

    if (g_pd) pdf_page_setup(g_pd);       /* device may have re-prefs */
    g_npages   = 0;
    g_charmap  = (const UBYTE *)sg.ps_PED.ped_8BitChars;
    text_reset();
    return 0;
}

static void ped_close(struct IORequest *ior)
{
    (void)ior;
    if (g_doc) {
        text_flush();
        if (g_npages > 0) pdf_output();
        HPDF_FreeDocAll(g_doc);
    }
    g_doc      = NULL;
    g_req      = NULL;
    g_npages   = 0;
    text_reset();
}

/*
 * Local packed copy of the IODRPReq (printer.device I/O request)
 * used for PRD_DUMPRPORT / PRD_TPEXTDUMPRPORT.
 *
 * We define everything from Message up so the packed layout is
 * correct regardless of the PPC (MorphOS) ABI.
 */
struct PrinterIORP {
    /* struct Message */
    APTR   ln_Succ;
    APTR   ln_Pred;
    UBYTE  ln_Type;
    BYTE   ln_Pri;
    STRPTR ln_Name;
    APTR   mn_ReplyPort;
    UWORD  mn_Length;
    /* struct IORequest */
    APTR   io_Device;
    APTR   io_Unit;
    UWORD  io_Command;
    UBYTE  io_Flags;
    UBYTE  io_Error;
    ULONG  io_Actual;
    ULONG  io_Length;
    APTR   io_Data;
    ULONG  io_Offset;
    /* struct IODRPReq additions */
    APTR   io_RastPort;
    ULONG  io_Modes;
    ULONG  io_ColorMap;
    LONG   io_SrcX;
    LONG   io_SrcY;
    LONG   io_SrcWidth;
    LONG   io_SrcHeight;
    LONG   io_DestCols;
    LONG   io_DestRows;
    ULONG  io_Special;
} __attribute__((packed));

/* ------------------------------------------------------------------
 *  DoSpecial -- dual entry point, two incompatible call shapes
 *
 *  printer.device invokes this in one of two ways:
 *
 *  1. Text form (OS-standard, via a \377 in a Commands[] entry):
 *       LONG fn(UWORD *command, UBYTE *out, BYTE *vline, BYTE *vmi,
 *               BYTE *crlf, UBYTE *params);
 *     command is an ANSI code aSGR0..aSGR24 used here for font styles.
 *  2. TurboPrint IORP form (via PRD_TPEXTDUMPRPORT):
 *       LONG fn(struct IORequest *ior);
 *     ior carries the TPExtIODRP (ptr via io_Modes) + RastPort bitmap.
 *
 *  Dispatch heuristic: the text form always passes a command code
 *  <= aRAW (76); the IORP form passes a pointer whose first word is the
 *  list successor (a memory address, virtually always > 76).  A zero
 *  ln_Succ decodes to aRIS (0), which merely resets the font style --
 *  benign for the worst case.
 * ------------------------------------------------------------------ */

static LONG dospecial_iorp(struct IORequest *ior);

static LONG ped_dospecial(UWORD *command, UBYTE *out,
                          BYTE *pl_curline, BYTE *pl_spacing,
                          BYTE *pl_crlf, UBYTE *params)
{
    UWORD code = *command;

    (void)out;
    (void)pl_curline;
    (void)pl_spacing;
    (void)pl_crlf;
    (void)params;

    if (code <= aRAW) {
        style_set((UBYTE)code);           /* SGR attribute command   */
        return -2;                        /* handled: emit nothing   */
    }

    return dospecial_iorp((struct IORequest *)command);
}

/* TurboPrint IORP path: PRD_TPEXTDUMPRPORT carries a TPExtIODRP
 * (pointer via io_Modes) + RastPort bitmap; read RGB24 and embed via
 * libHaru. */
static LONG dospecial_iorp(struct IORequest *ior)
{
    struct PrinterIORP   *req;
    struct TPExtIODRP    *tp;
    struct RastPort      *rp;
    struct BitMap        *bm;
    UBYTE                *rgb;
    ULONG                 w, h, stride, y;
    int                   st;

    if (ior->io_Command != PRD_TPEXTDUMPRPORT)
        return -1;

    req = (struct PrinterIORP *)ior;
    tp  = (struct TPExtIODRP *)req->io_Modes;
    if (!tp) return -1;

    rp = (struct RastPort *)req->io_RastPort;
    if (!rp || !rp->BitMap) return -1;

    bm     = rp->BitMap;
    w      = req->io_SrcWidth;
    h      = req->io_SrcHeight;
    stride = bm->BytesPerRow;

    if (w == 0 || h == 0 || stride == 0)
        return -1;

    /* Allocate RGB24 buffer for the full page */
    rgb = AllocVec(w * h * 3, MEMF_ANY);
    if (!rgb) return -1;

    /* Copy pixel data from the chunky bitmap (1 plane = RGB pixels).
     * FIXME: planar conversion needed for classic Amiga. */
    if (bm->Depth == 1 && bm->Planes[0]) {
        for (y = 0; y < h; y++)
            memcpy(rgb + y * w * 3,
                   (UBYTE *)bm->Planes[0] + y * stride,
                   w * 3);
    } else {
        memset(rgb, 0x80, w * h * 3);
    }

    /* Embed as a new PDF page via libHaru */
    st = pdf_add_rgb(w, h, rgb);

    FreeVec(rgb);
    return st;
}

/* ------------------------------------------------------------------
 *  Render -- standard raster data path
 *
 *  Called by printer.device for PRD_DUMPRPORT.
 *  We receive raster data via the RastPort, accumulate RGB24 rows and
 *  embed each band in the PDF via libHaru.
 * ------------------------------------------------------------------ */

static int ped_render(struct RastPort *rp,
                      ULONG c, ULONG x, ULONG y, ULONG status)
{
    struct BitMap  *bm;
    ULONG           stride, band_h, need;
    UBYTE          *src, *dst;

    (void)x;
    (void)status;

    switch (c) {

    /* pre-master init -- reset band accumulation */
    case 0:
        g_rowbuf   = NULL;
        g_rowbufsz = 0;
        g_rowstride = 0;
        g_nrows    = 0;
        return 0;

    /* scale, dither & render -- accumulate one band */
    case 1: {
        if (!rp || !rp->BitMap)
            return -1;

        bm     = rp->BitMap;
        band_h = y;                          /* rows in this band */
        stride = bm->BytesPerRow;

        if (band_h == 0 || stride == 0)
            return -1;

        /* Expand accumulation buffer if needed */
        need = stride * (g_nrows + band_h);
        if (need > g_rowbufsz) {
            UBYTE *nb = AllocVec(need, MEMF_ANY);
            if (!nb) return -1;
            if (g_rowbuf) {
                memcpy(nb, g_rowbuf, g_rowbufsz);
                FreeVec(g_rowbuf);
            }
            g_rowbuf   = nb;
            g_rowbufsz = need;
        }
        g_rowstride = stride;

        dst = g_rowbuf + g_nrows * stride;

        /* Read pixel data from the bitmap.
         * One plane -> chunky (CGX/MorphOS).  Multiple planes -> planar
         * (classic Amiga).  For now handle chunky; planar is stubbed. */
        if (bm->Depth == 1) {
            /* Chunky -- pixel data lives in Planes[0] */
            src = bm->Planes[0];
            if (src)
                memcpy(dst, src + g_nrows * stride, stride * band_h);
            else
                memset(dst, 0x80, stride * band_h);
        } else {
            /* Planar -- FIXME: implement proper planar->RGB conversion */
            memset(dst, 0x80, stride * band_h);
        }

        g_nrows += band_h;
        return 0;
    }

    /* output buffer -- embed accumulated rows as a new PDF page */
    case 2: {
        ULONG w, h;
        if (!g_rowbuf || g_nrows == 0 || g_rowstride == 0)
            return -1;

        w = g_rowstride / 3;                 /* pixels per row */
        h = g_nrows;                         /* total rows     */

        if (pdf_add_rgb(w, h, g_rowbuf) != 0) {
            FreeVec(g_rowbuf);
            g_rowbuf   = NULL;
            g_rowbufsz = 0;
            g_rowstride = 0;
            g_nrows    = 0;
            return -1;
        }

        FreeVec(g_rowbuf);
        g_rowbuf   = NULL;
        g_rowbufsz = 0;
        g_rowstride = 0;
        g_nrows    = 0;
        return 0;
    }

    /* post-master */
    case 3:
        return 0;

    /* CR */
    case 4:
        return 0;
    }

    return -1;
}
