/********************************************
fin.c
copyright 1991, 1992.  Michael D. Brennan

This is a source file for mawk, an implementation of
the AWK programming language.

Mawk is distributed without warranty under the terms of
the GNU General Public License, version 2, 1991.
********************************************/

/*
 * $MawkId: fin.c,v 1.31 2010/08/18 16:41:35 tom Exp $
 * @Log: fin.c,v @
 * Revision 1.10  1995/12/24  22:23:22  mike
 * remove errmsg() from inside FINopen
 *
 * Revision 1.9  1995/06/06  00:18:29  mike
 * change mawk_exit(1) to mawk_exit(2)
 *
 * Revision 1.8  1994/12/13  00:26:35  mike
 * rt_nr and rt_fnr for run-time error messages
 *
 * Revision 1.7  1994/12/11  23:25:05  mike
 * -Wi option
 *
 * Revision 1.6  1994/12/11  22:14:15  mike
 * remove THINK_C #defines.  Not a political statement, just no indication
 * that anyone ever used it.
 *
 * Revision 1.5  1994/10/08  19:15:42  mike
 * remove SM_DOS
 *
 * Revision 1.4  1993/07/17  13:22:55  mike
 * indent and general code cleanup
 *
 * Revision 1.3  1993/07/15  13:26:55  mike
 * SIZE_T and indent
 *
 * Revision 1.2	 1993/07/04  12:51:57  mike
 * start on autoconfig changes
 *
 * Revision 1.1.1.1  1993/07/03	 18:58:13  mike
 * move source to cvs
 *
 * Revision 5.7	 1993/01/01  21:30:48  mike
 * split new_STRING() into new_STRING and new_STRING0
 *
 * Revision 5.6	 1992/12/17  02:48:01  mike
 * 1.1.2d changes for DOS
 *
 * Revision 5.5	 1992/07/28  15:11:30  brennan
 * minor change in finding eol, needed for MsDOS
 *
 * Revision 5.4	 1992/07/10  16:17:10  brennan
 * MsDOS: remove NO_BINMODE macro
 *
 * Revision 5.3	 1992/07/08  16:14:27  brennan
 * FILENAME and FNR retain last values in the
 * END block.
 *
 * Revision 5.2	 1992/02/21  13:30:08  brennan
 * fixed bug that free'd FILENAME twice if
 * command line was var=value only
 *
 * Revision 5.1	 91/12/05  07:56:02  brennan
 * 1.1 pre-release
 *
*/

/* fin.c */

#include "mawk.h"
#include "fin.h"
#include "memory.h"
#include "bi_vars.h"
#include "field.h"
#include "symtype.h"
#include "scan.h"

#ifndef	  NO_FCNTL_H
#include <fcntl.h>
#endif

/* This file handles input files.  Opening, closing,
   buffering and (most important) splitting files into
   records, FINgets().
*/

static FIN *next_main(int);
static char *enlarge_fin_buffer(FIN *);
int is_cmdline_assign(char *);	/* also used by init */

/* this is how we mark EOF on main_fin  */
static char dead_buff = 0;
static FIN dead_main =
{0, (FILE *) 0, &dead_buff, &dead_buff, &dead_buff,
 1, EOF_FLAG};

static void
free_fin_data(FIN * fin)
{
    if (fin != &dead_main) {
	zfree(fin->buff, (size_t) (fin->nbuffs * BUFFSZ + 1));
	ZFREE(fin);
    }
}

/* convert file-descriptor to FIN*.
   It's the main stream if main_flag is set
*/
FIN *
FINdopen(int fd, int main_flag)
{
    FIN *fin = ZMALLOC(FIN);

    fin->fd = fd;
    fin->flags = main_flag ? (MAIN_FLAG | START_FLAG) : START_FLAG;
    fin->buffp = fin->buff = (char *) zmalloc((size_t) BUFFSZ + 1);
    fin->limit = fin->buffp;
    fin->nbuffs = 1;
    fin->buff[0] = 0;

    if ((isatty(fd) && rs_shadow.type == SEP_CHAR && rs_shadow.c == '\n')
	|| interactive_flag) {
	/* interactive, i.e., line buffer this file */
	if (fd == 0) {
	    fin->fp = stdin;
	} else if (!(fin->fp = fdopen(fd, "r"))) {
	    errmsg(errno, "fdopen failed");
	    free_fin_data(fin);
	    mawk_exit(2);
	}
    } else {
	fin->fp = (FILE *) 0;
    }

    return fin;
}

