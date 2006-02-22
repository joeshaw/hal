
#ifndef SHARED_H
#define SHARED_H

#include <time.h>
#include <sys/time.h>
#include <pwd.h>
#include <grp.h>

#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

static dbus_bool_t is_verbose = FALSE;

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

#endif /* SHARED_H */
