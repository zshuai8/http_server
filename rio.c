#include "rio.h"

/**************************
 * Error-handling functions
 **************************/

/* Prints unix-style errors printed to standard error.  Uses the actual standard
 * errors to do so, which is pretty cool.
 */
void unix_error(char *msg) /* unix-style error */
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(0);
}

/*
 * rio_readn - "robustly" read n bytes. Unbuffered
 * Takes a file descriptor and a pointer to a buffer.
 */
ssize_t rio_readn(int fd, void *usrbuf, size_t n)
{
    size_t nleft = n;
    ssize_t nread;
    char *bufp = usrbuf;

    while (nleft > 0)   //while more chars left to read
    {
    	if ((nread = read(fd, bufp, nleft)) < 0)
        {
            /* If there was an error in reading */
    	    if (errno == EINTR)     /* interrupted by sig handler return */
            {
    		    nread = 0;              /* and call read() again */
            }
            else
            {
    		    return -1;              /* errno set by read() */
            }
        }
    	else if (nread == 0) break;                  /* EOF */
        /* Fix and continue */
    	nleft -= nread;
    	bufp += nread;
    }
    return (n - nleft);         /* return >= 0 */
}

/*
 * rio_writen - robustly write n bytes. Unbuffered.
 * Takes a file descriptor and a pointer to a buffer.
 */
ssize_t rio_writen(int fd, void *usrbuf, size_t n)
{
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    while (nleft > 0) {
    	if ((nwritten = write(fd, bufp, nleft)) <= 0)
        {
    	    if (errno == EINTR)     /* interrupted by sig handler return */
            {
                nwritten = 0;           /* and call write() again */
            }
            else
            {
                return -1;              /* errorno set by write() */
            }
        }
	    nleft -= nwritten;
	    bufp += nwritten;
    }
    return n;
}


/*
 * rio_read - This is a wrapper for the Unix read() function that
 *    transfers min(n, rio_cnt) bytes from an internal buffer to a user
 *    buffer, where n is the number of bytes requested by the user and
 *    rio_cnt is the number of unread bytes in the internal buffer. On
 *    entry, rio_read() refills the internal buffer via a call to
 *    read() if the internal buffer is empty.
 */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0)
    {  /* refill if buf is empty */
    	rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
    	if (rp->rio_cnt < 0)
        {
    	    if (errno != EINTR) /* interrupted by sig handler return */
            {
                return -1;
            }
    	}
    	else if (rp->rio_cnt == 0)  return 0;/* EOF */
    	else rp->rio_bufptr = rp->rio_buf; /* reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;
    if (rp->rio_cnt < n) cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/*
 * rio_readinitb - Associate a descriptor with a read buffer and reset buffer
 */
void rio_readinitb(rio_t *rp, int fd)
{
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

/*
 * rio_readnb - Robustly read n bytes (buffered)
 */
ssize_t rio_readnb(rio_t *rp, void *usrbuf, size_t n)
{
    size_t nleft = n;
    ssize_t nread;
    char *bufp = usrbuf;

    while (nleft > 0)
    {
    	if ((nread = rio_read(rp, bufp, nleft)) < 0)
        {
    	    if (errno == EINTR) /* interrupted by sig handler return */
            {
                nread = 0;      /* call read() again */
            }
            else return -1;      /* errno set by read() */
    	}
    	else if (nread == 0) break;     /* EOF */
    	nleft -= nread;
    	bufp += nread;
    }
    return (n - nleft);         /* return >= 0 */
}

/*
 * rio_readlineb - robustly read a text line (buffered)
 */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++)
    {
    	if ((rc = rio_read(rp, &c, 1)) == 1)
        {
    	    *bufp++ = c;
    	    if (c == '\n') break;
    	}
        else if (rc == 0)
        {
    	    if (n == 1) return 0; /* EOF, no data read */
    	    else break;    /* EOF, some data was read */
    	}
        else return -1;	  /* error */
    }
    *bufp = 0;
    return n;
}

/**********************************
 * Wrappers for robust I/O routines
 **********************************/

/* Wrapper for read (unbuffered) */
ssize_t Rio_readn(int fd, void *ptr, size_t nbytes)
{
    ssize_t n;
    n = rio_readn(fd, ptr, nbytes);
    return n;
}

/* Wrapper for write (unbuffered) */
void Rio_writen(int fd, void *usrbuf, size_t n)
{
    rio_writen(fd, usrbuf, n);
}


void Rio_readinitb(rio_t *rp, int fd)
{
    rio_readinitb(rp, fd);
}

/* Wrapper for read (buffered) */
ssize_t Rio_readnb(rio_t *rp, void *usrbuf, size_t n)
{
    ssize_t rc;
    rc = rio_readnb(rp, usrbuf, n);
    return rc;
}

/* Wrapper for write (buffered) */
ssize_t Rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    ssize_t rc;
    rc = rio_readlineb(rp, usrbuf, maxlen);
    return rc;
}