/* open a FIN* by filename.
   It's the main stream if main_flag is set.
   Recognizes "-" as stdin.
*/

FIN *
FINopen(char *filename, int main_flag)
{
    FIN *result = 0;
    int fd;
    int oflag = O_RDONLY;

#if USE_BINMODE
    int bm = binmode() & 1;
    if (bm)
	oflag |= O_BINARY;
#endif

    if (filename[0] == '-' && filename[1] == 0) {
#if USE_BINMODE
	if (bm)
	    setmode(0, O_BINARY);
#endif
	result = FINdopen(0, main_flag);
    } else if ((fd = open(filename, oflag, 0)) != -1) {
	result = FINdopen(fd, main_flag);
    }
    return result;
}

/* frees the buffer and fd, but leaves FIN structure until
   the user calls close() */

void
FINsemi_close(FIN * fin)
{
    static char dead = 0;

    if (fin->buff != &dead) {
	zfree(fin->buff, (size_t) (fin->nbuffs * BUFFSZ + 1));

	if (fin->fd) {
	    if (fin->fp)
		fclose(fin->fp);
	    else
		close(fin->fd);
	}

	fin->limit =
	    fin->buff =
	    fin->buffp = &dead;	/* marks it semi_closed */
    }
    /* else was already semi_closed */
}

/* user called close() on input file */
void
FINclose(FIN * fin)
{
    FINsemi_close(fin);
    ZFREE(fin);
}

/* return one input record as determined by RS,
   from input file (FIN)  fin
*/

char *
FINgets(FIN * fin, size_t *len_p)
{
    char *p;
    char *q = 0;
    size_t match_len;
    size_t r;

  restart:

    if ((p = fin->buffp) >= fin->limit) {	/* need a refill */
	if (fin->flags & EOF_FLAG) {
	    if (fin->flags & MAIN_FLAG) {
		fin = next_main(0);
		goto restart;
	    } else {
		*len_p = 0;
		return (char *) 0;
	    }
	}

	if (fin->fp) {
	    /* line buffering */
	    if (!fgets(fin->buff, BUFFSZ + 1, fin->fp)) {
		fin->flags |= EOF_FLAG;
		fin->buff[0] = 0;
		fin->buffp = fin->buff;
		fin->limit = fin->buffp;
		goto restart;	/* might be main_fin */
	    } else {		/* return this line */
		/* find eol */
		p = fin->buff;
		while (*p != '\n' && *p != 0)
		    p++;

		*p = 0;
		*len_p = (unsigned) (p - fin->buff);
		fin->buffp = p;
		fin->limit = fin->buffp + strlen(fin->buffp);
		return fin->buff;
	    }
	} else {
	    /* block buffering */
	    r = fillbuff(fin->fd, fin->buff, (size_t) (fin->nbuffs * BUFFSZ));
	    if (r == 0) {
		fin->flags |= EOF_FLAG;
		fin->buffp = fin->buff;
		fin->limit = fin->buffp;
		goto restart;	/* might be main */
	    } else if (r < fin->nbuffs * BUFFSZ) {
		fin->flags |= EOF_FLAG;
	    }

	    fin->limit = fin->buff + r;
	    p = fin->buffp = fin->buff;

	    if (fin->flags & START_FLAG) {
		fin->flags &= ~START_FLAG;
		if (rs_shadow.type == SEP_MLR) {
		    /* trim blank lines from front of file */
		    while (*p == '\n')
			p++;
		    fin->buffp = p;
		    if (p >= fin->limit)
			goto restart;
		}
	    }
	}
    }

  retry:

    switch (rs_shadow.type) {
    case SEP_CHAR:
	q = memchr(p, rs_shadow.c, (size_t) (fin->limit - p));
	match_len = 1;
	break;

    case SEP_STR:
	q = str_str(p,
		    (size_t) (fin->limit - p),
		    ((STRING *) rs_shadow.ptr)->str,
		    match_len = ((STRING *) rs_shadow.ptr)->len);
	break;

    case SEP_MLR:
    case SEP_RE:
	q = re_pos_match(p, (size_t) (fin->limit - p), rs_shadow.ptr, &match_len);
	/* if the match is at the end, there might still be
	   more to match in the file */
	if (q && q[match_len] == 0 && !(fin->flags & EOF_FLAG))
	    q = (char *) 0;
	break;

    default:
	bozo("type of rs_shadow");
    }

    if (q) {
	/* the easy and normal case */
	*q = 0;
	*len_p = (unsigned) (q - p);
	fin->buffp = q + match_len;
	return p;
    }

    if (fin->flags & EOF_FLAG) {
	/* last line without a record terminator */
	*len_p = r = (unsigned) (fin->limit - p);
	fin->buffp = p + r;

	if (rs_shadow.type == SEP_MLR && fin->buffp[-1] == '\n'
	    && r != 0) {
	    (*len_p)--;
	    *--fin->buffp = 0;
	    fin->limit--;
	}
	return p;
    }

    if (p == fin->buff) {
	/* current record is too big for the input buffer, grow buffer */
	p = enlarge_fin_buffer(fin);
    } else {
	/* move a partial line to front of buffer and try again */
	size_t rr;
	size_t amount = (size_t) (fin->limit - p);

	p = (char *) memmove(fin->buff, p, r = (size_t) (fin->limit - p));
	q = p + r;
	rr = fin->nbuffs * BUFFSZ - r;

	if ((r = fillbuff(fin->fd, q, rr)) < rr) {
	    fin->flags |= EOF_FLAG;
	    fin->limit = fin->buff + amount + r;
	}
    }
    goto retry;
}

