#ifndef TURBOPRINT_H
#define TURBOPRINT_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

/*
 * Standard printer.device I/O commands (from devices/printer.h)
 * CMD_NONSTD = 0x200 (from exec/io.h)
 */
#define CMD_NONSTD        0x200
#define PRD_DUMPRPORT     (CMD_NONSTD + 0)
#define PRD_QUERY         (CMD_NONSTD + 1)
#define PRD_PRTCOMMAND    (CMD_NONSTD + 2)
#define PRD_DUMPRPORT2    (CMD_NONSTD + 3)
#define PRD_NEXTBAND      (CMD_NONSTD + 4)
#define PRD_STARTBAND     (CMD_NONSTD + 5)
#define PRD_SETDENSITY    (CMD_NONSTD + 6)

/* TurboPrint extension (V45+ of TurboPrint driver) */
#define PRD_TPEXTDUMPRPORT  (PRD_DUMPRPORT | 0x80)

/*
 * TurboPrint pixel formats (tpd_Compression values)
 */
#define TPFMT_RGB24         0x14
#define TPFMT_JPEG          0x05

/*
 * TurboPrint extended I/O request (EIODRP extension).
 *
 * The base EIODRP (printer.device) has:
 *   PixAspX, PixAspY, Mode  (3 UWORDs = 6 bytes)
 *
 * TurboPrint adds fields after those.  We define the whole thing
 * packed so the offsets match the m68k ABI on both big- and little-
 * endian MorphOS PPC.
 */
struct TPExtIODRP {
    UWORD  PixAspX;
    UWORD  PixAspY;
    UWORD  Mode;
    ULONG  tpd_XDPI;
    ULONG  tpd_YDPI;
    ULONG  tpd_Color;
    ULONG  tpd_Compression;
    ULONG  tpd_Width;
    ULONG  tpd_Height;
    APTR   tpd_Buf;          /* JPEG or RGB data   */
    ULONG  tpd_BufSize;      /* bytes of data      */
} __attribute__((packed));

#endif
