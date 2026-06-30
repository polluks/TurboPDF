TurboPDF — TurboPrint-compatible PDF printer driver for MorphOS/AmigaOS
=======================================================================

Generates PDF output from TurboPrint raster data using
[libHaru](https://github.com/libharu/libharu) (hpdf.library on MorphOS).

Two components, two usage models:

| File | Model | Purpose |
|------|-------|---------|
| `device.c` | `.tpd` | Installed in `DEVS:Printers/TurboPDF.tpd`, accepts `PRD_DUMPRPORT`/`PRD_TPEXTDUMPRPORT` I/O requests, writes PDF to stdout on close. |
| `turboprint_pdf.c` | Library API | Linked into VPDF or other applications that render pages via Cairo (ARGB32) and want to produce a PDF file directly. |

---

Files

| File | Description |
|------|-------------|
| `device.c` | Printer driver (`.tpd`) — RTF_AUTOINIT, pure C, no assembly |
| `turboprint.h` | TurboPrint pixel format constants + `TPExtIODRP` struct |
| `turboprint_pdf.h` | VPDF-integration API header |
| `turboprint_pdf.c` | VPDF-integration (Cairo ARGB32 → RGB24 → PDF) |
| `Makefile` | Build system for ppc-morphos-gcc |

---

device.c — TurboPrint `.tpd` driver

Architecture
  Standard Amiga Exec device using RTF_AUTOINIT.  The romtag is defined
  in C (no inline assembly) and placed in the `.romtag` section.

  IODRPReq fields are accessed by offset (CopyMem) so the same source
  works on m68k (AmigaOS) and PPC (MorphOS) without struct-padding
  differences.

  The driver binary is named `TurboPDF.tpd` and lives in `DEVS:Printers/`.

Supported I/O commands
  `PRD_DUMPRPORT`       Renders a RastPort region as an RGB24 PDF page.
  `PRD_TPEXTDUMPRPORT`  TurboPrint extension; reads pixel format from
                        the `TPExtIODRP` struct passed in `io_Modes`.
                        Currently only `TPFMT_RGB24` is supported.
  `CMD_RESET/START/STOP`  No-op, returns success.
  Everything else        Returns `IOERR_NOCMD`.

Output
  PDF is written to stdout (via dos.library `Output()`/`Write()`) when the
  device is closed or expunged.  The caller captures stdout to obtain
  the generated PDF.

Build (MorphOS cross-compiler — ppc-morphos-gcc)
  make            # release build → build-release/TurboPDF.tpd
  make debug=1    # debug build   → build-debug/TurboPDF.tpd
  make clean
  (requires morphos-sdk with hpdf.library, Amiga headers, and ppc-morphos-gcc)

Status
  Compiles conceptually but has NOT been tested on real MorphOS hardware
  (no cross-compiler available on this host).  May need adjustments for
  hpdf.library API compatibility and MorphOS linker script conventions.

---

turboprint_pdf.c — Library API for VPDF integration

Designed for applications that render pages to Cairo surfaces (ARGB32)
and want to produce PDF output.  Converts ARGB32 to packed RGB24 on
the fly and embeds each page as a raw RGB24 image in the PDF.

API
  struct PDFPrinter *pdfPrintPdfInit(const char *path,
      int first, int last, int dpi_x, int dpi_y);
      Create a new PDF document.  path is the output filename
      (used by HPDF_SaveToFile).  first/last are ignored (reserved).
      dpi_x/dpi_y determine page dimensions from pixel dimensions.

  BOOL pdfPrintPdfPage(struct PDFPrinter *pctx,
      const UBYTE *argb32_data, int width, int height,
      int stride, int page_num);
      Add a page.  argb32_data is a top-down ARGB32 buffer with
      the given stride (bytes per row).  page_num is informational.

  void pdfPrintPdfEnd(struct PDFPrinter *pctx);
      Finalize and save the PDF, then free all resources.

Build
  ppc-morphos-gcc -mcpu=750 -lhpdf -o turboprint_pdf_test \
      turboprint_pdf.c
  (link into your VPDF build with the same flags)

---

References

TurboPrint
- TurboPrint SDK (tp_devel.lha, 9k):
  https://www.irseesoft.de/tp_what.htm
- Canonical turboprint.h (AROS-vpdf):
  https://github.com/wattoc/AROS-vpdf/blob/master/turboprint.h
- VPDF TurboPrint printer path (poppler_printer.cpp):
  https://github.com/wattoc/AROS-vpdf/blob/master/poppler/poppler_printer.cpp
- TurboPrint on Wikipedia:
  https://en.wikipedia.org/wiki/TurboPrint

libHaru
- Homepage / source:      https://github.com/libharu/libharu
- API reference:          https://libharu.sourceforge.net/api.html

Amiga/MorphOS printing
- Amiga Printer Device docs (RKRM):
  https://wiki.amigaos.net/wiki/Printer_Device
- MorphOS Printing overview (PDF):
  https://www.morphos-storage.net/?id=1588365
- MorphOS Library — Printing:
  https://library.morph.zone/Printing
- RKRM_Devs source archive (Aminet):
  http://aminet.net/package/dev/src/RKRM_Devs_prgs

Device driver skeleton
- SimpleDevice:  https://github.com/jbilander/SimpleDevice

---

TODO

[ ] Add JPEG embedding inside PDF (HPDF_LoadJpegImageFromMem) for
    smaller file sizes.
[ ] Support more pixel formats (TPFMT_RGB16, TPFMT_Chunky8).
[ ] Write a test program that exercises OpenDevice()/PRD_DUMPRPORT()/
    CloseDevice() with a synthetic RastPort.
[ ] Write Makefile for MorphOS (ppc-morphos-gcc).
[ ] Test on real MorphOS hardware.
