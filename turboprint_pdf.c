#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <exec/exec.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <hpdf.h>

#include "turboprint_pdf.h"

struct PDFPrinter
{
    HPDF_Doc   doc;
    char       path[1024];
    int        dpi_x;
    int        dpi_y;
    int        page_count;
    HPDF_STATUS last_error;
};

static void HPDF_STDCALL ErrorHandler(HPDF_STATUS error_no,
                                      HPDF_STATUS detail_no,
                                      void *user_data)
{
    struct PDFPrinter *pctx = (struct PDFPrinter *)user_data;
    pctx->last_error = error_no;
    KPrintF("hpdf error: %08lx, detail: %08lx\n", error_no, detail_no);
}

struct PDFPrinter *pdfPrintPdfInit(const char *path, int first, int last,
                                   int dpi_x, int dpi_y)
{
    struct PDFPrinter *pctx;

    (void)first;
    (void)last;

    pctx = (struct PDFPrinter *)calloc(1, sizeof(*pctx));
    if (!pctx)
        return NULL;

    pctx->dpi_x = dpi_x;
    pctx->dpi_y = dpi_y;
    pctx->page_count = 0;

    strncpy(pctx->path, path, sizeof(pctx->path) - 1);
    pctx->path[sizeof(pctx->path) - 1] = '\0';

    pctx->doc = HPDF_New(ErrorHandler, pctx);
    if (!pctx->doc)
    {
        KPrintF("HPDF_New failed\n");
        free(pctx);
        return NULL;
    }

    if (HPDF_NewDoc(pctx->doc) != HPDF_OK)
    {
        KPrintF("HPDF_NewDoc failed\n");
        HPDF_Free(pctx->doc);
        free(pctx);
        return NULL;
    }

    return pctx;
}

BOOL pdfPrintPdfPage(struct PDFPrinter *pctx, const UBYTE *argb32_data,
                     int width, int height, int stride, int page_num)
{
    HPDF_Page page;
    HPDF_Image image;
    HPDF_REAL page_w, page_h;
    UBYTE *rgb;
    const UBYTE *src_row;
    UBYTE *dst;
    int x, y;

    if (!pctx || !pctx->doc)
        return FALSE;

    page = HPDF_AddPage(pctx->doc);
    if (!page)
    {
        pctx->last_error = HPDF_GetError(pctx->doc);
        KPrintF("HPDF_AddPage failed for page %d (err %08lx)\n",
                page_num, HPDF_GetError(pctx->doc));
        return FALSE;
    }

    page_w = (HPDF_REAL)width  * 72.0f / (HPDF_REAL)pctx->dpi_x;
    page_h = (HPDF_REAL)height * 72.0f / (HPDF_REAL)pctx->dpi_y;

    HPDF_Page_SetWidth (page, page_w);
    HPDF_Page_SetHeight(page, page_h);

    rgb = (UBYTE *)malloc((ULONG)width * (ULONG)height * 3);
    if (!rgb)
        return FALSE;

    src_row = argb32_data;
    dst = rgb;

    for (y = 0; y < height; y++)
    {
        const LONG *pixels = (const LONG *)src_row;
        for (x = 0; x < width; x++)
        {
            LONG pixel = pixels[x];
            *dst++ = (UBYTE)((pixel >> 16) & 0xFF);
            *dst++ = (UBYTE)((pixel >> 8) & 0xFF);
            *dst++ = (UBYTE)( pixel        & 0xFF);
        }
        src_row += stride;
    }

    image = HPDF_LoadRawImageFromMem(pctx->doc, rgb,
                                     (HPDF_UINT)width, (HPDF_UINT)height,
                                     HPDF_CS_DEVICE_RGB, 8);

    free(rgb);

    if (!image)
    {
        pctx->last_error = HPDF_GetError(pctx->doc);
        KPrintF("HPDF_LoadRawImageFromMem failed for page %d (err %08lx)\n",
                page_num, HPDF_GetError(pctx->doc));
        return FALSE;
    }

    HPDF_Page_DrawImage(page, image, 0, 0, page_w, page_h);

    pctx->page_count++;
    return TRUE;
}

void pdfPrintPdfEnd(struct PDFPrinter *pctx)
{
    HPDF_STATUS err;

    if (!pctx)
        return;

    if (pctx->doc)
    {
        if (pctx->page_count > 0)
        {
            err = HPDF_SaveToFile(pctx->doc, pctx->path);
            if (err != HPDF_OK)
            {
                KPrintF("HPDF_SaveToFile('%s') failed: %08lx\n",
                        pctx->path, err);
            }
        }

        HPDF_FreeDocAll(pctx->doc);
        pctx->doc = NULL;
    }

    free(pctx);
}
