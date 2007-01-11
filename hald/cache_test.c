/***************************************************************************
 * CVSID: $Id$
 *
 * cache_test.c : test FDI cache.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <dirent.h>
#include <errno.h>
#include <expat.h>
#include <glib.h>
#include <config.h>

#include "logger.h"
#include "rule.h"
#include "mmap_cache.h"

void 	*rules_ptr = NULL;

static void test_cache(u_int32_t offset, size_t size){
    u_int32_t		m = offset;
    struct rule 	*r;

    while(m < offset + size){
	r = (struct rule *) RULES_PTR(m);

	HAL_INFO(("rule=%08lx, rule_size=%d, rtype=%d", m, r->rule_size, r->rtype));
	HAL_INFO(("  jump_position=%08lx", r->jump_position));
	HAL_INFO(("  key='%s', key_len=%d, key_offset=%08lx",
		r->key, r->key_len, m + offsetof(struct rule, key)));
	HAL_INFO(("  value='%s', value_len=%d, value_offset=%08lx",
		(char*)rules_ptr + r->value_offset, r->value_len, r->value_offset));

	m += r->rule_size;
    }
}

int main(){
    struct cache_header	*header;

    cache_coherency_check();
    di_rules_init();
    header = (struct cache_header*) RULES_PTR(0);

    test_cache(header->fdi_rules_preprobe, header->fdi_rules_information - header->fdi_rules_preprobe);
    test_cache(header->fdi_rules_information, header->fdi_rules_policy - header->fdi_rules_information);
    test_cache(header->fdi_rules_policy, header->all_rules_size - header->fdi_rules_policy);
    return 0;
}
