/*
 * Signal handling for Plan 9 programs. 
 * We stubbornly use the strings from Plan 9 instead 
 * of the enumerated Unix constants.  
 * There are some weird translations.  In particular,
 * a "kill" note is the same as SIGTERM in Unix.
 * There is no equivalent note to Unix's SIGKILL, since
 * it's not a deliverable signal anyway.
 *
 * We do not handle SIGABRT or SIGSEGV, mainly because
 * the thread library queues its notes for later, and we want
 * to dump core with the state at time of delivery.
 *
 * We have to add some extra entry points to provide the
 * ability to tweak which signals are deliverable and which
 * are acted upon.  Notifydisable and notifyenable play with
 * the process signal mask.  Notifyignore enables the signal
 * but will not call notifyf when it comes in.  This is occasionally
 * useful.
 */

#include <u.h>
#include <signal.h>
#define NOPLAN9DEFINES
#include <libc.h>

extern char *_p9sigstr(int, char*);
extern int _p9strsig(char*);

typedef struct Sig Sig;
struct Sig
{
	int sig;			/* signal number */
	int restart;			/* do we restart the system call after this signal is handled? */
	int enabled;		/* is this signal enabled (not masked)? */
	int notified;		/* do we call the notify function for this signal? */
};

/* initial settings; for current status, ask the kernel */
static Sig sigs[] = {
	SIGHUP, 0, 1, 1,
	SIGINT, 0, 1, 1,
	SIGQUIT, 0, 1, 1,
	SIGILL, 0, 1, 1,
	SIGTRAP, 0, 1, 1,
/*	SIGABRT, 0, 1, 1,	*/
#ifdef SIGEMT
	SIGEMT, 0, 1, 1,
#endif
	SIGFPE, 0, 1, 1,
	SIGBUS, 0, 1, 1,
/*	SIGSEGV, 0, 1, 1,	*/
	SIGCHLD, 1, 0, 1,
	SIGSYS, 0, 1, 1,
	SIGPIPE, 0, 0, 1,
	SIGALRM, 0, 1, 1,
	SIGTERM, 0, 1, 1,
	SIGTSTP, 1, 0, 1,
	SIGTTIN, 1, 0, 1,
	SIGTTOU, 1, 0, 1,
	SIGXCPU, 0, 1, 1,
	SIGXFSZ, 0, 1, 1,
	SIGVTALRM, 0, 1, 1,
	SIGUSR1, 0, 1, 1,
	SIGUSR2, 0, 1, 1,
	SIGWINCH, 1, 0, 1,
#ifdef SIGINFO
	SIGINFO, 1, 1, 1,
#endif
};

static Sig*
findsig(int s)
{
	int i;

	for(i=0; i<nelem(sigs); i++)
		if(sigs[i].sig == s)
			return &sigs[i];
	return nil;
}

/*
 * The thread library initializes _notejmpbuf to its own
 * routine which provides a per-pthread jump buffer.
 * If we're not using the thread library, we assume we are
 * single-threaded.
 */
typedef struct Jmp Jmp;
struct Jmp
{
	p9jmp_buf b;
};

static Jmp onejmp;

static Jmp*
getonejmp(void)
{
	return &onejmp;
}

Jmp *(*_notejmpbuf)(void) = getonejmp;
static void noteinit(void);

/*
 * Actual signal handler. 
 */

static void (*notifyf)(void*, char*);	/* Plan 9 handler */

static void
signotify(int sig)
{
	char tmp[64];
	Jmp *j;

	j = (*_notejmpbuf)();
	switch(p9setjmp(j->b)){
	case 0:
		if(notifyf)
			(*notifyf)(nil, _p9sigstr(sig, tmp));
		/* fall through */
	case 1:	/* noted(NDFLT) */
		if(0)print("DEFAULT %d\n", sig);
		signal(sig, SIG_DFL);
		raise(sig);
		_exit(1);
	case 2:	/* noted(NCONT) */
		if(0)print("HANDLED %d\n", sig);
		return;
	}
}

static void
signonotify(int sig)
{
	USED(sig);
}

int
noted(int v)
{
	p9longjmp((*_notejmpbuf)()->b, v==NCONT ? 2 : 1);
	abort();
	return 0;
}

int
notify(void (*f)(void*, char*))
{
	static int init;

	notifyf = f;
	if(!init){
		init = 1;
		noteinit();
	}
	return 0;
}

/*
 * Nonsense about enabling and disabling signals.
 */
typedef void Sighandler(int);
static Sighandler*
handler(int s)
{
	struct sigaction sa;

	sigaction(s, nil, &sa);
	return sa.sa_handler;
}

static void
notesetenable(int sig, int enabled)
{
	sigset_t mask;

	if(sig == 0)
		return;

	sigemptyset(&mask);
	sigaddset(&mask, sig);
	sigprocmask(enabled ? SIG_UNBLOCK : SIG_BLOCK, &mask, nil);
}

void
noteenable(char *msg)
{
	notesetenable(_p9strsig(msg), 1);
}

void
notedisable(char *msg)
{
	notesetenable(_p9strsig(msg), 0);
}

static void
notifyseton(int s, int on)
{
	Sig *sig;
	struct sigaction sa;

	sig = findsig(s);
	if(sig == nil)
		return;
	if(on)
		notesetenable(s, 1);
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on ? signotify : signonotify;
	if(sig->restart)
		sa.sa_flags |= SA_RESTART;

	/*
	 * We can't allow signals within signals because there's
	 * only one jump buffer.
	 */
	sigfillset(&sa.sa_mask);

	/*
	 * Install handler.
	 */
	sigaction(sig->sig, &sa, nil);
}

void
notifyon(char *msg)
{
	notifyseton(_p9strsig(msg), 1);
}

void
notifyoff(char *msg)
{
	notifyseton(_p9strsig(msg), 0);
}

/*
 * Initialization follows sigs table.
 */
static void
noteinit(void)
{
	int i;
	Sig *sig;

	for(i=0; i<nelem(sigs); i++){
		sig = &sigs[i];
		/*
		 * If someone has already installed a handler,
		 * It's probably some ld preload nonsense,
		 * like pct (a SIGVTALRM-based profiler).
		 * Or maybe someone has already called notifyon/notifyoff.
		 * Leave it alone.
		 */
		if(handler(sig->sig) != SIG_DFL)
			continue;
		/*
		 * Should we only disable and not enable signals?
		 * (I.e. if parent has disabled for us, should we still enable?)
		 * Right now we always initialize to the state we want.
		 */
		notesetenable(sig->sig, sig->enabled);
		notifyseton(sig->sig, sig->notified);
	}
}

