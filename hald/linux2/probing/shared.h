
#ifndef SHARED_H
#define SHARED_H

#include <time.h>
#include <sys/time.h>
#include <pwd.h>
#include <grp.h>

#include <sys/types.h>
#include <sys/syslog.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

static dbus_bool_t is_verbose = FALSE;
static dbus_bool_t use_syslog = FALSE;

static void
_do_dbg (const char *format, va_list args)
{
	char buf[512];
	char tbuf[256];
	struct timeval tnow;
	struct tm *tlocaltime;
	struct timezone tzone;
	static pid_t pid = -1;

	if (!is_verbose)
		return;

	if ((int) pid == -1)
		pid = getpid ();

	vsnprintf (buf, sizeof (buf), format, args);

	gettimeofday (&tnow, &tzone);
	tlocaltime = localtime (&tnow.tv_sec);
	strftime (tbuf, sizeof (tbuf), "%H:%M:%S", tlocaltime);

	if (use_syslog)
		syslog (LOG_INFO, "%d: %s.%03d: %s", pid, tbuf, (int)(tnow.tv_usec/1000), buf);
	else
		fprintf (stderr, "%d: %s.%03d: %s", pid, tbuf, (int)(tnow.tv_usec/1000), buf);

	va_end (args);
}

static void
_dbg (const char *format, ...)
{
	va_list args;
	va_start (args, format);
	_do_dbg (format, args);
}

static void
_set_debug ()
{
        if ((getenv ("HALD_VERBOSE")) != NULL)
                is_verbose = TRUE;

        if ((getenv ("HALD_USE_SYSLOG")) != NULL)
                use_syslog = TRUE;
}

#define dbg(format, arg...)							\
	do {									\
		_dbg ("%s:%d: " format "\n", __FILE__, __LINE__, ## arg); \
	} while (0)

/** Drop root privileges: Set the running user id to HAL_USER and
 *  group to HAL_GROUP, and optionally retain auxiliary groups of HAL_USER.
 */
static void
drop_privileges (int keep_auxgroups)
{
	struct passwd *pw = NULL;
	struct group *gr = NULL;

	/* determine user id */
	pw = getpwnam (HAL_USER);
	if (!pw)  {
		dbg ("drop_privileges: user " HAL_USER " does not exist");
		exit (-1);
	}

	/* determine primary group id */
	gr = getgrnam (HAL_GROUP);
	if (!gr) {
		dbg ("drop_privileges: group " HAL_GROUP " does not exist");
		exit (-1);
	}

	if (keep_auxgroups) {
		if (initgroups (HAL_USER, gr->gr_gid)) {
			dbg ("drop_privileges: could not initialize groups");
			exit (-1);
		}
	}

	if (setgid (gr->gr_gid)) {
		dbg ("drop_privileges: could not set group id");
		exit (-1);
	}

	if (setuid (pw->pw_uid)) {
		dbg ("drop_privileges: could not set user id");
		exit (-1);
	}
}

#ifdef __linux__
extern char **environ;
#endif

static char **argv_buffer = NULL;
static size_t argv_size = 0;

static void
hal_set_proc_title_init (int argc, char *argv[])
{
#ifdef __linux__
	unsigned int i;
	char **new_environ, *endptr;

	/* This code is really really ugly. We make some memory layout
	 * assumptions and reuse the environment array as memory to store
	 * our process title in */
	
	for (i = 0; environ[i] != NULL; i++)
		;
	
	endptr = i ? environ[i-1] + strlen (environ[i-1]) : argv[argc-1] + strlen (argv[argc-1]);
	
	argv_buffer = argv;
	argv_size = endptr - argv_buffer[0];
	
	/* Make a copy of environ */
	
	new_environ = malloc (sizeof(char*) * (i + 1));
	for (i = 0; environ[i] != NULL; i++)
		new_environ[i] = strdup (environ[i]);
	new_environ[i] = NULL;
	
	environ = new_environ;
#endif
}

/* this code borrowed from avahi-daemon's setproctitle.c (LGPL v2) */
static void
hal_set_proc_title (const char *format, ...)
{
#ifdef __linux__
	size_t len;
	va_list ap;

	if (argv_buffer == NULL)
		goto out;
		
	va_start (ap, format);
	vsnprintf (argv_buffer[0], argv_size, format, ap);
	va_end (ap);
 	
	len = strlen (argv_buffer[0]);
 	   
	memset (argv_buffer[0] + len, 0, argv_size - len);
	argv_buffer[1] = NULL;
out:
	;
#endif
}

#endif /* SHARED_H */
