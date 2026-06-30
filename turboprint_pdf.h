#ifndef TURBOPRINT_PDF_H
#define TURBOPRINT_PDF_H

#include <exec/types.h>

#define VPDF_PRINT_PDF 3

struct PDFPrinter;

struct PDFPrinter *pdfPrintPdfInit(const char *path, int first, int last,
                                   int dpi_x, int dpi_y);
BOOL pdfPrintPdfPage(struct PDFPrinter *pctx, const UBYTE *argb32_data,
                     int width, int height, int stride, int page_num);
void pdfPrintPdfEnd(struct PDFPrinter *pctx);

#endif
