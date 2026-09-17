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
| `external/libharu` | libHaru git submodule, built statically into the driver |

---

How it works

The driver is a **PrinterSegment** — a LoadSeg'd code module loaded by
`printer.device`.  It is **not** an Exec device; it has no romtag, no
`RTF_AUTOINIT`, and no `OpenLibrary`/`CloseLibrary` reference.

The first bytes of the loaded module are a `struct PrinterSegment`
containing a `PrinterExtendedData` (PED) which exposes entry points:

  `Init`, `Expunge`, `Open`, `Close`, `DoSpecial`, `Render`

Three data paths:

  **Text path** (text mode → `ped_ConvFunc`)
    printer.device delivers printable characters to the driver's
    `ped_ConvFunc` (requires driver version ≥ 34).  Lines accumulate and
    are emitted as PDF pages via libHaru.  Page geometry is taken from
    the driver's copy of the user's Preferences (`pd_Preferences`):
    the selected paper size (US Letter, A4/A5/A3..A0, Legal, tractor
    feed) maps to PDF page dimensions in points, the left/right margins
    (in characters × pitch) define the printable area, and the spacing
    selection sets the line pitch (6 or 8 lines/inch).  Unknown sizes
    fall back to US Letter.
    `\n`/`\r` start a new line, tab expansion is to 8-column stops, and
    `\014` (formfeed) ejects a page.
    ANSI SGR sequences are recognized through the Commands table entries
    that contain `\377`, which route to `DoSpecial` in its text form:
    `aSGR0` resets, `aSGR1`/`aSGR22` toggle bold, `aSGR3`/`aSGR23` toggle
    italic, `aSGR4`/`aSGR24` toggle underline (per-character styles).
    Colored runs arrive through the 8-bit CSI form (see below): `30-37` /
    `40-47` and `90-97` / `100-107` select the ANSI foreground/background
    palette, `38;5;n` / `48;5;n` select an xterm-256 colour, and
    `38;2;r;g;b` / `48;2;r;g;b` set 24-bit RGB; `39`/`49` restore the
    default.  Colors are shared with the styles: runs split on every
    attribute, a coloured background is painted as a filled rectangle
    behind the run, and underlined runs are stroked in the current
    foreground color.
    The base-14 Helvetica family is used: Helvetica, Helvetica-Bold,
    Helvetica-Oblique, Helvetica-BoldOblique; underlined runs get a
    stroked rule.  The point size follows the selected pitch — PICA
    10pt, ELITE 8.5pt, FINE 7pt — override the default with
    `-DTP_FONTSIZE=<pt>` in the build.
    Characters ≥ 0x80 are mapped through the driver's 8-bit character
    table (`ped_8BitChars`, one set): C1 controls `0x80..0x9F` become
    spaces and the Latin-1 range `0xA0..0xFF` keeps its glyph, so
    accented text prints correctly.

  **Hyperlinks** — text URLs become clickable PDF links:
  * Auto-detection: `http://`, `https://`, `ftp://` and `www.`
    tokens in the printed text get a URI link annotation matching
    the rendered glyphs (trailing sentence punctuation trimmed).
  * OSC 8 (terminal hyperlinks): an explicit link is opened and
    closed in the character stream with
    `ESC ] 8 ; <id> ; <uri> BEL` (or `ESC ] 8 ; ; <uri> ESC \ `) and
    closed with an empty URI.  The region gets a URI link
    annotation with the given URI.  An open link also survives a
    formfeed so multi-line hyperlinks keep working.  `ESC]` is not
    a standard printer escape, so printer.device passes the sequence
    through unmodified for the driver's `ped_ConvFunc` to consume
    (it never appears on the printed page).
  * 8-bit ECMA-48 forms are recognised as well: CSI `0x9B` and OSC
    `0x9D`, terminated by ST `0x9C` (BEL for OSC).  `ped_ConvFunc`
    parses `0x9B <params> m` with the same SGR mapping as the table
    commands (0,1,3,4,22,23,24 -> aSGR0..aSGR24) plus the full color
    subset above, and routes `0x9D` through the same OSC-8 parser as
    the 7-bit `ESC]` form.  Both 8-bit controls are dropped before
    they can reach the text.

  **TurboPrint path** (PRD_TPEXTDUMPRPORT → DoSpecial)
    The driver reads the raster bitmap passed in the `TPExtIODRP`
    request and embeds it as a raw RGB image via `HPDF_LoadRawImageFromMem`.

  **Standard path** (PRD_DUMPRPORT → Render)
    The driver converts the RastPort bitmap to RGB24, accumulates it
    across bands and embeds each band as a raw RGB image via libHaru.

Output
  PDF is written to stdout (via dos.library `Output()`/`Write()`) when
  the printer is closed.  The caller captures stdout to obtain the
  generated PDF.

Supported driver entry points

| Entry | Description |
|-------|-------------|
| Init | Save PrinterData pointer; reset PDF + text state |
| Open | Create new HPDF document |
| Close | Flush final text page + PDF to stdout; free HPDF document |
| Expunge | Emergency cleanup if Close wasn't called |
| ConvFunc | Text-mode: accumulate characters and emit PDF text pages |
| DoSpecial | \377 text-command form: toggle bold/italic/underline styles. IORP form: handle PRD_TPEXTDUMPRPORT (TurboPrint bitmap data). Dispatch is by first word ≤ aRAW (text) vs. a pointer (IORP) |
| Render | Handle PRD_DUMPRPORT (standard RastPort raster data) |

Struct packing
  The `PrinterSegment` and `PrinterExtendedData` structs are defined
  locally with `__attribute__((packed))` to match the exact m68k byte
  layout expected by printer.device, regardless of PPC (MorphOS)
  alignment rules.  The `TPExtIODRP` struct is also packed.

Build
  ```
  make            # release build → build-release/TurboPDF.tpd
                  #   libHaru built statically from the bundled submodule
  make static=0   # dynamic build — link against the system hpdf.library
  make debug=1    # debug build   → build-debug/TurboPDF.tpd
  make check      # clean-builds static + dynamic, verifies output
                  #   make check CHECK_MODES=static   (build one mode only)
  make clean
  ```
  (requires `ppc-morphos-gcc`.  The static build pulls libHaru from the
  `external/libharu` submodule, so no system `hpdf.library` is needed
  there; the dynamic build requires the MorphOS SDK with `hpdf.library`.
  All image and PDF output goes through libHaru — no jpeg/jfif dependency.)

Pull submodule before building:
  ```
  git submodule update --init
  ```

Status
  Untested on real MorphOS hardware.  May need adjustments for
  hpdf.library API compatibility, linker script conventions, and
  bitmap conversion in the Render path.

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

- [ ] Test on real MorphOS hardware.
- [ ] Implement planar → RGB conversion in Render() for classic Amiga bitmaps.
- [ ] Investigate per-PrinterData state for concurrent printer instances.
- [ ] Top/bottom page margins: Preferences exposes no vertical-margin field
      (top-of-form is fixed by the device), so a constant margin is used.
- [ ] Justification/centering: printer.device delivers text pre-formatted and
      provides no alignment control channel, so text is always left-aligned.
- [ ] `pf_PrintColor` is not consulted — the color class is PPCF_COLOR and SGR
      colors are always honoured.
