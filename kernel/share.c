/* fdpp port by stsp. original copyrights below */

/*
	FreeDOS SHARE
	Copyright (c) 2000 Ronald B. Cemer under the GNU GPL
	You know the drill.
	If not, see www.gnu.org for details.  Read it, learn it, BE IT.  :-)
*/

/* #include <stdio.h> */ /* (fprintf removed...) */
/* #include <fcntl.h> */ /* Not used, using defines below... */
//#include <io.h>		/* write (what else?) */
//#include <stdlib.h>	/* _psp, NULL, malloc, free, atol, atoi */
//#include <dos.h>	/* MK_FP, FP_OFF, FP_SEG, int86, intdosx, */
			/* freemem, keep */
#include <string.h>	/* strchr, strlen, memset */

#include "portab.h"
#include "globals.h"
#include "proto.h"
#include "init-mod.h"
#define write(x,y,z) DosWrite(x,z,y)
#define interrupt

/* Changed by Eric Auer 5/2004: Squeezing executable size a bit -> */
/* Replaced fprint(stderr or stdout,...) by write(hand, buf, size) */
/* Keeps stream stuff and printf stuff outside the file and TSR... */

		/* ------------- DEFINES ------------- */
#define MUX_INT_NO 0x2f
#define MULTIPLEX_ID 0x10

#define FILE_TABLE_MIN 128
#define FILE_TABLE_MAX 62000U

#define LOCK_TABLE_MIN 1
#define LOCK_TABLE_MAX 3800

	/* Valid values for openmode: */
#define OPEN_READ_ONLY   0
#define OPEN_WRITE_ONLY  1
#define OPEN_READ_WRITE  2

	/* Valid values for sharemode: */
#define SHARE_COMPAT     0
#define SHARE_DENY_ALL   1
#define SHARE_DENY_WRITE 2
#define SHARE_DENY_READ  3
#define SHARE_DENY_NONE  4

		/* ------------- TYPEDEFS ------------- */
	/* This table determines the action to take when attempting to open
	   a file.  The first array index is the sharing mode of a previous
	   open on the same file.  The second array index is the sharing mode
	   of the current open attempt on the same file.  Action codes are
	   defined as follows:
		   0 = open may proceed
		   1 = open fails with error code 05h
		   2 = open fails and an INT 24h is generated
		   3 = open proceeds if the file is read-only; otherwise fails
			   with error code (used only in exception table below)
		   4 = open proceeds if the file is read-only; otherwise fails
			   with INT 24H (used only in exception table below)
	   Exceptions to the rules are handled in the table
	   below, so this table only covers the general rules.
	*/
static unsigned char open_actions[5][5] = {
	{ 0, 1, 1, 1, 1 },
	{ 2, 1, 1, 1, 1 },
	{ 2, 1, 1, 1, 1 },
	{ 2, 1, 1, 1, 1 },
	{ 2, 1, 1, 1, 0 },
};

typedef struct {
	unsigned char first_sharemode;
	unsigned char first_openmode;
	unsigned char current_sharemode;
	unsigned char current_openmode;
	unsigned char action;
} open_action_exception_t;

static open_action_exception_t open_exceptions[] = {
	{ 0, 0, 2, 0, 3 },
	{ 0, 0, 4, 0, 3 },	/* compatibility-read/deny none-read, MED 08/2004 */
	{ 2, 0, 0, 0, 4 },
	{ 2, 0, 2, 0, 0 },
	{ 2, 0, 4, 0, 0 },
	{ 3, 0, 2, 1, 0 },
	{ 3, 0, 4, 1, 0 },
	{ 3, 1, 4, 1, 0 },
	{ 3, 2, 4, 1, 0 },
	{ 4, 0, 0, 0, 4 },
	{ 4, 0, 0, 1, 0 },	/* deny none-read/compatibility-write */
	{ 4, 0, 0, 2, 0 },	/* deny none-read/compatibility-read+write */
	{ 4, 0, 2, 0, 0 },
	{ 4, 0, 2, 1, 0 },
	{ 4, 0, 2, 2, 0 },
	{ 4, 1, 3, 0, 0 },
	{ 4, 1, 3, 1, 0 },
	{ 4, 1, 3, 2, 0 },
};

	/* One of these exists for each instance of an open file. */
