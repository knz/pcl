
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <pcl.h>

/*
  Implements a simple cooperative multi-threading environment.

  Copyright 2000 by E. Toernig <froese@gmx.de>.

  The API is made out of four functions:

  void cothread_init(void)
  Initializes the data structures.  Has to be called once
  at program start.

  coroutine_t cothread_new(void (*func)(), ...)
  Creates a new coroutine.  func is called as it's startup
  function and the additional arguments are passed as a va_list.
  Returns 0 on success and -1 on failure.

  int cothread_wait(int mode [, int fd] [, int timeout])
  mode is the bitwise OR of IOREAD, IOWRITE, and IOEXCEPT which
  require the fd, and IOTIMEOUT which requires the timeout.
  If any of the conditions become true, the function returns
  with the mode-bits that became ready.

  There are some special combinations:
  cothread_wait(0)
  Waits forever.  Coroutine is deleted.	 This is the
  standard method to delete a coroutine.
  cothread_wait(IOTIMEOUT, ms)
  Sleeps for ms milliseconds.
  cothread_wait(IOTIMEOUT, 0)
  Return immediately.  Side effect: all other coroutines
  waiting for an event get a chance to run.

  int cothread_schedule(void)
  Give up processing and let other coroutines run.  To restart
  this one, another coroutine has to co_call back.

  The cothread routines only manage coroutines that are currently
  executing cothread_wait.  You are not required to create them via
  cothread_new.	 Any coroutine may use these functions.	 cothread_new
  just makes sure, that execution comes back to the creating coroutine.

  [[xref: co_create, co_delete, co_call, co_current]]
  [[xref: select, gettimeofday, memset]]
*/



#define IOREAD		1	// wait for fd to become readable
#define IOWRITE		2	// wait for fd to become writeable
#define IOEXCEPT	4	// wait for an exception condition on fd
#define IOTIMEOUT	8	// time out of specified time

struct ioreq {
	struct ioreq *next;
	coroutine_t coro;	// coroutine that is waiting
	int mode;			// the events it is waiting for
	int fd;			// optional file descriptor
	struct timeval timeout[1];	// optional time out
};

struct ioqueue {
	struct ioreq *req;		// first request in this queue
	int maxfd;			// highest fd used in the requests
	struct timeval *mintime;	// earliest timeout in the requests
	fd_set *rp, *wp, *ep;	// pointers to the fd_sets below.
	fd_set rfds[1], wfds[1], efds[1];	// fd_sets for the select
};

struct iosched {
	struct timeval ctime[1];	// system time after last select
	struct ioqueue *active;	// requests processed by the last poll
	struct ioqueue *wait;	// requests for the next poll
	struct ioqueue queues[2];	// data area of the queues.
};


static struct timeval *tvadd(struct timeval *dst, struct timeval *a,
			     struct timeval *b)
{
	dst->tv_sec = a->tv_sec + b->tv_sec;
	dst->tv_usec = a->tv_usec + b->tv_usec;
	if (dst->tv_usec >= 1000000)
		dst->tv_sec++, dst->tv_usec -= 1000000;

	return dst;
}

static struct timeval *tvsub(struct timeval *dst, struct timeval *a,
			     struct timeval *b)
{
	dst->tv_sec = a->tv_sec - b->tv_sec;
	dst->tv_usec = a->tv_usec - b->tv_usec;
	if (dst->tv_usec < 0)
		dst->tv_sec--, dst->tv_usec += 1000000;

	return dst;
}

static long tvcmp(struct timeval *a, struct timeval *b)
{
	if (a->tv_sec - b->tv_sec)
		return a->tv_sec - b->tv_sec;

	return a->tv_usec - b->tv_usec;
}

static struct timeval *to2tv(struct timeval *dst, int timeout)
{
	dst->tv_sec = timeout / 1000;
	dst->tv_usec = timeout % 1000 * 1000;

	return dst;
}

static void set_fds(struct ioreq *r, int mode, fd_set *fds, fd_set **fp)
{
	if (r->mode & mode) {
		FD_SET(r->fd, fds);
		*fp = fds;
	}
}

static int tst_fds(struct ioreq *r, int mode, fd_set *fds)
{
	if (r->mode & mode)
		if (FD_ISSET(r->fd, fds)) {
			FD_CLR(r->fd, fds);
			return mode;
		}

	return 0;
}

static int check(struct ioqueue *q, struct ioreq *r, struct timeval *ctime)
{
	int res = 0;

	if (r->mode & (IOREAD|IOWRITE|IOEXCEPT)) {
		res |= tst_fds(r, IOREAD, q->rp);
		res |= tst_fds(r, IOWRITE, q->wp);
		res |= tst_fds(r, IOEXCEPT, q->ep);
	}
	if (res == 0) // IOTIMEOUT has lower precedence
		if (r->mode & IOTIMEOUT)
			if (tvcmp(r->timeout, ctime) <= 0)
				res |= IOTIMEOUT;
	return res;
}

static void enqueue(struct ioqueue *q, struct ioreq *r)
{
	if (r->mode & (IOREAD|IOWRITE|IOEXCEPT)) {
		set_fds(r, IOREAD, q->rfds, &q->rp);
		set_fds(r, IOWRITE, q->wfds, &q->wp);
		set_fds(r, IOEXCEPT, q->efds, &q->ep);
		if (r->fd >= q->maxfd)
			q->maxfd = r->fd + 1;
	}
	if (r->mode & IOTIMEOUT)
		if (!q->mintime || tvcmp(q->mintime, r->timeout) > 0)
			q->mintime = r->timeout;
	r->next = q->req;
	q->req = r;
}

