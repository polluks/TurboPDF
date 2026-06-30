/*
 * TurboPDF.tpd — TurboPrint-compatible printer driver (PrinterSegment)
 * that outputs PDF via libHaru (hpdf.library) to stdout.
 *
 * Pure C — no assembly.  The PrinterSegment + PrinterExtendedData
 * structs are defined locally with __attribute__((packed)) to match
 * the exact byte layout that printer.device expects on both m68k and
 * PPC (MorphOS).
 *
 * Two data paths:
 *   - PRD_TPEXTDUMPRPORT (TurboPrint, via DoSpecial): receives pre-
 *     compressed JPEG, embeds directly in PDF.
 *   - PRD_DUMPRPORT (standard, via Render): receives planar/chunky
 *     bitmap from RastPort; compresses to JPEG via -ljpeg (jpeg.library),
 *     then embeds in PDF.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <jpeglib.h>
#include <hpdf.h>

#include "turboprint.h"

#define STR_(s) #s
#define STR(s)  STR_(s)

#define DRIVER_VERSION   1
#define DRIVER_REVISION  0

/* ------------------------------------------------------------------
 *  Packed structs — match the exact m68k byte layout from
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
static int   __attribute__((used)) ped_dospecial(struct IORequest *ior);
static int   __attribute__((used)) ped_render(struct RastPort *rp,
                                              ULONG c, ULONG x,
                                              ULONG y, ULONG status);

/* ------------------------------------------------------------------
 *  PrinterSegment header — MUST be the very first thing in the code
 *  hunk.  __attribute__((section(".text"))) places it in the code
 *  section; combined with -nostartfiles it becomes the first word
 *  of the LoadSeg'd module.
 * ------------------------------------------------------------------ */

static const STRPTR sg_name  = "TurboPDF";
static const APTR   sg_cmds[] = { NULL };

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
        .ped_MaxColumns   = 0,
        .ped_NumCharSets  = 0,
        .ped_NumRows      = 1,
        .ped_MaxXDots     = 0,
        .ped_MaxYDots     = 0,
        .ped_XDotsInch    = 300,
        .ped_YDotsInch    = 300,
        .ped_Commands     = (APTR)sg_cmds,
        .ped_DoSpecial    = ped_dospecial,
        .ped_Render       = ped_render,
        .ped_TimeoutSecs  = 120L,
        .ped_8BitChars    = NULL,
        .ped_PrintMode    = 0,
        .ped_ConvFunc     = NULL,
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
    if (sz == 0) return 0;                  /* empty — nothing to do */

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

/* Add a JPEG buffer to the current PDF as a new page.
 *  w, h  – image dimensions in pixels
 *  jpeg  – pointer to JPEG-compressed data
 *  jsz   – size of JPEG data */
