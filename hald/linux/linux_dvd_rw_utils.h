//
// This is part of dvd+rw-tools by Andy Polyakov <appro@fy.chalmers.se>
//
// Use-it-on-your-own-risk, GPL bless...
//
// For further details see http://fy.chalmers.se/~appro/linux/DVD+RW/
//

#ifndef LINUX_DVD_RW_UTILS_H
#define LINUX_DVD_RW_UTILS_H

int get_dvd_r_rw_profile (int fd);
int get_read_write_speed (int fd, int *read_speed, int *write_speed);
int get_disc_type (int fd);
int disc_is_appendable (int fd);
int disc_is_rewritable (int fd);

#endif				/* LINUX_DVD_RW_UTILS_H */
