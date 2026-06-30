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
| `device.c` | Printer driver (`.tpd`) — RTF_AUTOINIT, pure C, no assembly |
| `turboprint.h` | TurboPrint pixel format constants + `TPExtIODRP` struct |
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

Build
  make            # release build → build-release/TurboPDF.tpd
  make debug=1    # debug build   → build-debug/TurboPDF.tpd
  make clean
  (requires ppc-morphos-gcc, morphos-sdk with hpdf.library and Amiga headers)

Status
  Untested on real MorphOS hardware.  May need adjustments for
  hpdf.library API compatibility and MorphOS linker script conventions.

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
[ ] Test on real MorphOS hardware.