static int pdf_add_jpeg(ULONG w, ULONG h,
                        const HPDF_BYTE *jpeg, HPDF_UINT32 jsz)
{
    HPDF_Page  pg;
    HPDF_Image im;
    HPDF_REAL  pw, ph;

    if (!g_doc) return -1;

    im = HPDF_LoadJpegImageFromMem(g_doc, jpeg, jsz);
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

/* Compress RGB24 data to JPEG in memory using -ljpeg (jpeg.library).
 *  *out     receives malloc'd buffer (caller must FreeVec)
 *  *outsz   receives size of the buffer
 * Returns 0 on success, -1 on failure. */
static int rgb_to_jpeg(const UBYTE *rgb, int w, int h, int quality,
                       HPDF_BYTE **out, HPDF_UINT32 *outsz)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr       jerr;
    JSAMPROW                   row[1];
    int                        y;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, out, (unsigned long *)outsz);
    if (!*out) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    cinfo.image_width      = w;
    cinfo.image_height     = h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < (JDIMENSION)h) {
        row[0] = (JSAMPROW)(rgb + cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return 0;
}

/* ------------------------------------------------------------------
 *  Driver entry points
 * ------------------------------------------------------------------ */

static int ped_init(struct PrinterData *pd)
{
    (void)pd;
    g_doc    = NULL;
    g_req    = NULL;
    g_npages = 0;
    return 0;
}

static void ped_expunge(void)
{
    if (g_doc) {
        if (g_npages > 0) pdf_output();
        HPDF_FreeDocAll(g_doc);
    }
    g_doc    = NULL;
    g_req    = NULL;
    g_npages = 0;
}

static int ped_open(struct IORequest *ior)
{
    g_req = ior;

    g_doc = HPDF_New(NULL, NULL);
    if (!g_doc) return -1;

    g_npages = 0;
    return 0;
}

static void ped_close(struct IORequest *ior)
{
    (void)ior;
    if (g_doc) {
        if (g_npages > 0) pdf_output();
        HPDF_FreeDocAll(g_doc);
    }
    g_doc    = NULL;
    g_req    = NULL;
    g_npages = 0;
}

/* ------------------------------------------------------------------
 *  DoSpecial — handles the TurboPrint data path
 *
 *  PRD_TPEXTDUMPRPORT  receives a TPExtIODRP with pre-compressed
 *  JPEG (or RGB24) raster data.  We embed it in the PDF document.
 * ------------------------------------------------------------------ */

static int ped_dospecial(struct IORequest *ior)
{
    struct TPExtIODRP *tp;

    if (ior->io_Command != PRD_TPEXTDUMPRPORT)
        return -1;                          /* not handled */

    tp = (struct TPExtIODRP *)ior->io_Data;
    if (!tp) return -1;

    /* Only handle JPEG-compressed data for now.
     * TPFMT_RGB24 support requires libjpeg compression here too. */
    if (tp->tpd_Compression == TPFMT_JPEG) {
        pdf_add_jpeg(tp->tpd_Width, tp->tpd_Height,
                     (HPDF_BYTE *)tp->tpd_Buf,
                     (HPDF_UINT32)tp->tpd_BufSize);
        return 0;
    }

    /* Unknown compression — skip */
    return -1;
}

/* ------------------------------------------------------------------
 *  Render — standard raster data path
 *
 *  Called by printer.device for PRD_DUMPRPORT.
 *  We receive raster data via the RastPort and convert it to JPEG.
 * ------------------------------------------------------------------ */

static int ped_render(struct RastPort *rp,
                      ULONG c, ULONG x, ULONG y, ULONG status)
{
    struct BitMap  *bm;
    ULONG           stride, band_h, need;
    UBYTE          *src, *dst;
    HPDF_BYTE      *jpeg;
    HPDF_UINT32     jsz;

    (void)x;
    (void)status;

    switch (c) {

    /* pre-master init — reset band accumulation */
    case 0:
        g_rowbuf   = NULL;
        g_rowbufsz = 0;
        g_rowstride = 0;
        g_nrows    = 0;
        return 0;

    /* scale, dither & render — accumulate one band */
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
                CopyMem(g_rowbuf, nb, g_rowbufsz);
                FreeVec(g_rowbuf);
            }
            g_rowbuf   = nb;
            g_rowbufsz = need;
        }
        g_rowstride = stride;

        dst = g_rowbuf + g_nrows * stride;

        /* Read pixel data from the bitmap.
         * One plane → chunky (CGX/MorphOS).  Multiple planes → planar
         * (classic Amiga).  For now handle chunky; planar is stubbed. */
        if (bm->Depth == 1) {
            /* Chunky — pixel data lives in Planes[0] */
            src = bm->Planes[0];
            if (src)
                CopyMem(src + g_nrows * stride, dst, stride * band_h);
            else
                SetMem(dst, 0x80, stride * band_h);
        } else {
            /* Planar — FIXME: implement proper planar→RGB conversion */
            SetMem(dst, 0x80, stride * band_h);
        }

        g_nrows += band_h;
        return 0;
    }

    /* output buffer — compress accumulated rows to JPEG, add to PDF */
    case 2: {
        ULONG w, h;
        if (!g_rowbuf || g_nrows == 0 || g_rowstride == 0)
            return -1;

        w = g_rowstride / 3;                 /* pixels per row */
        h = g_nrows;                         /* total rows     */

        jpeg = NULL;
        jsz  = 0;

        if (rgb_to_jpeg(g_rowbuf, w, h, 90, &jpeg, &jsz) == 0 && jpeg) {
            pdf_add_jpeg(w, h, jpeg, jsz);
            FreeVec(jpeg);
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
