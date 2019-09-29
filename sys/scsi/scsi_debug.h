/*	$OpenBSD: scsi_debug.h,v 1.18 2019/09/29 17:23:24 krw Exp $	*/
/*	$NetBSD: scsi_debug.h,v 1.7 1996/10/12 23:23:16 christos Exp $	*/

/*
 * Written by Julian Elischer (julian@tfs.com)
 */
#ifndef	_SCSI_SCSI_DEBUG_H
#define _SCSI_SCSI_DEBUG_H
#ifdef _KERNEL

/*
 * These are the new debug bits.  (Sat Oct  2 12:46:46 WST 1993)
 * the following DEBUG bits are defined to exist in the flags word of
 * the scsi_link structure.
 */
#define	SDEV_DB1		0x0010	/* scsi commands, errors, data	*/
#define	SDEV_DB2		0x0020	/* routine flow tracking */
#define	SDEV_DB3		0x0040	/* internal to routine flows	*/
#define	SDEV_DB4		0x0080	/* level 4 debugging for this dev */

#ifdef	SCSIDEBUG
/* targets and LUNs we want to debug */
#ifndef SCSIDEBUG_BUSES
#define SCSIDEBUG_BUSES		0
#endif /* ~SCSIDBUG_BUSES */
#ifndef SCSIDEBUG_TARGETS
#define	SCSIDEBUG_TARGETS	0
#endif /* ~SCSIDEBUG_TARGETS */
#ifndef SCSIDEBUG_LUNS
#define	SCSIDEBUG_LUNS		0
#endif /* ~SCSIDEBUG_LUNS */
#ifndef SCSIDEBUG_LEVEL
#define	SCSIDEBUG_LEVEL		(SDEV_DB1|SDEV_DB2)
#endif /* ~SCSIDEBUG_LEVEL */

extern u_int32_t scsidebug_buses, scsidebug_targets, scsidebug_luns;
extern int scsidebug_level;

extern const char *flagnames[16];
extern const char *quirknames[16];
extern const char *devicetypenames[32];

struct scsi_xfer;

void	scsi_show_sense(struct scsi_xfer *);
void	scsi_show_xs(struct scsi_xfer *);
void	scsi_show_mem(u_char *, int);
void	scsi_show_flags(u_int16_t, const char **);

/*
 * This is the usual debug macro for use with the above bits
 */
#define	SC_DEBUG(link,Level,Printstuff) do {	\
	if ((link)->flags & (Level)) {		\
		sc_print_addr(link);		\
		printf Printstuff;		\
	}					\
} while (0)
#else
#define SC_DEBUG(A,B,C)
#endif /* SCSIDEBUG */

#endif /* _KERNEL */
#endif /* _SCSI_SCSI_DEBUG_H */
