/********************************************
files.c
copyright 2008-2010,2012.  Thomas E. Dickey
copyright 1991-1994,1996.  Michael D. Brennan

This is a source file for mawk, an implementation of
the AWK programming language.

Mawk is distributed without warranty under the terms of
the GNU General Public License, version 2, 1991.
********************************************/

/*
 * $MawkId: files.c,v 1.20 2010/12/10 17:00:00 tom Exp $
 *
 * @Log: files.c,v @
 * Revision 1.9  1996/01/14  17:14:10  mike
 * flush_all_output()
 *
 * Revision 1.8  1995/06/06  00:18:27  mike
 * change mawk_exit(1) to mawk_exit(2)
 *
 * Revision 1.7  1994/12/11  20:48:50  mike
 * fflush builtin
 *
 * Revision 1.6  1994/10/08  19:15:40  mike
 * remove SM_DOS
 *
 * Revision 1.5  1994/04/17  20:01:37  mike
 * recognize filename "/dev/stdout"
 *
 * Revision 1.4  1994/02/21  00:11:07  mike
 * code cleanup
 *
 * Revision 1.3  1993/07/16  01:00:36  mike
 * cleanup and indent
 *
 * Revision 5.5	 1992/12/17  02:48:01  mike
 * 1.1.2d changes for DOS
 *
 * Revision 5.4	 1992/07/10  16:10:30  brennan
 * patch2
 * MsDOS: remove useless NO_BINMODE macro
 * get process exit code on in pipes
 *
 * Revision 5.3	 1992/04/07  20:21:17  brennan
 * patch 2
 * unbuffered output to a tty
 *
 * Revision 5.2	 1992/04/07  16:03:08  brennan
 * patch 2
 * allow same filename for output and input, but use different descriptors
 * E.g. < "/dev/tty" and > "/dev/tty"
 *
 * Revision 5.1	 91/12/05  07:56:00  brennan
 * 1.1 pre-release
 */

/* files.c */

#include "mawk.h"
#include "files.h"
#include "memory.h"
#include "fin.h"
#include "init.h"

#include <sys/types.h>

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef	V7
#include  <sgtty.h>		/* defines FIOCLEX */
#endif

#ifndef	 NO_FCNTL_H

#include <fcntl.h>
#define	 CLOSE_ON_EXEC(fd)    fcntl(fd, F_SETFD, 1)

#else
#define	 CLOSE_ON_EXEC(fd) ioctl(fd, FIOCLEX, (PTR) 0)
#endif

/* We store dynamically created files on a linked linear
   list with move to the front (big surprise)  */

typedef struct file {
    struct file *link;
    STRING *name;
    short type;
    int pid;			/* we need to wait() when we close a pipe */
    /* holds temp file index under MSDOS */

#ifdef  HAVE_FAKE_PIPES
    int inpipe_exit;
#endif

    PTR ptr;			/* FIN*   or  FILE*   */
} FILE_NODE;

static FILE_NODE *file_list;

/* Prototypes for local functions */

static FILE *tfopen(const char *, const char *);
static void efflush(FILE *);
static void close_error(FILE_NODE * p);

static FILE_NODE *
alloc_filenode(void)
{
    FILE_NODE *result;

    result = ZMALLOC(FILE_NODE);

#ifdef NO_LEAKS
    result->name = 0;
#endif

    return result;
}

static void
free_filenode(FILE_NODE * p)
{
#ifdef NO_LEAKS
    if (p->name != 0) {
	free_STRING(p->name);
    }
#endif
    zfree(p, sizeof(FILE_NODE));
}

