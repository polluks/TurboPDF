/*
 * turboprint.pdf.device — TurboPrint-compatible printer device
 * that outputs PDF via libHaru (hpdf.library).
 *
 * Pure C — no inline assembly.  Uses the RTF_AUTOINIT romtag model
 * via a resident struct placed in the ".romtag" section.
 *
 * Based on the driver skeleton from:
 *   https://github.com/jbilander/SimpleDevice
 *
 * Printer I/O fields follow the standard IODRPReq layout
 * (Amiga ROM Kernel Reference Manual: Devices, 3rd ed.).
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <devices/printer.h>

#include <hpdf.h>

#include "turboprint.h"

#if DEBUG
extern void KPrintF(CONST_STRPTR fmt, ...);
#endif

#define STR_(s) #s
#define STR(s)  STR_(s)

#define DEVICE_NAME       "TurboPDF.tpd"
#define DEVICE_DATE       "(30 Jun 2026)"
#define DEVICE_ID_STRING  "TurboPDF " \
    STR(DEVICE_VERSION) "." STR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_VERSION    1
#define DEVICE_REVISION   0
#define DEVICE_PRIORITY   0

/* ---------------------------------------------------------------
 *  IODRPReq field offsets (works on m68k and PPC)
 *
 *  struct IODRPReq {
 *      struct Message  io_Message;        0 (+ struct Node 0-13, +mn_ReplyPort 14, +mn_Length 18)
 *      struct Device  *io_Device;        20
 *      struct Unit    *io_Unit;          24
 *      UWORD           io_Command;       28
 *      UBYTE           io_Flags;         30
 *      BYTE            io_Error;         31
 *      struct RastPort *io_RastPort;     32
 *      struct ColorMap *io_ColorMap;     36
 *      ULONG           io_Modes;         40
 *      UWORD           io_SrcX;          44
 *      UWORD           io_SrcY;          46
 *      UWORD           io_SrcWidth;      48
 *      UWORD           io_SrcHeight;     50
 *      LONG            io_DestCols;      52
 *      LONG            io_DestRows;      56
 *      UWORD           io_Special;       60
 *  };
 * --------------------------------------------------------------- */
#define OFF_RASTPORT  32
#define OFF_SRCWIDTH  48
#define OFF_SRCHEIGHT 50
#define OFF_MODES     40

static struct RastPort *get_rp(struct IORequest *ior)
{
    struct RastPort *rp;
    CopyMem((UBYTE *)ior + OFF_RASTPORT, &rp, sizeof(rp));
    return rp;
}
static UWORD get_sw(struct IORequest *ior)
{
    UWORD v;
    CopyMem((UBYTE *)ior + OFF_SRCWIDTH, &v, 2);
    return v;
}
static UWORD get_sh(struct IORequest *ior)
{
    UWORD v;
    CopyMem((UBYTE *)ior + OFF_SRCHEIGHT, &v, 2);
    return v;
}
static ULONG get_modes(struct IORequest *ior)
{
    ULONG v;
    CopyMem((UBYTE *)ior + OFF_MODES, &v, 4);
    return v;
}

/* ---------------------------------------------------------------
 *  per-instance data
 * --------------------------------------------------------------- */

struct ExecBase *SysBase;
BPTR   saved_seg_list;
BOOL   is_open;
HPDF_Doc  g_doc;
int       g_npages;
int       g_dpi;

/* ---------------------------------------------------------------
 *  PDF helpers
 * --------------------------------------------------------------- */

static BOOL write_pdf(void)
{
    HPDF_STATUS st;
    HPDF_UINT32 sz;
    HPDF_BYTE *buf;
    BPTR out;

    st = HPDF_SaveToStream(g_doc);
    if (st != HPDF_OK) return FALSE;

    sz = HPDF_GetStreamSize(g_doc);
    if (sz == 0) return FALSE;

    buf = AllocVec(sz, MEMF_ANY);
    if (!buf) return FALSE;

    st = HPDF_GetContents(g_doc, buf, &sz);
    if (st != HPDF_OK) { FreeVec(buf); return FALSE; }

    out = Output();
    if (out) Write(out, buf, (LONG)sz);
    FreeVec(buf);
    return TRUE;
}

