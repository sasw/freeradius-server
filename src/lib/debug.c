/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * @file debug.c
 * @brief Various functions to aid in debugging
 *
 * @copyright 2013  The FreeRADIUS server project
 * @copyright 2013  Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
#include <freeradius-devel/libradius.h>
#include <sys/stat.h>

/*
 *	runtime backtrace functions are not POSIX but are included in
 *	glibc, OSX >= 10.5 and various BSDs
 */
#ifdef HAVE_EXECINFO
#  include <execinfo.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
#endif

#ifdef HAVE_PTHREAD_H
#  define PTHREAD_MUTEX_LOCK pthread_mutex_lock
#  define PTHREAD_MUTEX_UNLOCK pthread_mutex_unlock
#else
#  define PTHREAD_MUTEX_LOCK(_x)
#  define PTHREAD_MUTEX_UNLOCK(_x)
#endif

#ifdef HAVE_EXECINFO
#  define MAX_BT_FRAMES 128
#  define MAX_BT_CBUFF  65536			//!< Should be a power of 2

#  ifdef HAVE_PTHREAD_H
static pthread_mutex_t fr_debug_init = PTHREAD_MUTEX_INITIALIZER;
#  endif

typedef struct fr_bt_info {
	void 		*obj;				//!< Memory address of the block of allocated memory.
	void		*frames[MAX_BT_FRAMES];		//!< Backtrace frame data
	int		count;				//!< Number of frames stored
} fr_bt_info_t;

struct fr_bt_marker {
	void 		*obj;			//!< Pointer to the parent object, this is our needle
						//!< when we iterate over the contents of the circular buffer.
	fr_cbuff_t 	*cbuff;			//!< Where we temporarily store the backtraces
};
#endif

static char panic_action[512];
static fr_fault_cb panic_cb;
static int fr_debugger_present = -1;

#ifdef HAVE_SYS_RESOURCE_H
static struct rlimit core_limits;
#endif

/** Stub callback to see if the SIGTRAP handler is overriden
 *
 * @param signum signal raised.
 */
static void _sigtrap_handler(UNUSED int signum)
{
	fr_debugger_present = 0;
	signal(SIGTRAP, SIG_DFL);
}

/** Break in debugger (if were running under a debugger)
 *
 * If the server is running under a debugger this will raise a
 * SIGTRAP which will pause the running process.
 *
 * If the server is not running under debugger then this will do nothing.
 */
void fr_debug_break(void)
{
	if (fr_debugger_present == -1) {
		fr_debugger_present = 0;
		signal(SIGTRAP, _sigtrap_handler);
		raise(SIGTRAP);
	} else if (fr_debugger_present == 1) {
		raise(SIGTRAP);
	}
}

#ifdef HAVE_EXECINFO
/** Generate a backtrace for an object during destruction
 *
 * If this is the first entry being inserted
 */
static int _fr_do_bt(fr_bt_marker_t *marker)
{
	fr_bt_info_t *bt;

	if (!fr_assert(marker->obj) || !fr_assert(marker->cbuff)) {
		return -1;
	}

	bt = talloc_zero(marker->cbuff, fr_bt_info_t);
	if (!bt) {
		return -1;
	}
	bt->count = backtrace(bt->frames, MAX_BT_FRAMES);
	fr_cbuff_rp_insert(marker->cbuff, bt);

	return 0;
}

/** Print backtrace entry for a given object
 *
 * @param cbuff to search in.
 * @param obj pointer to original object
 */
void backtrace_print(fr_cbuff_t *cbuff, void *obj)
{
	fr_bt_info_t *p;
	bool found = false;
	int i = 0;
	char **frames;

	while ((p = fr_cbuff_rp_next(cbuff, NULL))) {
		if ((p == obj) || !obj) {
			found = true;
			frames = backtrace_symbols(p->frames, p->count);

			fprintf(stderr, "Stacktrace for: %p\n", p);
			for (i = 0; i < p->count; i++) {
				fprintf(stderr, "%s\n", frames[i]);
			}

			/* We were only asked to look for one */
			if (obj) {
				return;
			}
		}
	};

	if (!found) {
		fprintf(stderr, "No backtrace available for %p", obj);
	}
}

