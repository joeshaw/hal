/***************************************************************************
 * CVSID: $Id$
 *
 * PCMCIA Card utilities
 *
 * Copyright (C) 2003 Dan Williams <dcbw@redhat.com>
 *
 * Licensed under the Academic Free License version 2.0
 *
 * Most of this code was derived from pcmcia-cs code, originally
 * developed by David A. Hinds <dahinds@users.sourceforge.net>.
 * Portions created by David A. Hinds are Copyright (C) 1999 David A. Hinds.
 * All Rights Reserved.  It has been modified for integration into HAL
 * by Dan Williams and is covered by the GPL version 2 license.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **************************************************************************/

#ifndef PCMCIA_CS_H
#define PCMCIA_CS_H

#include <sys/types.h>

/* cs_types.h */
typedef u_short	ioaddr_t;
typedef u_short	socket_t;
typedef u_int	event_t;
typedef u_char	cisdata_t;
typedef u_short	page_t;

struct client_t;
typedef struct client_t *client_handle_t;

struct window_t;
typedef struct window_t *window_handle_t;

struct region_t;
typedef struct region_t *memory_handle_t;

struct eraseq_t;
typedef struct eraseq_t *eraseq_handle_t;

#ifndef DEV_NAME_LEN
#define DEV_NAME_LEN 32
#endif

typedef char dev_info_t[DEV_NAME_LEN];

/* cs.h */

/* For AccessConfigurationRegister */
typedef struct conf_reg_t {
    u_char	Function;
    u_int	Action;
    off_t	Offset;
    u_int	Value;
} conf_reg_t;

/* Actions */
#define CS_READ		1
#define CS_WRITE	2

/* for AdjustResourceInfo */
typedef struct adjust_t {
    u_int	Action;
    u_int	Resource;
    u_int	Attributes;
    union {
	struct memory {
	    u_long	Base;
	    u_long	Size;
	} memory;
	struct io {
	    ioaddr_t	BasePort;
	    ioaddr_t	NumPorts;
	    u_int	IOAddrLines;
	} io;
	struct irq {
	    u_int	IRQ;
	} irq;
    } resource;
} adjust_t;

typedef struct servinfo_t {
    char	Signature[2];
    u_int	Count;
    u_int	Revision;
    u_int	CSLevel;
    char	*VendorString;
} servinfo_t;

typedef struct event_callback_args_t {
    client_handle_t client_handle;
    void	*info;
    void	*mtdrequest;
    void	*buffer;
    void	*misc;
    void	*client_data;
    struct bus_operations *bus;
} event_callback_args_t;

/* for GetConfigurationInfo */
typedef struct config_info_t {
    u_char	Function;
    u_int	Attributes;
    u_int	Vcc, Vpp1, Vpp2;
    u_int	IntType;
    u_int	ConfigBase;
    u_char	Status, Pin, Copy, Option, ExtStatus;
    u_int	Present;
    u_int	CardValues;
    u_int	AssignedIRQ;
    u_int	IRQAttributes;
    ioaddr_t	BasePort1;
    ioaddr_t	NumPorts1;
    u_int	Attributes1;
    ioaddr_t	BasePort2;
    ioaddr_t	NumPorts2;
    u_int	Attributes2;
    u_int	IOAddrLines;
} config_info_t;

/* For GetFirst/NextClient */
typedef struct client_req_t {
    socket_t	Socket;
    u_int	Attributes;
} client_req_t;

#define CLIENT_THIS_SOCKET	0x01

/* For RegisterClient */
typedef struct client_reg_t {
    dev_info_t	*dev_info;
    u_int	Attributes;
    u_int	EventMask;
    int		(*event_handler)(event_t event, int priority,
				 event_callback_args_t *);
    event_callback_args_t event_callback_args;
    u_int	Version;
} client_reg_t;

/* IntType field */
#define INT_CARDBUS		0x04

/* For GetMemPage, MapMemPage */
typedef struct memreq_t {
    u_int	CardOffset;
    page_t	Page;
} memreq_t;

/* For ModifyWindow */
typedef struct modwin_t {
    u_int	Attributes;
    u_int	AccessSpeed;
} modwin_t;

/* For RequestWindow */
typedef struct win_req_t {
    u_int	Attributes;
    u_long	Base;
    u_int	Size;
    u_int	AccessSpeed;
} win_req_t;

typedef struct cs_status_t {
    u_char	Function;
    event_t 	CardState;
    event_t	SocketState;
} cs_status_t;

typedef struct error_info_t {
    int		func;
    int		retcode;
} error_info_t;

/* Special stuff for binding drivers to sockets */
typedef struct bind_req_t {
    socket_t	Socket;
    u_char	Function;
    dev_info_t	*dev_info;
} bind_req_t;