static char *
enlarge_fin_buffer(FIN * fin)
{
    size_t r;
    size_t oldsize = fin->nbuffs * BUFFSZ + 1;
    size_t limit = (size_t) (fin->limit - fin->buff);

#ifdef  MSDOS
    /* I'm not sure this can really happen:
       avoid "16bit wrap" */
    if (fin->nbuffs == MAX_BUFFS) {
	errmsg(0, "out of input buffer space");
	mawk_exit(2);
    }
#endif

    fin->buffp =
	fin->buff = (char *) zrealloc(fin->buff, oldsize, oldsize + BUFFSZ);
    fin->nbuffs++;

    r = fillbuff(fin->fd, fin->buff + (oldsize - 1), (size_t) BUFFSZ);
    if (r < BUFFSZ)
	fin->flags |= EOF_FLAG;

    fin->limit = fin->buff + limit + r;
    return fin->buff;
}

/*--------
  target is big enough to hold size + 1 chars
  on exit the back of the target is zero terminated
 *--------------*/
size_t
fillbuff(int fd, char *target, size_t size)
{
    register int r;
    size_t entry_size = size;

    while (size)
	switch (r = (int) read(fd, target, size)) {
	case -1:
	    errmsg(errno, "read error");
	    mawk_exit(2);

	case 0:
	    goto out;

	default:
	    target += r;
	    size -= (unsigned) r;
	    break;
	}

  out:
    *target = 0;
    return (size_t) (entry_size - size);
}

/* main_fin is a handle to the main input stream
   == 0	 never been opened   */

FIN *main_fin;
ARRAY Argv;			/* to the user this is ARGV  */
static double argi = 1.0;	/* index of next ARGV[argi] to try to open */

static void
set_main_to_stdin(void)
{
    cell_destroy(FILENAME);
    FILENAME->type = C_STRING;
    FILENAME->ptr = (PTR) new_STRING("-");
    cell_destroy(FNR);
    FNR->type = C_DOUBLE;
    FNR->dval = 0.0;
    rt_fnr = 0;
    main_fin = FINdopen(0, 1);
}

/* this gets called once to get the input stream going.
   It is called after the execution of the BEGIN block
   unless there is a getline inside BEGIN {}
*/
void
open_main(void)
{
    CELL argc;

#if USE_BINMODE
    int k = binmode();

    if (k & 1)
	setmode(0, O_BINARY);
    if (k & 2) {
	setmode(1, O_BINARY);
	setmode(2, O_BINARY);
    }
#endif

    cellcpy(&argc, ARGC);
    if (argc.type != C_DOUBLE)
	cast1_to_d(&argc);

    if (argc.dval == 1.0)
	set_main_to_stdin();
    else
	next_main(1);
}