/** Inserts a backtrace marker into the provided context
 *
 * Allows for maximum laziness and will initialise a circular buffer if one has not already been created.
 *
 * Code augmentation should look something like:
@verbatim
	// Create a static cbuffer pointer, the first call to backtrace_attach will initialise it
	static fr_cbuff *my_obj_bt;

	my_obj_t *alloc_my_obj(TALLOC_CTX *ctx) {
		my_obj_t *this;

		this = talloc(ctx, my_obj_t);

		// Attach backtrace marker to object
		backtrace_attach(&my_obj_bt, this);

		return this;
	}
@endverbatim
 *
 * Then, later when a double free occurs:
@verbatim
	(gdb) call backtrace_print(&my_obj_bt, <pointer to double freed memory>)
@endverbatim
 *
 * which should print a limited backtrace to stderr. Note, this backtrace will not include any argument
 * values, but should at least show the code path taken.
 *
 * @param cbuff this should be a pointer to a static *fr_cbuff.
 * @param obj we want to generate a backtrace for.
 */
fr_bt_marker_t *fr_backtrace_attach(fr_cbuff_t **cbuff, TALLOC_CTX *obj)
{
	fr_bt_marker_t *marker;

	if (*cbuff == NULL) {
		PTHREAD_MUTEX_LOCK(&fr_debug_init);
		/* Check again now we hold the mutex - eww*/
		if (*cbuff == NULL) {
			TALLOC_CTX *ctx;

			ctx = fr_autofree_ctx();
			*cbuff = fr_cbuff_alloc(ctx, MAX_BT_CBUFF, true);
		}
		PTHREAD_MUTEX_UNLOCK(&fr_debug_init);
	}

	marker = talloc(obj, fr_bt_marker_t);
	if (!marker) {
		return NULL;
	}

	marker->obj = (void *) obj;
	marker->cbuff = *cbuff;

	talloc_set_destructor(marker, _fr_do_bt);

	return marker;
}
#else
void backtrace_print(UNUSED fr_cbuff_t *cbuff, UNUSED void *obj)
{
	fprintf(stderr, "Server built without fr_backtrace_* support, requires execinfo.h and possibly -lexecinfo\n");
}
fr_bt_marker_t *fr_backtrace_attach(UNUSED fr_cbuff_t **cbuff, UNUSED TALLOC_CTX *obj)
{
	fprintf(stderr, "Server built without fr_backtrace_* support, requires execinfo.h and possibly -lexecinfo\n");
	abort();
}
#endif /* ifdef HAVE_EXECINFO */

/** Set the dumpable flag, also controls whether processes can PATTACH
 *
 * @param dumpable whether we should allow core dumping
 */
#if defined(HAVE_SYS_PRCTL_H) && defined(PR_SET_DUMPABLE)
static int fr_set_dumpable_flag(bool dumpable)
{
	if (prctl(PR_SET_DUMPABLE, dumpable ? 1 : 0) < 0) {
		fr_strerror_printf("Cannot re-enable core dumps: prctl(PR_SET_DUMPABLE) failed: %s",
				   fr_syserror(errno));
		return -1;
	}

	return 0;
}
#else
static int fr_set_dumpable_flag(UNUSED bool dumpable)
{
	return 0;
}
#endif

/** Get the current maximum for core files
 *
 * Do this before anything else so as to ensure it's properly initialized.
 */
int fr_set_dumpable_init(void)
{
#ifdef HAVE_SYS_RESOURCE_H
	if (getrlimit(RLIMIT_CORE, &core_limits) < 0) {
		fr_strerror_printf("Failed to get current core limit:  %s", fr_syserror(errno));
		return -1;
	}
#endif
	return 0;
}