/* find a file on file_list */
PTR
file_find(STRING * sval, int type)
{
    FILE_NODE *p = file_list;
    FILE_NODE *q = (FILE_NODE *) 0;
    char *name = sval->str;
    const char *ostr;

    while (1) {
	if (!p) {
	    /* open a new one */
	    p = alloc_filenode();

	    switch (p->type = (short) type) {
	    case F_TRUNC:
#if USE_BINMODE
		ostr = (binmode() & 2) ? "wb" : "w";
#else
		ostr = "w";
#endif
		if (!(p->ptr = (PTR) tfopen(name, ostr)))
		    goto out_failure;
		break;

	    case F_APPEND:
#if USE_BINMODE
		ostr = (binmode() & 2) ? "ab" : "a";
#else
		ostr = "a";
#endif
		if (!(p->ptr = (PTR) tfopen(name, ostr)))
		    goto out_failure;
		break;

	    case F_IN:
		if (!(p->ptr = (PTR) FINopen(name, 0))) {
		    free_filenode(p);
		    return (PTR) 0;
		}
		break;

	    case PIPE_OUT:
	    case PIPE_IN:

#if    defined(HAVE_REAL_PIPES) || defined(HAVE_FAKE_PIPES)

		if (!(p->ptr = get_pipe(name, type, &p->pid))) {
		    if (type == PIPE_OUT)
			goto out_failure;
		    else {
			free_filenode(p);
			return (PTR) 0;
		    }
		}
#else
		rt_error("pipes not supported");
#endif
		break;

#ifdef	DEBUG
	    default:
		bozo("bad file type");
#endif
	    }
	    /* successful open */
	    p->name = sval;
	    sval->ref_cnt++;
	    break;		/* while loop */
	}

	/* search is by name and type */
	if (strcmp(name, p->name->str) == 0 &&
	    (p->type == type ||
	/* no distinction between F_APPEND and F_TRUNC here */
	     (p->type >= F_APPEND && type >= F_APPEND))) {
	    /* found */
	    if (!q)		/*at front of list */
		return p->ptr;
	    /* delete from list for move to front */
	    q->link = p->link;
	    break;		/* while loop */
	}

	q = p;
	p = p->link;
    }				/* end while loop */

    /* put p at the front of the list */
    p->link = file_list;
    return (PTR) (file_list = p)->ptr;

  out_failure:
    errmsg(errno, "cannot open \"%s\" for output", name);
    mawk_exit(2);
    /* NOTREACHED */
}

/* Close a file and delete its node from the file_list.
   Walk the whole list, in case a name has two nodes,
   e.g. < "/dev/tty" and > "/dev/tty"
*/

int
file_close(STRING * sval)
{
    FILE_NODE dummy;
    FILE_NODE *p;
    FILE_NODE *q = &dummy;	/* trails p */
    FILE_NODE *hold;
    char *name = sval->str;
    int retval = -1;

    dummy.link = p = file_list;
    while (p) {
	if (strcmp(name, p->name->str) == 0) {
	    /* found */

	    /* Remove it from the list first because we might be called
	       again if an error occurs leading to an infinite loop.

	       Note that we don't have to consider the list corruption
	       caused by a recursive call because it will never return. */

	    q->link = p->link;
	    file_list = dummy.link;	/* maybe it was the first file */

	    switch (p->type) {
	    case F_TRUNC:
	    case F_APPEND:
		if (fclose((FILE *) p->ptr) != 0) {
		    close_error(p);
		}
		retval = 0;
		break;

	    case PIPE_OUT:
		if (fclose((FILE *) p->ptr) != 0) {
		    close_error(p);
		}
#ifdef  HAVE_REAL_PIPES
		retval = wait_for(p->pid);
#endif
#ifdef  HAVE_FAKE_PIPES
		retval = close_fake_outpipe(p->name->str, p->pid);
#endif
		break;

	    case F_IN:
		FINclose((FIN *) p->ptr);
		retval = 0;
		break;

	    case PIPE_IN:
		FINclose((FIN *) p->ptr);

#ifdef  HAVE_REAL_PIPES
		retval = wait_for(p->pid);
#endif
#ifdef  HAVE_FAKE_PIPES
		{
		    char xbuff[100];
		    unlink(tmp_file_name(p->pid, xbuff));
		    retval = p->inpipe_exit;
		}
#endif
		break;
	    }

	    hold = p;
	    p = p->link;
	    free_filenode(hold);
	} else {
	    q = p;
	    p = p->link;
	}
    }

    return retval;
}

/*
find an output file with name == sval and fflush it
*/