static void add_rgb24_page(struct RastPort *rp, UWORD w, UWORD h)
{
    HPDF_Page pg;
    HPDF_Image im;
    HPDF_REAL pw, ph;
    struct BitMap *bm;
    UBYTE *rgb;
    int y;

    if (!rp || !rp->BitMap || w == 0 || h == 0) return;
    bm = rp->BitMap;

    pg = HPDF_AddPage(g_doc);
    if (!pg) return;

    pw = (HPDF_REAL)w * 72.0f / (HPDF_REAL)g_dpi;
    ph = (HPDF_REAL)h * 72.0f / (HPDF_REAL)g_dpi;
    HPDF_Page_SetWidth(pg, pw);
    HPDF_Page_SetHeight(pg, ph);

    rgb = AllocVec((ULONG)w * (ULONG)h * 3, MEMF_ANY);
    if (!rgb) return;

    for (y = 0; y < (int)h; y++)
    {
        UBYTE *src = (UBYTE *)bm->Planes[0] + (ULONG)y * bm->BytesPerRow;
        CopyMem(src, rgb + (ULONG)y * (ULONG)w * 3, (ULONG)w * 3);
    }

    im = HPDF_LoadRawImageFromMem(g_doc, rgb, w, h,
                                  HPDF_CS_DEVICE_RGB, 8);
    FreeVec(rgb);
    if (im)
    {
        HPDF_Page_DrawImage(pg, im, 0, 0, pw, ph);
        g_npages++;
    }
}

/* ---------------------------------------------------------------
 *  Exec device entry points
 * --------------------------------------------------------------- */

