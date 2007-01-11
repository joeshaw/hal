/***************************************************************************
 * CVSID: $Id$
 *
 * mmap_cache.c : FDI cache routines.
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2006 Kay Sievers, <kay.sievers@vrfy.org>
 * Copyright (C) 2006 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2007 Mikhail Kshevetskiy <mikhail.kshevetskiy@gmail.com>
 * Copyright (C) 2007 Sergey Lapin <slapinid@gmail.com>
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>

#include "logger.h"
#include "rule.h"
#include "mmap_cache.h"

extern void *rules_ptr;

void di_rules_init(void){
	struct cache_header	*header;
	char 			*cachename;
	int			fd;
	struct stat		statbuf;

	cachename = getenv ("HAL_FDI_CACHE_NAME");
	if(!cachename) cachename = CACHE_FILE;

	if((fd = open(cachename, O_RDONLY)) < 0)
		DIE(("Unable to open cache %s\n", cachename));

	if(fstat(fd,&statbuf) < 0)
		DIE(("Unable to stat cache %s\n", cachename));

	rules_ptr = mmap (NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if(rules_ptr == MAP_FAILED)
		DIE (("Couldn't mmap file '%s', errno=%d: %s", cachename, errno, strerror (errno)));

	header = (struct cache_header*) rules_ptr;
	HAL_INFO(("preprobe: offset=%08lx, size=%d", header->fdi_rules_preprobe,
		header->fdi_rules_information - header->fdi_rules_preprobe));
	HAL_INFO(("information: offset=%08lx, size=%d", header->fdi_rules_information,
		header->fdi_rules_policy - header->fdi_rules_information));
	HAL_INFO(("policy: offset=%08lx, size=%d", header->fdi_rules_policy,
		header->all_rules_size - header->fdi_rules_policy));

	close(fd);
}

static void dir_mtime(const char * path, time_t * mt)
{
    struct dirent **namelist;
    struct stat st;
    int n;
    char cpath[PATH_MAX];

    if (!stat(path, &st)) {
	if(st.st_mtime > *mt)
	    *mt = st.st_mtime;

	if(S_ISDIR(st.st_mode)) {
    		n = scandir(path, &namelist, 0, alphasort);
    		if (n < 0)
        		return;
    		else {
            		while(n--) {
#ifdef HAVE_SNPRINTF
			    snprintf(cpath, PATH_MAX, "%s/%s", path, namelist[n]->d_name);
#else
			    sprintf(cpath, "%s/%s", path, namelist[n]->d_name);
#endif
			    if(namelist[n]->d_name[0] != '.')
				    dir_mtime(cpath, mt);

                	    free(namelist[n]);
            	    }
            	    free(namelist);
    		}
	}
    }
}

static void regen_cache(void)
{
    int ret;
    struct stat st;
    char cmd [PATH_MAX + 9];
    char * c;
    
    if((c = getenv("HALD_CACHE_BIN")) == NULL) {
	c = HALD_CACHE_BIN;
    }
#ifdef HAVE_SNPRINTF
    snprintf(cmd, PATH_MAX + 8, "%s --force", c);
#else
    sprintf(cmd, "%s --force", c);
#endif

    if(!stat(c, &st) && (ret = system(cmd)) !=- 1) {

	if(WTERMSIG(ret) == SIGQUIT || WTERMSIG(ret) == SIGINT)
		DIE(("Cache creation was interrupted"));

	if(WEXITSTATUS(ret) == 0)
		HAL_INFO(("Subprocess terminated normally, cache generated."));
    }
    else {
	HAL_INFO(("Unable to generate cache"));
    }
}
void cache_coherency_check(void)
{
 time_t mt;
 struct stat st;
 dir_mtime(PACKAGE_DATA_DIR "/hal/fdi/preprobe", &mt);
 dir_mtime(PACKAGE_SYSCONF_DIR "/hal/fdi/preprobe", &mt);
 dir_mtime(PACKAGE_DATA_DIR "/hal/fdi/information", &mt);
 dir_mtime(PACKAGE_SYSCONF_DIR "/hal/fdi/information", &mt);
 dir_mtime(PACKAGE_DATA_DIR "/hal/fdi/policy", &mt);
 dir_mtime(PACKAGE_SYSCONF_DIR "/hal/fdi/policy", &mt);
 if(!stat(CACHE_FILE, &st)) {
	if(st.st_mtime < mt) {
		HAL_INFO(("Cache needs update"));
		regen_cache();
	}
 }
 else
	regen_cache();

 HAL_INFO(("cache mtime is %d",mt));
}