/** Enable or disable core dumps
 *
 * @param allow_core_dumps whether to enable or disable core dumps.
 */
int fr_set_dumpable(bool allow_core_dumps)
{
	/*
	 *	If configured, turn core dumps off.
	 */
	if (!allow_core_dumps) {
#ifdef HAVE_SYS_RESOURCE_H
		struct rlimit no_core;

		no_core.rlim_cur = 0;
		no_core.rlim_max = 0;

		if (setrlimit(RLIMIT_CORE, &no_core) < 0) {
			fr_strerror_printf("Failed disabling core dumps: %s", fr_syserror(errno));

			return -1;
		}
#endif
		return 0;
	}

	if (fr_set_dumpable_flag(true) < 0) return -1;

	/*
	 *	Reset the core dump limits to their original value.
	 */
#ifdef HAVE_SYS_RESOURCE_H
	if (setrlimit(RLIMIT_CORE, &core_limits) < 0) {
		fr_strerror_printf("Cannot update core dump limit: %s", fr_syserror(errno));

		return -1;
	}
#endif
	return 0;
}

/** Check to see if panic_action file is world writeable
 *
 * @return 0 if file is OK, else -1.
 */
static int fr_fault_check_permissions(void)
{
	char const *p, *q;
	char *filename = NULL;
	struct stat statbuf;

	/*
	 *	Try and guess which part of the command is the binary, and check to see if
	 *	it's world writeable, to try and save the admin from their own stupidity.
	 *
	 *	@fixme we should do this properly and take into account single and double
	 *	quotes.
	 */
	if ((q = strchr(panic_action, ' '))) {
		(void) asprintf(&filename, "%.*s", (int)(q - panic_action), panic_action);
		p = filename;
	} else {
		p = panic_action;
	}

	if (stat(p, &statbuf) == 0) {
#ifdef S_IWOTH
		if ((statbuf.st_mode & S_IWOTH) != 0) {
			fr_strerror_printf("panic_action file \"%s\" is globally writable", p);
			return -1;
		}
#endif
	}

	free(filename);

	return 0;
}

/** Prints a simple backtrace (if execinfo is available) and calls panic_action if set.
 *
 * @param sig caught
 */
void fr_fault(int sig)
{
	char cmd[sizeof(panic_action) + 20];
	char *out = cmd;
	size_t left = sizeof(cmd), ret;

	char const *p = panic_action;
	char const *q;

	int code;

	fprintf(stderr, "CAUGHT SIGNAL: %s\n", strsignal(sig));

	/*
	 *	Check for administrator sanity.
	 */
	if (fr_fault_check_permissions() < 0) {
		fprintf(stderr, "Refusing to execute panic action: %s\n", fr_strerror());
		return;
	}

#ifdef SIGUSR1
	/*
	 *	SIGUSR1 skips the callback, and the backtrace and just runs the
	 *	panic_action.
	 */
	if (sig == SIGUSR1) goto skip_backtrace;
#endif

	/*
	 *	Run the callback if one was registered
	 */
	if (panic_cb && (panic_cb(sig) < 0)) fr_exit_now(1);

	/*
	 *	Produce a simple backtrace - They've very basic but at least give us an
	 *	idea of the area of the code we hit the issue in.
	 */
#ifdef HAVE_EXECINFO
	size_t frame_count, i;
	void *stack[MAX_BT_FRAMES];
	char **frames;

	frame_count = backtrace(stack, MAX_BT_FRAMES);
	frames = backtrace_symbols(stack, frame_count);

	fprintf(stderr, "Backtrace of last %zu frames:\n", frame_count);
	for (i = 0; i < frame_count; i++) {
		fprintf(stderr, "%s\n", frames[i]);
		/* Leak the backtrace strings, freeing may lead to undefined behaviour... */
	}
#endif

skip_backtrace:
	/* No panic action set... */
	if (panic_action[0] == '\0') {
		fprintf(stderr, "No panic action set\n");
		fr_exit_now(1);
	}

	/* Substitute %p for the current PID (useful for attaching a debugger) */
	while ((q = strstr(p, "%p"))) {
		out += ret = snprintf(out, left, "%.*s%d", (int) (q - p), p, (int) getpid());
		if (left <= ret) {
		oob:
			fprintf(stderr, "Panic action too long\n");
			fr_exit_now(1);
		}
		left -= ret;
		p = q + 2;
	}
	if (strlen(p) >= left) goto oob;
	strlcpy(out, p, left);

	fprintf(stderr, "Calling: %s\n", cmd);
	code = system(cmd);
	fprintf(stderr, "Panic action exited with %i\n", code);

#ifdef SIGUSR1
	if (sig == SIGUSR1) return;
#endif

#ifdef SIGUSR2
	if (sig == SIGUSR2) return;
#endif
	fr_exit_now(1);
}

