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

/* TurboPrint extension (V45+) */
#define PRD_TPEXTDUMPRPORT  (PRD_DUMPRPORT | 0x80)

/*
 * TurboPrint pixel format constants (tpd_Compression / Mode values)
 */
#define TPFMT_BitPlanes     0x00
#define TPFMT_Chunky8       0x01
#define TPFMT_RGB15         0x12
#define TPFMT_RGB16         0x13
#define TPFMT_RGB24         0x14
#define TPFMT_BGR15         0x02
#define TPFMT_BGR16         0x03
#define TPFMT_BGR24         0x04
#define TPFMT_HAM           0x800
#define TPFMT_EHB           0x080
#define TPFMT_CyberGraphX   0x400

#define TPMATCHWORD         0xf10a57ef

/*
 * TurboPrint extended I/O request structure.
 *
 * Passed via io_Modes in the IODRPReq sent with PRD_TPEXTDUMPRPORT.
 * The SDK struct is exactly 3 UWORDs:
 *
 *     PixAspX, PixAspY, Mode
 *
 * This is the official TurboPrint 3.x/4.x definition from IrseeSoft.
 */
struct TPExtIODRP {
    UWORD PixAspX;          /* pixel aspect ratio X     */
    UWORD PixAspY;          /* pixel aspect ratio Y     */
    UWORD Mode;             /* pixel format (TPFMT_*)  */
} __attribute__((packed));
/* The internal TurboPrint driver appends Planes[8], BytesPerRow, XOffset
 * after Mode; these fields are filled by the driver, not the caller. */

#endif