/* get the next command line file open */
static FIN *
next_main(int open_flag)	/* called by open_main() if on */
{
    register CELL *cp;
    CELL *cp0;
    CELL argc;			/* copy of ARGC */
    CELL c_argi;		/* cell copy of argi */
    CELL argval;		/* copy of ARGV[c_argi] */

    argval.type = C_NOINIT;
    c_argi.type = C_DOUBLE;

    if (main_fin) {
	FINclose(main_fin);
	main_fin = 0;
    }
    /* FILENAME and FNR don't change unless we really open
       a new file */

    /* make a copy of ARGC to avoid side effect */
    if (cellcpy(&argc, ARGC)->type != C_DOUBLE)
	cast1_to_d(&argc);

    while (argi < argc.dval) {
	c_argi.dval = argi;
	argi += 1.0;

	if (!(cp0 = array_find(Argv, &c_argi, NO_CREATE)))
	    continue;		/* its deleted */

	/* make a copy so we can cast w/o side effect */
	cell_destroy(&argval);
	cp = cellcpy(&argval, cp0);
#ifndef NO_LEAKS
	cell_destroy(cp0);
#endif

	if (cp->type < C_STRING)
	    cast1_to_s(cp);
	if (string(cp)->len == 0) {
	    /* file argument is "" */
	    cell_destroy(cp);
	    continue;
	}

	/* it might be a command line assignment */
	if (is_cmdline_assign(string(cp)->str)) {
	    continue;
	}

	/* try to open it -- we used to continue on failure,
	   but posix says we should quit */
	if (!(main_fin = FINopen(string(cp)->str, 1))) {
	    errmsg(errno, "cannot open %s", string(cp)->str);
	    mawk_exit(2);
	}

	/* success -- set FILENAME and FNR */
	cell_destroy(FILENAME);
	cellcpy(FILENAME, cp);
	cell_destroy(cp);
	cell_destroy(FNR);
	FNR->type = C_DOUBLE;
	FNR->dval = 0.0;
	rt_fnr = 0;

	return main_fin;
    }
    /* failure */
    cell_destroy(&argval);

    if (open_flag) {
	/* all arguments were null or assignment */
	set_main_to_stdin();
	return main_fin;
    }

    return main_fin = &dead_main;
    /* since MAIN_FLAG is not set, FINgets won't call next_main() */
}

int
is_cmdline_assign(char *s)
{
    register char *p;
    int c;
    SYMTAB *stp;
    CELL *cp = 0;
    size_t len;
    CELL cell;			/* used if command line assign to pseudo field */
    CELL *fp = (CELL *) 0;	/* ditto */
    size_t length;

    if (scan_code[*(unsigned char *) s] != SC_IDCHAR)
	return 0;

    p = s + 1;
    while ((c = scan_code[*(unsigned char *) p]) == SC_IDCHAR
	   || c == SC_DIGIT)
	p++;

    if (*p != '=')
	return 0;

    *p = 0;
    stp = find(s);

    switch (stp->type) {
    case ST_NONE:
	stp->type = ST_VAR;
	stp->stval.cp = cp = ZMALLOC(CELL);
	break;

    case ST_VAR:
    case ST_NR:		/* !! no one will do this */
	cp = stp->stval.cp;
	cell_destroy(cp);
	break;

    case ST_FIELD:
	/* must be pseudo field */
	fp = stp->stval.cp;
	cp = &cell;
	break;

    default:
	rt_error(
		    "cannot command line assign to %s\n\ttype clash or keyword"
		    ,s);
    }

    /* we need to keep ARGV[i] intact */
    *p++ = '=';
    len = strlen(p) + 1;
    /* posix says escape sequences are on from command line */
    p = rm_escape(strcpy((char *) zmalloc(len), p), &length);
    cp->ptr = (PTR) new_STRING1(p, length);
    zfree(p, len);
    check_strnum(cp);		/* sets cp->type */
    if (fp)			/* move it from cell to pfield[] */
    {
	field_assign(fp, cp);
	free_STRING(string(cp));
    }
    return 1;
}

#ifdef NO_LEAKS
void
fin_leaks(void)
{
    TRACE(("fin_leaks\n"));
    if (main_fin) {
	free_fin_data(main_fin);
	main_fin = 0;
    }
}
#endif