typedef struct {
	char filename[128];		/* fully-qualified filename; "\0" if unused */
	unsigned short psp;		/* PSP of process which opened this file */
	unsigned char openmode;	/* 0=read-only, 1=write-only, 2=read-write */
	unsigned char sharemode;/* SHARE_COMPAT, etc... */
	unsigned char first_openmode;	/* openmode of first open */
	unsigned char first_sharemode;	/* sharemode of first open */
} file_t;

	/* One of these exists for each active lock region. */
typedef struct {
	unsigned char used;		/* Non-zero if this entry is used. */
	unsigned long start;	/* Beginning offset of locked region */
	unsigned long end;		/* Ending offset of locked region */
	unsigned short fileno;	/* file_table entry number */
	unsigned short psp;		/* PSP of process which owns the lock */
} lock_t;


		/* ------------- GLOBALS ------------- */
//static char progname[9];
static unsigned int file_table_size_bytes = 2048;
static unsigned int file_table_size = 0;	/* # of file_t we can have */
static file_t *file_table = NULL;
static unsigned int lock_table_size = 20;	/* # of lock_t we can have */
static lock_t *lock_table = NULL;


		/* ------------- PROTOTYPES ------------- */
	/* DOS calls this to see if it's okay to open the file.
	   Returns a file_table entry number to use (>= 0) if okay
	   to open.  Otherwise returns < 0 and may generate a critical
	   error.  If < 0 is returned, it is the negated error return
	   code, so DOS simply negates this value and returns it in
	   AX. */
static int open_check
	(char FAR  *filename,/* FAR  pointer to fully qualified filename */
	 unsigned short psp,/* psp segment address of owner process */
	 int openmode,		/* 0=read-only, 1=write-only, 2=read-write */
	 int sharemode);	/* SHARE_COMPAT, etc... */

	/* DOS calls this to record the fact that it has successfully
	   closed a file, or the fact that the open for this file failed. */
static void close_file
	(int fileno);		/* file_table entry number */

	/* DOS calls this to determine whether it can access (read or
	   write) a specific section of a file.  We call it internally
	   from lock_unlock (only when locking) to see if any portion
	   of the requested region is already locked.  If psp is zero,
	   then it matches any psp in the lock table.  Otherwise, only
	   locks which DO NOT belong to psp will be considered.
	   Returns zero if okay to access or lock (no portion of the
	   region is already locked).  Otherwise returns non-zero and
	   generates a critical error (if allowcriter is non-zero).
	   If non-zero is returned, it is the negated return value for
	   the DOS call. */
static int access_check
	(unsigned short psp,/* psp segment address of owner process */
	 int fileno,		/* file_table entry number */
	 unsigned long ofs,	/* offset into file */
	 unsigned long len,	/* length (in bytes) of region to access */
	 int allowcriter);	/* allow a critical error to be generated */

	/* DOS calls this to lock or unlock a specific section of a file.
	   Returns zero if successfully locked or unlocked.  Otherwise
	   returns non-zero.
	   If the return value is non-zero, it is the negated error
	   return code for the DOS 0x5c call. */
static int lock_unlock
	(unsigned short psp,/* psp segment address of owner process */
	 int fileno,		/* file_table entry number */
	 unsigned long ofs,	/* offset into file */
	 unsigned long len,	/* length (in bytes) of region to lock or unlock */
	 int unlock);		/* non-zero to unlock; zero to lock */

	/* Multiplex interrupt handler */

		/* ------------- HOOK ------------- */