int
file_flush(STRING * sval)
{
    int ret = -1;
    FILE_NODE *p = file_list;
    size_t len = sval->len;
    char *str = sval->str;

    if (len == 0) {
	/* for consistency with gawk */
	flush_all_output();
	return 0;
    }

    while (p) {
	if (IS_OUTPUT(p->type) &&
	    len == p->name->len &&
	    strcmp(str, p->name->str) == 0) {
	    ret = 0;
	    efflush((FILE *) p->ptr);
	    /* it's possible for a command and a file to have the same
	       name -- so keep looking */
	}
	p = p->link;
    }
    return ret;
}

void
flush_all_output(void)
{
    FILE_NODE *p;

    for (p = file_list; p; p = p->link)
	if (IS_OUTPUT(p->type))
	    efflush((FILE *) p->ptr);
}

static void
efflush(FILE *fp)
{
    if (fflush(fp) < 0) {
	errmsg(errno, "unexpected write error");
	mawk_exit(2);
    }
}

/* When we exit, we need to close and wait for all output pipes */

#ifdef   HAVE_REAL_PIPES

/* work around for bug in AIX 4.1 -- If there are exactly 16 or
   32 or 48 ..., open files then the last one doesn't get flushed on
   exit.  So the following is now a misnomer as we'll really close
   all output.
*/

void
close_out_pipes(void)
{
    FILE_NODE *p = file_list;
    FILE_NODE *q = 0;

    while (p) {

	if (IS_OUTPUT(p->type)) {
	    if (fclose((FILE *) p->ptr) != 0) {
		/* if another error occurs we do not want to be called
		   for the same file again */

		if (q != 0)
		    q->link = p->link;
		else
		    file_list = p->link;
		close_error(p);
	    } else if (p->type == PIPE_OUT) {
		wait_for(p->pid);
	    }
	}

	q = p;
	p = p->link;
    }
}

#else
#ifdef  HAVE_FAKE_PIPES		/* pipes are faked with temp files */

void
close_fake_pipes(void)
{
    FILE_NODE *p = file_list;
    char xbuff[100];

    /* close input pipes first to free descriptors for children */
    while (p) {
	if (p->type == PIPE_IN) {
	    FINclose((FIN *) p->ptr);
	    unlink(tmp_file_name(p->pid, xbuff));
	}
	p = p->link;
    }
    /* doit again */
    p = file_list;
    while (p) {
	if (p->type == PIPE_OUT) {
	    if (fclose(p->ptr) != 0) {
		close_error(p);
	    }
	    close_fake_outpipe(p->name->str, p->pid);
	}
	p = p->link;
    }
}
#endif /* HAVE_FAKE_PIPES */
#endif /* ! HAVE_REAL_PIPES */

/* hardwire to /bin/sh for portability of programs */
const char *shell = "/bin/sh";

#ifdef  HAVE_REAL_PIPES

PTR
get_pipe(char *name, int type, int *pid_ptr)
{
    int the_pipe[2], local_fd, remote_fd;

    if (pipe(the_pipe) == -1)
	return (PTR) 0;
    local_fd = the_pipe[type == PIPE_OUT];
    remote_fd = the_pipe[type == PIPE_IN];
    /* to keep output ordered correctly */
    fflush(stdout);
    fflush(stderr);

    switch (*pid_ptr = fork()) {
    case -1:
	close(local_fd);
	close(remote_fd);
	return (PTR) 0;

    case 0:
	close(local_fd);
	close(type == PIPE_IN);
	dup(remote_fd);
	close(remote_fd);
	execl(shell, shell, "-c", name, (char *) 0);
	errmsg(errno, "failed to exec %s -c %s", shell, name);
	fflush(stderr);
	_exit(128);

    default:
	close(remote_fd);
	/* we could deadlock if future child inherit the local fd ,
	   set close on exec flag */
	CLOSE_ON_EXEC(local_fd);
	break;
    }

    return ((type == PIPE_IN)
	    ? (PTR) FINdopen(local_fd, 0)
	    : (PTR) fdopen(local_fd, "w"));
}

/*------------ children ------------------*/

/* we need to wait for children at the end of output pipes to
   complete so we know any files they have created are complete */

/* dead children are kept on this list */

static struct child {
    int pid;
    int exit_status;
    struct child *link;
} *child_list;

