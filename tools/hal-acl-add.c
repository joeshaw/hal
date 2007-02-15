
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <glib.h>

#if 0
static int lock_acl_fd = -1;

static gboolean
lock_hal_acl (void)
{
	if (lock_acl_fd >= 0)
		return TRUE;

	printf ("%d: attempting to get lock on /var/lib/hal/acl-list\n", getpid ());

	lock_acl_fd = open ("/var/lib/hal/acl-list", O_CREAT | O_RDWR);

	if (lock_acl_fd < 0)
		return FALSE;

tryagain:
#if sun
	if (lockf (lock_acl_fd, F_LOCK, 0) != 0) {
#else
	if (flock (lock_acl_fd, LOCK_EX) != 0) {
#endif
		if (errno == EINTR)
			goto tryagain;
		return FALSE;
	}
	
	printf ("%d: got lock on /var/lib/hal/acl-list\n", getpid ());
	
	
	return TRUE;
}
	
static void
unlock_hal_acl (void)
{
#if sun
	lockf (lock_acl_fd, F_ULOCK, 0);
#else
	flock (lock_acl_fd, LOCK_UN);
#endif
	close (lock_acl_fd);
	lock_acl_fd = -1;
	printf ("%d: released lock on /var/lib/hal/acl-list\n", getpid ());
}

/* Each entry here represents a line in the acl-list file 
 *
 *   <device-file>\t<hal-udi>\t<uid-as-number>\t<gid-as-number>\t<session-id>
 *
 * example:
 *
 *   /dev/snd/controlC0\t/org/freedesktop/Hal/devices/pci_8086_27d8_alsa_control__1\t500\t\t/org/freedesktop/ConsoleKit/Session0
 *
 * This means that the 
 */
typedef struct HalACL_s {
	const char *device;
	const char *uid;
	uid_t uid;           /* 0 if unset */
	gid_t gid;           /* 0 if unset */
	const char *session; /* NULL if unset */
} HalACL;

/* hal-acl-grant can run in two modes of operation;
 *
 * 1) as a hal callout via info.callouts.add - this will set ACL's on the device file pointed to by acl.file
 *    using the information provided by the other acl.* properties to determine what ACL's to add to the given
 *    device file
 *
 * 2) 
 *
 */
#endif

int 
main (int argc, char *argv[])
{
	fprintf (stderr, "hal-acl-add\n");
	return 0;
}