static void interrupt FAR handler2f(iregs FAR *iregs_p)
{
#define iregs (*iregs_p)
#define ax a.x
#define bx b.x
#define cx c.x
#define dx d.x

	iregs.flags &= ~FLG_CARRY;
	if (((iregs.ax >> 8) & 0xff) == MULTIPLEX_ID) {
		if ((iregs.ax & 0xff) == 0) {
				/* Installation check.  Return 0xff in AL. */
			iregs.ax |= 0xff;
			return;
		}
			/* These subfuctions are nonstandard, but are highly
			   unlikely to be used by another multiplex TSR, since
			   our multiplex Id (0x10) is basically reserved for
			   SHARE.  So we should be able to get away with using
			   these for our own purposes. */
			/* open_check */
		if ((iregs.ax & 0xff) == 0xa0) {
			iregs.ax = open_check
				(MK_FP(iregs.ds, iregs.si),
				 iregs.bx,
				 iregs.cx,
				 iregs.dx);
			return;
		}
			/* close_file */
		if ((iregs.ax & 0xff) == 0xa1) {
			close_file(iregs.bx);
			return;
		}
			/* access_check (0xa2) */
			/* access_check with critical error (0xa3) */
		if ((iregs.ax & 0xfe) == 0xa2) {
			iregs.ax = access_check
				(iregs.bx,
                 iregs.cx,
#if 0
				 (   ((((unsigned long)iregs.si)<<16) & 0xffff0000L) |
					 (((unsigned long)iregs.di) & 0xffffL)   ),
				 (   ((((unsigned long)iregs.es)<<16) & 0xffff0000L) |
                     (((unsigned long)iregs.dx) & 0xffffL)   ),
#else
				 (   (((unsigned long)iregs.si)<<16) + iregs.di   ),
				 (   (((unsigned long)iregs.es)<<16) + iregs.dx   ),
#endif
				 (iregs.ax & 0x01));
			return;
		}
			/* lock_unlock lock (0xa4)*/
			/* lock_unlock unlock (0xa5) */
		if ((iregs.ax & 0xfe) == 0xa4) {
			iregs.ax = lock_unlock
				(iregs.bx,
                 iregs.cx,
#if 0
				 (   ((((unsigned long)iregs.si)<<16) & 0xffff0000L) |
  				 (((unsigned long)iregs.di) & 0xffffL)   ),
				 (   ((((unsigned long)iregs.es)<<16) & 0xffff0000L) |
                     (((unsigned long)iregs.dx) & 0xffffL)   ),
#else
				 (   (((unsigned long)iregs.si)<<16) | ((unsigned long)iregs.di)   ),
				 (   (((unsigned long)iregs.es)<<16) | ((unsigned long)iregs.dx)   ),
#endif
				 (iregs.ax & 0x01));
			return;
		}
	}
	iregs.flags |= FLG_CARRY;

#undef iregs
#undef ax
#undef bx
#undef cx
#undef dx
}

void int2F_10_handler(iregs FAR *iregs_p)
{
	if (!file_table_size)
		return;
	handler2f(iregs_p);
}

static void remove_all_locks(int fileno) {
	int i;
	lock_t *lptr;

	for (i = 0; i < lock_table_size; i++) {
		lptr = &lock_table[i];
		if (lptr->fileno == fileno) lptr->used = 0;
	}
}

static void free_file_table_entry(int fileno) {
	file_table[fileno].filename[0] = '\0';
}

/* DOS 7 does not have read-only restrictions, MED 08/2004 */
/*
static int file_is_read_only(char FAR  *filename) {
	union REGS regs;
	struct SREGS sregs;

	regs.x.ax = 0x4300;
	sregs.ds = FP_SEG(filename);
	regs.x.dx = FP_OFF(filename);
	intdosx(&regs, &regs, &sregs);
	if (regs.x.cflag) return 0;
	return ((regs.h.cl & 0x19) == 0x01);
}
*/

static int fnmatches(char *fn1, char *fn2) {
	while (*fn1) {
		if (*fn1 != *fn2) return 0;
		fn1++;
		fn2++;
	}
	return (*fn1 == *fn2);
}