/* Flag to bind to all functions */
#define BIND_FN_ALL	0xff

typedef struct mtd_bind_t {
    socket_t	Socket;
    u_int	Attributes;
    u_int	CardOffset;
    dev_info_t	*dev_info;
} mtd_bind_t;


/* cistpl.h */

#define CISTPL_VERS_1		0x15
#define CISTPL_MANFID		0x20
#define CISTPL_FUNCID		0x21

typedef struct cistpl_longlink_t {
    u_int	addr;
} cistpl_longlink_t;

typedef struct cistpl_checksum_t {
    u_short	addr;
    u_short	len;
    u_char	sum;
} cistpl_checksum_t;

#define CISTPL_MAX_FUNCTIONS	8

typedef struct cistpl_longlink_mfc_t {
    u_char	nfn;
    struct {
	u_char	space;
	u_int	addr;
    } fn[CISTPL_MAX_FUNCTIONS];
} cistpl_longlink_mfc_t;

#define CISTPL_MAX_ALTSTR_STRINGS	4

typedef struct cistpl_altstr_t {
    u_char	ns;
    u_char	ofs[CISTPL_MAX_ALTSTR_STRINGS];
    char	str[254];
} cistpl_altstr_t;

#define CISTPL_MAX_DEVICES	4

typedef struct cistpl_device_t {
    u_char	ndev;
    struct {
	u_char 	type;
	u_char	wp;
	u_int	speed;
	u_int	size;
    } dev[CISTPL_MAX_DEVICES];
} cistpl_device_t;

typedef struct cistpl_device_o_t {
    u_char		flags;
    cistpl_device_t	device;
} cistpl_device_o_t;

#define CISTPL_VERS_1_MAX_PROD_STRINGS	4

typedef struct cistpl_vers_1_t {
    u_char	major;
    u_char	minor;
    u_char	ns;
    u_char	ofs[CISTPL_VERS_1_MAX_PROD_STRINGS];
    char	str[254];
} cistpl_vers_1_t;

typedef struct cistpl_jedec_t {
    u_char	nid;
    struct {
	u_char	mfr;
	u_char	info;
    } id[CISTPL_MAX_DEVICES];
} cistpl_jedec_t;

typedef struct cistpl_manfid_t {
    u_short	manf;
    u_short	card;
} cistpl_manfid_t;

typedef struct cistpl_funcid_t {
    u_char	func;
    u_char	sysinit;
} cistpl_funcid_t;

typedef struct cistpl_funce_t {
    u_char	type;
    u_char	data[0];
} cistpl_funce_t;

typedef struct cistpl_bar_t {
    u_char	attr;
    u_int	size;
} cistpl_bar_t;

typedef struct cistpl_config_t {
    u_char	last_idx;
    u_int	base;
    u_int	rmask[4];
    u_char	subtuples;
} cistpl_config_t;

typedef struct cistpl_power_t {
    u_char	present;
    u_char	flags;
    u_int	param[7];
} cistpl_power_t;

typedef struct cistpl_timing_t {
    u_int	wait, waitscale;
    u_int	ready, rdyscale;
    u_int	reserved, rsvscale;
} cistpl_timing_t;

#define CISTPL_IO_MAX_WIN	16

typedef struct cistpl_io_t {
    u_char	flags;
    u_char	nwin;
    struct {
	u_int	base;
	u_int	len;
    } win[CISTPL_IO_MAX_WIN];
} cistpl_io_t;

typedef struct cistpl_irq_t {
    u_int	IRQInfo1;
    u_int	IRQInfo2;
} cistpl_irq_t;

#define CISTPL_MEM_MAX_WIN	8

typedef struct cistpl_mem_t {
    u_char	flags;
    u_char	nwin;
    struct {
	u_int	len;
	u_int	card_addr;
	u_int	host_addr;
    } win[CISTPL_MEM_MAX_WIN];
} cistpl_mem_t;

typedef struct cistpl_cftable_entry_t {
    u_char		index;
    u_short		flags;
    u_char		interface;
    cistpl_power_t	vcc, vpp1, vpp2;
    cistpl_timing_t	timing;
    cistpl_io_t		io;
    cistpl_irq_t	irq;
    cistpl_mem_t	mem;
    u_char		subtuples;
} cistpl_cftable_entry_t;

typedef struct cistpl_cftable_entry_cb_t {
    u_char		index;
    u_int		flags;
    cistpl_power_t	vcc, vpp1, vpp2;
    u_char		io;
    cistpl_irq_t	irq;
    u_char		mem;
    u_char		subtuples;
} cistpl_cftable_entry_cb_t;