static void
add_to_child_list(int pid, int exit_status)
{
    struct child *p = ZMALLOC(struct child);

    p->pid = pid;
    p->exit_status = exit_status;
    p->link = child_list;
    child_list = p;
}

static struct child *
remove_from_child_list(int pid)
{
    struct child dummy;
    struct child *p;
    struct child *q = &dummy;

    dummy.link = p = child_list;
    while (p) {
	if (p->pid == pid) {
	    q->link = p->link;
	    break;
	} else {
	    q = p;
	    p = p->link;
	}
    }

    child_list = dummy.link;
    return p;
    /* null return if not in the list */
}

/* wait for a specific child to complete and return its
   exit status

   If pid is zero, wait for any single child and
   put it on the dead children list
*/

int
wait_for(int pid)
{
    int exit_status = 0;
    struct child *p;
    int id;

    if (pid == 0) {
	id = wait(&exit_status);
	add_to_child_list(id, exit_status);
    }
    /* see if an earlier wait() caught our child */
    else if ((p = remove_from_child_list(pid))) {
	exit_status = p->exit_status;
	ZFREE(p);
    } else {
	/* need to really wait */
	while ((id = wait(&exit_status)) != pid) {
	    if (id == -1)	/* can't happen */
		bozo("wait_for");
	    else {
		/* we got the exit status of another child
		   put it on the child list and try again */
		add_to_child_list(id, exit_status);
	    }
	}
    }

    if (exit_status & 0xff)
	exit_status = 128 + (exit_status & 0xff);
    else
	exit_status = (exit_status & 0xff00) >> 8;

    return exit_status;
}

#endif /* HAVE_REAL_PIPES */

/*
 * Create names by which the standard streams can be referenced in a script.
 */
void
set_stdio(void)
{
    FILE_NODE *p, *q, *r;

    /* We insert stderr first to get it at the end of the list. This is
       needed because we want to output errors encountered on closing
       stdout. */

    q = alloc_filenode();
    q->link = (FILE_NODE *) 0;
    q->type = F_TRUNC;
    q->name = new_STRING("/dev/stderr");
    q->ptr = (PTR) stderr;

    p = alloc_filenode();
    p->link = q;
    p->type = F_TRUNC;
    p->name = new_STRING("/dev/stdout");
    p->ptr = (PTR) stdout;

    r = alloc_filenode();
    r->link = p;
    r->type = F_IN;
    r->name = new_STRING("/dev/stdin");
    r->ptr = (PTR) stdin;

    file_list = r;
}

/* fopen() but no buffering to ttys */
static FILE *
tfopen(const char *name, const char *mode)
{
    FILE *retval = fopen(name, mode);

    if (retval) {
	if (isatty(fileno(retval)))
	    setbuf(retval, (char *) 0);
	else {
#ifdef MSDOS
	    enlarge_output_buffer(retval);
#endif
	}
    }
    return retval;
}

#ifdef MSDOS
void
enlarge_output_buffer(FILE *fp)
{
    if (setvbuf(fp, (char *) 0, _IOFBF, BUFFSZ) < 0) {
	errmsg(errno, "setvbuf failed on fileno %d", fileno(fp));
	mawk_exit(2);
    }
}
#endif

#if USE_BINMODE
void
stdout_init(void)
{
#ifdef MSDOS
    if (!isatty(1))
	enlarge_output_buffer(stdout);
#endif
    if (binmode() & 2) {
	setmode(1, O_BINARY);
	setmode(2, O_BINARY);
    }
}
#endif /* USE_BINMODE */

/* An error occurred closing the file referred to by P. We tell the
   user and terminate the program. */

static void
close_error(FILE_NODE * p)
{
    TRACE(("close_error(%s)\n", p->name->str));
    errmsg(errno, "close failed on file %s", p->name->str);
#ifdef NO_LEAKS
    free_filenode(p);
#endif
    mawk_exit(2);
}

#ifdef NO_LEAKS
void
files_leaks(void)
{
    TRACE(("files_leaks\n"));
    while (file_list != 0) {
	FILE_NODE *p = file_list;
	file_list = p->link;
	free_filenode(p);
    }
}
#endif
