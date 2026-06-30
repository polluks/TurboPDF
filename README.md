TurboPDF — TurboPrint-compatible PDF printer driver for MorphOS/AmigaOS
=======================================================================

TurboPrint driver (`TurboPDF.tpd`) that intercepts `PRD_DUMPRPORT` /
`PRD_TPEXTDUMPRPORT` I/O requests and outputs a PDF to stdout using
[libHaru](https://github.com/libharu/libharu) (hpdf.library on MorphOS).

Any Amiga application can select it in the printer preferences and
capture stdout to obtain the generated PDF.

---

Files

| File | Description |
|------|-------------|
| `tpd.c` | PrinterSegment driver — pure C, no assembly |
| `turboprint.h` | TurboPrint pixel format constants + `TPExtIODRP` struct |
| `Makefile` | Build system for ppc-morphos-gcc |

---

How it works

The driver is a **PrinterSegment** — a LoadSeg'd code module loaded by
`printer.device`.  It is **not** an Exec device; it has no romtag, no
`RTF_AUTOINIT`, and no `OpenLibrary`/`CloseLibrary` reference.

The first bytes of the loaded module are a `struct PrinterSegment`
containing a `PrinterExtendedData` (PED) which exposes entry points:

  `Init`, `Expunge`, `Open`, `Close`, `DoSpecial`, `Render`

Two data paths:

  **TurboPrint path** (PRD_TPEXTDUMPRPORT → DoSpecial)
    The application sends pre-compressed JPEG data in a `TPExtIODRP`
    struct.  The driver embeds the JPEG directly into the PDF document
    via `HPDF_LoadJpegImageFromMem`.

  **Standard path** (PRD_DUMPRPORT → Render)
    The application sends a RastPort with raster data.  The driver
    converts the bitmap to RGB24, compresses it to JPEG using the
    libjpeg API (via `-ljfif`, wrapping `jfif.library`), then embeds
    it in the PDF.

Output
  PDF is written to stdout (via dos.library `Output()`/`Write()`) when
  the printer is closed.  The caller captures stdout to obtain the
  generated PDF.

Supported driver entry points

| Entry | Description |
|-------|-------------|
| Init | Save PrinterData pointer; reset PDF state |
| Open | Create new HPDF document |
| Close | Flush PDF to stdout; free HPDF document |
| Expunge | Emergency cleanup if Close wasn't called |
| DoSpecial | Handle PRD_TPEXTDUMPRPORT (TurboPrint JPEG data) |
| Render | Handle PRD_DUMPRPORT (standard RastPort raster data) |

Struct packing
  The `PrinterSegment` and `PrinterExtendedData` structs are defined
  locally with `__attribute__((packed))` to match the exact m68k byte
  layout expected by printer.device, regardless of PPC (MorphOS)
  alignment rules.  The `TPExtIODRP` struct is also packed.

Build
  make            # release build → build-release/TurboPDF.tpd
  make debug=1    # debug build   → build-debug/TurboPDF.tpd
  make clean
  (requires ppc-morphos-gcc, morphos-sdk with hpdf.library and jfif.library)

Status
  Untested on real MorphOS hardware.  May need adjustments for
  hpdf.library / jfif.library API compatibility, linker script
  conventions, and bitmap conversion in the Render path.

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

PrinterSegment docs
- https://wiki.amigaos.net/wiki/Printer_Device#Creating_a_Printer_Driver

---

TODO

[ ] Test on real MorphOS hardware.
[ ] Implement planar → RGB conversion in Render() for classic Amiga bitmaps.
[ ] Handle TPFMT_RGB24 in DoSpecial (compress with libjfif).
[ ] Add multiple-page PDF support (one page per dump).
[ ] Investigate per-PrinterData state for concurrent printer instances.