typedef struct cistpl_device_geo_t {
    u_char		ngeo;
    struct {
	u_char		buswidth;
	u_int		erase_block;
	u_int		read_block;
	u_int		write_block;
	u_int		partition;
	u_int		interleave;
    } geo[CISTPL_MAX_DEVICES];
} cistpl_device_geo_t;

typedef struct cistpl_vers_2_t {
    u_char	vers;
    u_char	comply;
    u_short	dindex;
    u_char	vspec8, vspec9;
    u_char	nhdr;
    u_char	vendor, info;
    char	str[244];
} cistpl_vers_2_t;

typedef struct cistpl_org_t {
    u_char	data_org;
    char	desc[30];
} cistpl_org_t;

typedef struct cistpl_format_t {
    u_char	type;
    u_char	edc;
    u_int	offset;
    u_int	length;
} cistpl_format_t;

typedef union cisparse_t {
    cistpl_device_t		device;
    cistpl_checksum_t		checksum;
    cistpl_longlink_t		longlink;
    cistpl_longlink_mfc_t	longlink_mfc;
    cistpl_vers_1_t		version_1;
    cistpl_altstr_t		altstr;
    cistpl_jedec_t		jedec;
    cistpl_manfid_t		manfid;
    cistpl_funcid_t		funcid;
    cistpl_funce_t		funce;
    cistpl_bar_t		bar;
    cistpl_config_t		config;
    cistpl_cftable_entry_t	cftable_entry;
    cistpl_cftable_entry_cb_t	cftable_entry_cb;
    cistpl_device_geo_t		device_geo;
    cistpl_vers_2_t		vers_2;
    cistpl_org_t		org;
    cistpl_format_t		format;
} cisparse_t;

typedef struct tuple_t {
    u_int	Attributes;
    cisdata_t 	DesiredTuple;
    u_int	Flags;		/* internal use */
    u_int	LinkOffset;	/* internal use */
    u_int	CISOffset;	/* internal use */
    cisdata_t	TupleCode;
    cisdata_t	TupleLink;
    cisdata_t	TupleOffset;
    cisdata_t	TupleDataMax;
    cisdata_t	TupleDataLen;
    cisdata_t	*TupleData;
} tuple_t;

#define TUPLE_RETURN_COMMON	0x02

/* For ValidateCIS */
typedef struct cisinfo_t {
    u_int	Chains;
} cisinfo_t;

#define CISTPL_MAX_CIS_SIZE	0x200

/* For ReplaceCIS */
typedef struct cisdump_t {
    u_int	Length;
    cisdata_t	Data[CISTPL_MAX_CIS_SIZE];
} cisdump_t;

/* bulkmem.h */

typedef struct region_info_t {
    u_int		Attributes;
    u_int		CardOffset;
    u_int		RegionSize;
    u_int		AccessSpeed;
    u_int		BlockSize;
    u_int		PartMultiple;
    u_char		JedecMfr, JedecInfo;
    memory_handle_t	next;
} region_info_t;


/* ds.h */

typedef struct tuple_parse_t {
    tuple_t		tuple;
    cisdata_t		data[255];
    cisparse_t		parse;
} tuple_parse_t;

typedef struct win_info_t {
    window_handle_t	handle;
    win_req_t		window;
    memreq_t		map;
} win_info_t;
    
typedef struct bind_info_t {
    dev_info_t		dev_info;
    u_char		function;
    struct dev_link_t	*instance;
    char		name[DEV_NAME_LEN];
    u_short		major, minor;
    void		*next;
} bind_info_t;

typedef struct mtd_info_t {
    dev_info_t		dev_info;
    u_int		Attributes;
    u_int		CardOffset;
} mtd_info_t;

typedef union ds_ioctl_arg_t {
    servinfo_t		servinfo;
    adjust_t		adjust;
    config_info_t	config;
    tuple_t		tuple;
    tuple_parse_t	tuple_parse;
    client_req_t	client_req;
    cs_status_t		status;
    conf_reg_t		conf_reg;
    cisinfo_t		cisinfo;
    region_info_t	region;
    bind_info_t		bind_info;
    mtd_info_t		mtd_info;
    win_info_t		win_info;
    cisdump_t		cisdump;
} ds_ioctl_arg_t;

#define DS_GET_CONFIGURATION_INFO	_IOWR('d', 3, config_info_t)
#define DS_GET_FIRST_TUPLE		_IOWR('d', 4, tuple_t)
#define DS_GET_TUPLE_DATA		_IOWR('d', 6, tuple_parse_t)
#define DS_PARSE_TUPLE			_IOWR('d', 7, tuple_parse_t)


#endif
