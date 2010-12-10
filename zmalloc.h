/********************************************
zmalloc.h
copyright 2009,2010, Thomas E. Dickey
copyright 1991,1993, Michael D. Brennan

This is a source file for mawk, an implementation of
the AWK programming language.

Mawk is distributed without warranty under the terms of
the GNU General Public License, version 2, 1991.
********************************************/

/*
 * $MawkId: zmalloc.h,v 1.6 2010/07/24 14:32:48 tom Exp $
 * @Log: zmalloc.h,v @
 * Revision 1.2  1993/07/04  12:52:22  mike
 * start on autoconfig changes
 *
 * Revision 1.1.1.1  1993/07/03  18:58:23  mike
 * move source to cvs
 *
 * Revision 5.1  1991/12/05  07:59:41  brennan
 * 1.1 pre-release
 *
 */

/* zmalloc.h */

#ifndef  ZMALLOC_H
#define  ZMALLOC_H

#include "nstd.h"

extern PTR zmalloc(size_t);
extern void zfree(PTR, size_t);
extern PTR zrealloc(PTR, size_t, size_t);

#define ZMALLOC(type)  ((type*)zmalloc(sizeof(type)))
#define ZFREE(p)	zfree(p,sizeof(*(p)))

#endif /* ZMALLOC_H */