static int do_open_check
	(int fileno) {		/* file_table entry number */
	file_t *p, *fptr = &file_table[fileno];
	int i, j, action = 0, foundexc;
	unsigned char current_sharemode = fptr->sharemode;
	unsigned char current_openmode = fptr->openmode;
	open_action_exception_t *excptr;

	fptr->first_sharemode = fptr->sharemode;
	fptr->first_openmode = fptr->openmode;
	for (i = 0; i < file_table_size; i++) {
		if (i == fileno) continue;
		p = &file_table[i];
		if (p->filename[0] == '\0') continue;
		if (!fnmatches(p->filename, fptr->filename)) continue;
		fptr->first_sharemode = p->first_sharemode;
		fptr->first_openmode = p->first_openmode;
			/* Look for exceptions to the general rules first. */
		foundexc = 0;
		for (j = 0;
			 j < (sizeof(open_exceptions)/sizeof(open_action_exception_t));
			 j++) {
			excptr = &open_exceptions[j];
			if (   (excptr->first_sharemode == fptr->first_sharemode)
				&& (excptr->current_sharemode == current_sharemode)
				&& (excptr->first_openmode == fptr->first_openmode)
				&& (excptr->current_openmode == current_openmode)  ) {
				foundexc = 1;
				action = excptr->action;
				break;
			}
		}
			/* If no exception to rules, use normal rules. */
		if (!foundexc)
			action = open_actions[fptr->first_sharemode][current_sharemode];
			/* Fail appropriately based on action. */
		switch (action) {

/* DOS 7 does not have read-only restrictions, fall through to proceed, MED 08/2004 */
		case 3:
		case 4:

		case 0:		/* proceed with open */
			break;
/*		case 3:	*/		/* succeed if file read-only, else fail with error 05h */
/*			if (file_is_read_only(fptr->filename)) break;	*/
		case 1:		/* fail with error code 05h */
			free_file_table_entry(fileno);
			return -5;
/*		case 4:	*/		/* succeed if file read-only, else fail with int 24h */
/*			if (file_is_read_only(fptr->filename)) break;	*/
		case 2:		/* fail with int 24h */
			{
				iregs regs;

				regs.a.b.h = 0x0e;	/* disk I/O; fail allowed; data area */
				regs.a.b.l = 0;
				regs.di = 0x0d;	/* sharing violation */
				if ( (fptr->filename[0]!='\0') && (fptr->filename[1]==':') )
					regs.a.b.l = fptr->filename[0]-'A';
				free_file_table_entry(fileno);
				call_intr(0x24, MK_FAR_SCP(regs));
			}
			return -0x20;			/* sharing violation */
		}
		break;
	}
	return fileno;
}

	/* DOS calls this to see if it's okay to open the file.
	   Returns a file_table entry number to use (>= 0) if okay
	   to open.  Otherwise returns < 0 and may generate a critical
	   error.  If < 0 is returned, it is the negated error return
	   code, so DOS simply negates this value and returns it in
	   AX. */
static int open_check
	(char FAR  *filename,/* FAR  pointer to fully qualified filename */
	 unsigned short psp,/* psp segment address of owner process */
	 int openmode,		/* 0=read-only, 1=write-only, 2=read-write */
	 int sharemode) {	/* SHARE_COMPAT, etc... */

	int i, fileno = -1;
	file_t *fptr;

		/* Whack off unused bits in the share mode
		   in case we were careless elsewhere. */
	sharemode &= 0x07;

		/* Assume compatibility mode if invalid share mode. */
/* ??? IS THIS CORRECT ??? */
	if ( (sharemode < SHARE_COMPAT) || (sharemode > SHARE_DENY_NONE) )
		sharemode = SHARE_COMPAT;

		/* Whack off unused bits in the open mode
		   in case we were careless elsewhere. */
	openmode &= 0x03;

		/* Assume read-only mode if invalid open mode. */
/* ??? IS THIS CORRECT ??? */
	if ( (openmode < OPEN_READ_ONLY) || (openmode > OPEN_READ_WRITE) )
		openmode = OPEN_READ_ONLY;

	for (i = 0; i < file_table_size; i++) {
		if (file_table[i].filename[0] == '\0') {
			fileno = i;
			break;
		}
	}
	if (fileno == -1) return -1;
	fptr = &file_table[fileno];

		/* Copy the filename into ftpr->filename. */
	for (i = 0; i < sizeof(fptr->filename); i++) {
		if ((fptr->filename[i] = filename[i]) == '\0') break;
	}
	fptr->psp = psp;
	fptr->openmode = (unsigned char)openmode;
	fptr->sharemode = (unsigned char)sharemode;
		/* Do the sharing check and return fileno if
		   okay, or < 0 (and free the entry) if error. */
	return do_open_check(fileno);
}

	/* DOS calls this to record the fact that it has successfully
	   closed a file, or the fact that the open for this file failed. */
static void close_file
	(int fileno) {		/* file_table entry number */

	remove_all_locks(fileno);
	free_file_table_entry(fileno);
}

	/* DOS calls this to determine whether it can access (read or
	   write) a specific section of a file.  We call it internally
	   from lock_unlock (only when locking) to see if any portion
	   of the requested region is already locked.  If psp is zero,
	   then it matches any psp in the lock table.  Otherwise, only
	   locks which DO NOT belong to psp will be considered.
	   Returns zero if okay to access or lock (no portion of the
	   region is already locked).  Otherwise returns non-zero and
	   generates a critical error (if allowcriter is non-zero).
	   If non-zero is returned, it is the negated return value for
	   the DOS call. */