static void vadd_req(struct iosched *glbl, struct ioreq *r, int mode,
		     va_list args)
{
	r->coro = co_current();
	r->mode = mode;
	if (mode & (IOREAD|IOWRITE|IOEXCEPT))
		r->fd = va_arg(args, int);
	if (mode & IOTIMEOUT)
		tvadd(r->timeout, to2tv(r->timeout, va_arg(args, int)),
		      glbl->ctime);

	enqueue(glbl->wait, r);
}

static void add_req(struct iosched *glbl, struct ioreq *r, int mode, ...)
{
	va_list args;

	va_start(args, mode);
	vadd_req(glbl, r, mode, args);
	va_end(args);
}

static int cothread_schedule(struct iosched *glbl)
{
	struct ioqueue *q;
	struct ioreq *r;
	struct timeval tv[1];
	int res;

	for (;;) {
		q = glbl->active;
		while ((r = q->req) != NULL) {
			q->req = r->next;
			if ((res = check(q, r, glbl->ctime)) != 0) {
				co_call(r->coro);
				return -1;
			}
			if (r->mode == 0 && r->coro != co_current())
				co_delete(r->coro);
			else
				enqueue(glbl->wait, r);
		}
		q->rp = q->wp = q->ep = 0;
		q->mintime = 0;
		q->maxfd = 0;
		glbl->active = glbl->wait;
		glbl->wait = q;

		q = glbl->active;
		if (q->mintime)
			q->mintime = tvsub(tv, q->mintime, glbl->ctime);

		while (select(q->maxfd, q->rp, q->wp, q->ep, q->mintime) == -1)
			;
		gettimeofday(glbl->ctime, 0);
	}

	return 0;
}

static int cothread_wait(struct iosched *glbl, int mode, ...)
{
	va_list args;
	struct ioreq req[1];

	va_start(args, mode);
	vadd_req(glbl, req, mode, args);
	va_end(args);

	return cothread_schedule(glbl);
}

static coroutine_t cothread_new(void (*func)(), ...)
{
	coroutine_t co;
	va_list args;
	struct iosched *glbl;
	struct ioreq req[1];

	va_start(args, func);
	glbl = va_arg(args, struct iosched *);
	va_end(args);

	add_req(glbl, req, IOTIMEOUT, 0);
	va_start(args, func);
	if ((co = co_create(func, &args, 0, 32768)))
		co_call(co);

	va_end(args);
	return co;
}

static void cothread_init(struct iosched *glbl)
{
	memset(glbl, 0, sizeof(*glbl));
	gettimeofday(glbl->ctime, 0);
	glbl->active = glbl->queues;
	glbl->wait = glbl->queues + 1;
	memset(glbl->queues, 0, sizeof(glbl->queues));
}

static void test1(va_list *args)
{
	struct iosched *glbl = va_arg(*args, struct iosched *);
	char *str = va_arg(*args, char *);
	int limit = va_arg(*args, int);
	int i = 0;

	printf("%s started\n", str);
	while (i < limit) {
		cothread_wait(glbl, IOTIMEOUT, 1000);
		printf("%s: %d\n", str, i++);
	}
	printf("%s: dying\n", str);
	cothread_wait(glbl, 0);
}

static void test2(va_list *args)
{
	struct iosched *glbl = va_arg(*args, struct iosched *);
	char *str = va_arg(*args, char *);
	int in = va_arg(*args, int);
	int out = va_arg(*args, int);
	char buf[256];
	int n;

	printf("%s started\n", str);
	for (;;) {
		cothread_wait(glbl, IOREAD, in);
		if ((n = read(in, buf, sizeof(buf))) <= 0)
			break;
		cothread_wait(glbl, IOWRITE, out);
		write(out, buf, n);
	}
	printf("%s: dying\n", str);
	cothread_wait(glbl, 0);
}

static void *run_test(void *data)
{
	struct iosched glbl[1];

	cothread_init(glbl);

	cothread_new(test1, glbl, "test1a", 10);
	cothread_new(test1, glbl, "test1b", 12);
	cothread_new(test1, glbl, "test1c", 14);
	cothread_new(test2, glbl, "test2", 0, 2);

	for (;;) {
		printf("main: waiting ...\n");
		cothread_wait(glbl, IOTIMEOUT, 3000);
	}

	return NULL;
}

static void *thread_proc(void *data)
{
	void *result;

	co_thread_init();
	result = run_test(data);
	co_thread_cleanup();

	return result;
}

int main(int argc, char **argv)
{
	int i, nthreads;
	pthread_t *thids;

	nthreads = 1;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0) {
			if (++i < argc)
				nthreads = atoi(argv[i]);
		}
	}
	if (nthreads == 1)
		run_test(NULL);
	else {
		thids = (pthread_t *) malloc(nthreads * sizeof(pthread_t));
		for (i = 0; i < nthreads; i++) {
			if (pthread_create(&thids[i], NULL, thread_proc,
					   NULL)) {
				perror("creating worker threads");
				return 1;
			}
		}
		for (i = 0; i < nthreads; i++)
			pthread_join(thids[i], NULL);
	}

	return 0;
}