static BPTR do_expunge(struct Library *dev)
{
#if DEBUG
    KPrintF("do_expunge()\n");
#endif
    if (dev->lib_OpenCnt != 0)
    {
        dev->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    if (g_doc)
    {
        if (g_npages > 0) write_pdf();
        HPDF_FreeDocAll(g_doc);
        g_doc = NULL;
    }
    g_npages = 0;
    {
        BPTR seg = saved_seg_list;
        Remove(&dev->lib_Node);
        FreeMem((char *)dev - dev->lib_NegSize,
                dev->lib_NegSize + dev->lib_PosSize);
        return seg;
    }
}

static void do_open(struct Library *dev, struct IORequest *ior,
                    ULONG unit, ULONG flags)
{
    (void)flags;
#if DEBUG
    KPrintF("do_open()\n");
#endif
    ior->io_Error = IOERR_OPENFAIL;
    ior->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    if (unit != 0) return;

    if (!is_open)
    {
        g_doc = HPDF_New(NULL, NULL);
        if (!g_doc) return;
        if (HPDF_NewDoc(g_doc) != HPDF_OK)
        {
            HPDF_Free(g_doc);
            g_doc = NULL;
            return;
        }
        g_npages = 0;
        g_dpi = 300;
        is_open = TRUE;
    }
    dev->lib_OpenCnt++;
    ior->io_Error = 0;
}

static BPTR do_close(struct Library *dev, struct IORequest *ior)
{
#if DEBUG
    KPrintF("do_close()\n");
#endif
    ior->io_Device = NULL;
    ior->io_Unit   = NULL;
    dev->lib_OpenCnt--;

    if (dev->lib_OpenCnt == 0)
    {
        if (g_doc)
        {
            if (g_npages > 0) write_pdf();
            HPDF_FreeDocAll(g_doc);
            g_doc = NULL;
        }
        g_npages = 0;
        is_open = FALSE;
    }
    if (dev->lib_OpenCnt == 0 && (dev->lib_Flags & LIBF_DELEXP))
        return do_expunge(dev);
    return 0;
}

static void do_begin_io(struct Library *dev, struct IORequest *ior)
{
    (void)dev;
#if DEBUG
    KPrintF("do_begin_io() cmd %lu\n", ior->io_Command);
#endif

    switch (ior->io_Command)
    {
    case PRD_DUMPRPORT:
    {
        struct RastPort *rp = get_rp(ior);
        if (rp) add_rgb24_page(rp, get_sw(ior), get_sh(ior));
        ior->io_Error = 0;
        break;
    }
    case PRD_TPEXTDUMPRPORT:
    {
        struct RastPort *rp = get_rp(ior);
        ULONG m = get_modes(ior);
        UWORD fmt = ((struct TPExtIODRP *)m)
                        ? ((struct TPExtIODRP *)m)->Mode
                        : TPFMT_RGB24;
        if (rp && fmt == TPFMT_RGB24)
            add_rgb24_page(rp, get_sw(ior), get_sh(ior));
        ior->io_Error = 0;
        break;
    }
    case CMD_RESET:
    case CMD_START:
    case CMD_STOP:
        ior->io_Error = 0;
        break;
    default:
        ior->io_Error = IOERR_NOCMD;
        break;
    }

    if (!(ior->io_Flags & IOF_QUICK))
        ReplyMsg(&ior->io_Message);
}

static ULONG do_abort_io(struct Library *dev, struct IORequest *ior)
{
    (void)dev; (void)ior;
    return IOERR_ABORTED;
}

/* ---------------------------------------------------------------
 *  RTF_AUTOINIT function vectors and entry points
 * --------------------------------------------------------------- */

static BPTR  expunge_entry(struct Library *dev asm("a6"))
{
    return do_expunge(dev);
}
static void  open_entry(struct Library *dev asm("a6"),
                        struct IORequest *ior asm("a1"),
                        ULONG unit asm("d0"), ULONG flags asm("d1"))
{
    do_open(dev, ior, unit, flags);
}
static BPTR  close_entry(struct Library *dev asm("a6"),
                         struct IORequest *ior asm("a1"))
{
    return do_close(dev, ior);
}
static void  begin_io_entry(struct Library *dev asm("a6"),
                            struct IORequest *ior asm("a1"))
{
    do_begin_io(dev, ior);
}
static ULONG abort_io_entry(struct Library *dev asm("a6"),
                            struct IORequest *ior asm("a1"))
{
    return do_abort_io(dev, ior);
}

static const ULONG device_vectors[] =
{
    (ULONG)open_entry,
    (ULONG)close_entry,
    (ULONG)expunge_entry,
    0,
    (ULONG)begin_io_entry,
    (ULONG)abort_io_entry,
    (ULONG)-1
};

static struct Library __attribute__((used)) *
init_device(struct ExecBase *sysbase asm("a6"),
            BPTR seglist asm("a0"),
            struct Library *dev asm("d0"))
{
    SysBase = sysbase;
#if DEBUG
    KPrintF("init_device(): turboprint.pdf.device\n");
#endif
    saved_seg_list = seglist;

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = (STRPTR)DEVICE_NAME;
    dev->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib_Version = DEVICE_VERSION;
    dev->lib_Revision = DEVICE_REVISION;
    dev->lib_IdString = (APTR)DEVICE_ID_STRING;

    g_doc = NULL; g_npages = 0; g_dpi = 300; is_open = FALSE;
    return dev;
}

static const ULONG auto_init_tables[4] =
{
    sizeof(struct Library),
    (ULONG)device_vectors,
    0,
    (ULONG)init_device
};

/* ---------------------------------------------------------------
 *  Romtag — placed at a fixed section so it appears before any code.
 *
 *  The linker script places .romtag at offset 4 (right after the
 *  _start safety-net).  We use __attribute__((section(".romtag")))
 *  to control placement.
 * --------------------------------------------------------------- */

static const struct Resident romtag
__attribute__((used, section(".romtag"))) =
{
    .rt_MatchWord   = RTC_MATCHWORD,
    .rt_MatchTag    = (APTR)&romtag,
    .rt_EndSkip     = (APTR)&romtag + sizeof(romtag),
    .rt_Flags       = RTF_AUTOINIT,
    .rt_Version     = DEVICE_VERSION,
    .rt_Type        = NT_DEVICE,
    .rt_Pri         = DEVICE_PRIORITY,
    .rt_Name        = device_name,
    .rt_IdString    = device_id_string,
    .rt_Init        = (APTR)auto_init_tables
};

/* _start — if someone tries to run the file as a program, return -1 */
int __attribute__((no_reorder)) _start(void)
{
    return -1;
}

char device_name[] = DEVICE_NAME;
char device_id_string[] = DEVICE_ID_STRING;