static int access_check
	(unsigned short psp,/* psp segment address of owner process */
	 int fileno,		/* file_table entry number */
	 unsigned long ofs,	/* offset into file */
	 unsigned long len,	/* length (in bytes) of region to access */
	 int allowcriter) {	/* allow a critical error to be generated */
	int i;
	file_t *fptr = &file_table[fileno];
	char *filename = fptr->filename;
	lock_t *lptr;
	unsigned long endofs = ofs + len;

	if (endofs < ofs) {
		endofs = 0xffffffffL;
		len = endofs-ofs;
	}

	if (len < 1L) return 0;

	for (i = 0; i < lock_table_size; i++) {
		lptr = &lock_table[i];
		if (   (lptr->used)
			&& ( (psp == 0) || (lptr->psp != psp) )
			&& (fnmatches(filename, file_table[lptr->fileno].filename))
			&& (   ( (ofs>=lptr->start) && (ofs<lptr->end) )
				|| ( (endofs>lptr->start) && (endofs<=lptr->end) )   )   ) {
			if (allowcriter) {
				iregs regs;

				regs.a.b.h = 0x0e;	/* disk I/O; fail allowed; data area */
				regs.a.b.l = 0;
				regs.di = 0x0e;	/* lock violation */
				if ( (fptr->filename[0]!='\0') && (fptr->filename[1]==':') )
					regs.a.b.l = fptr->filename[0]-'A';
				free_file_table_entry(fileno);
				call_intr(0x24, MK_FAR_SCP(regs));
			}
			return -0x21;			/* lock violation */
		}
	}
	return 0;
}

	/* DOS calls this to lock or unlock a specific section of a file.
	   Returns zero if successfully locked or unlocked.  Otherwise
	   returns non-zero.
	   If the return value is non-zero, it is the negated error
	   return code for the DOS 0x5c call. */
static int lock_unlock
	(unsigned short psp,/* psp segment address of owner process */
	 int fileno,		/* file_table entry number */
	 unsigned long ofs,	/* offset into file */
	 unsigned long len,	/* length (in bytes) of region to lock or unlock */
	 int unlock) {		/* non-zero to unlock; zero to lock */

	int i;
	lock_t *lptr;
	unsigned long endofs = ofs + len;

    if (endofs < ofs) {
		endofs = 0xffffffffL;
		len = endofs-ofs;
	}

	if (len < 1L) return 0;

    /* there was a error in the code below preventing any other
     than the first locked region to be unlocked (japheth, 09/2005) */

	if (unlock) {
		for (i = 0; i < lock_table_size; i++) {
			lptr = &lock_table[i];
			if (   (lptr->used)
				&& (lptr->psp == psp)
				&& (lptr->fileno == fileno)
				&& (lptr->start == ofs)
				&& (lptr->end == endofs)   ) {
				lptr->used = 0;
				return 0;
			}
		}
			/* Not already locked by us; can't unlock. */
		return -(0x21);		/* lock violation */
	} else {
		if (access_check(0, fileno, ofs, len, 0)) {
				/* Already locked; can't lock. */
			return -(0x21);		/* lock violation */
		}
		for (i = 0; i < lock_table_size; i++) {
			lptr = &lock_table[i];
			if (!lptr->used) {
				lptr->used = 1;
				lptr->start = ofs;
				lptr->end = ofs+(unsigned long)len;
				lptr->fileno = fileno;
				lptr->psp = psp;
				return 0;
			}
		}
		return -(0x24);		/* sharing buffer overflow */
	}
}

static void FAR *high_malloc(uint32_t size, int high)
{
	return KernelAlloc(size, 'F', high);
}

		/* ------------- INIT ------------- */
	/* Allocate tables and install hooks into the kernel.
	   If we run out of memory, return non-zero. */
int share_init(int high)
{
	/* int i; */
#define malloc(x) high_malloc(x, high)
	file_table_size = file_table_size_bytes/sizeof(file_t);
	if ((file_table=(file_t *)malloc(file_table_size_bytes)) == NULL)
		return 1;
	memset(file_table, 0, file_table_size_bytes);
	if ((lock_table=(lock_t *)malloc(lock_table_size*sizeof(lock_t))) == NULL)
		return 1;
	memset(lock_table, 0, lock_table_size*sizeof(lock_t));
	return 0;
}
