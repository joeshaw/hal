
#ifndef SHARED_H
#define SHARED_H

#include <time.h>
#include <sys/time.h>

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

#endif