#ifdef SIGABRT
static void _fr_talloc_fault(UNUSED char const *message)
{
	fr_fault(SIGABRT);
}
#endif

/** Registers signal handlers to execute panic_action on fatal signal
 *
 * May be called multiple time to change the panic_action/program.
 *
 * @param cmd to execute on fault. If present %p will be substituted
 *        for the parent PID before the command is executed, and %e
 *        will be substituted for the currently running program.
 * @param program Name of program currently executing (argv[0]).
 * @return 0 on success -1 on failure.
 */
int fr_fault_setup(char const *cmd, char const *program)
{
	static bool setup = false;

	char *out = panic_action;
	size_t left = sizeof(panic_action), ret;

	char const *p = cmd;
	char const *q;

	if (cmd) {
		/* Substitute %e for the current program */
		while ((q = strstr(p, "%e"))) {
			out += ret = snprintf(out, left, "%.*s%s", (int) (q - p), p, program ? program : "");
			if (left <= ret) {
			oob:
				fr_strerror_printf("Panic action too long");
				return -1;
			}
			left -= ret;
			p = q + 2;
		}
		if (strlen(p) >= left) goto oob;
		strlcpy(out, p, left);
	} else {
		*panic_action = '\0';
	}

	/*
	 *	Check for administrator sanity.
	 */
	if (fr_fault_check_permissions() < 0) return -1;

	/*
	 *	This is required on some systems to be able to PATTACH to the process.
	 */
	fr_set_dumpable_flag(true);

	/* Unsure what the side effects of changing the signal handler mid execution might be */
	if (!setup) {
#ifdef SIGSEGV
		if (fr_set_signal(SIGSEGV, fr_fault) < 0) return -1;
#endif
#ifdef SIGBUS
		if (fr_set_signal(SIGBUS, fr_fault) < 0) return -1;
#endif
#ifdef SIGABRT
		if (fr_set_signal(SIGABRT, fr_fault) < 0) return -1;
		/*
		 *  Use this instead of abort so we get a
		 *  full backtrace with broken versions of LLDB
		 */
		talloc_set_abort_fn(_fr_talloc_fault);
#endif
#ifdef SIGFPE
		if (fr_set_signal(SIGFPE, fr_fault) < 0) return -1;
#endif

#ifdef SIGUSR1
		if (fr_set_signal(SIGUSR1, fr_fault) < 0) return -1;
#endif

#ifdef SIGUSR2
		if (fr_set_signal(SIGUSR2, fr_fault) < 0) return -1;
#endif
	}
	setup = true;

	return 0;
}

/** Set a callback to be called before fr_fault()
 *
 * @param cb to execute. If callback returns < 0
 *	fr_fault will exit before running panic_action code.
 */
void fr_fault_set_cb(fr_fault_cb cb)
{
	panic_cb = cb;
};
