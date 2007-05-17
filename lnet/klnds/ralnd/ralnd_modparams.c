/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "ralnd.h"

static int n_connd = 4;
CFS_MODULE_PARM(n_connd, "i", int, 0444,
                "# of connection daemons");

static int min_reconnect_interval = 1;
CFS_MODULE_PARM(min_reconnect_interval, "i", int, 0644,
		"minimum connection retry interval (seconds)");

static int max_reconnect_interval = 60;
CFS_MODULE_PARM(max_reconnect_interval, "i", int, 0644,
		"maximum connection retry interval (seconds)");

static int ntx = 256;
CFS_MODULE_PARM(ntx, "i", int, 0444,
		"# of transmit descriptors");

static int credits = 128;
CFS_MODULE_PARM(credits, "i", int, 0444,
		"# concurrent sends");

static int peer_credits = 32;
CFS_MODULE_PARM(peer_credits, "i", int, 0444,
		"# concurrent sends to 1 peer");

static int fma_cq_size = 8192;
CFS_MODULE_PARM(fma_cq_size, "i", int, 0444,
		"size of the completion queue");

static int timeout = 30;
CFS_MODULE_PARM(timeout, "i", int, 0644,
		"communications timeout (seconds)");

static int max_immediate = (2<<10);
CFS_MODULE_PARM(max_immediate, "i", int, 0644,
		"immediate/RDMA breakpoint");

kra_tunables_t kranal_tunables = {
	.kra_n_connd                = &n_connd,
	.kra_min_reconnect_interval = &min_reconnect_interval,
	.kra_max_reconnect_interval = &max_reconnect_interval,
	.kra_ntx                    = &ntx,
	.kra_credits                = &credits,
	.kra_peercredits            = &peer_credits,
	.kra_fma_cq_size            = &fma_cq_size,
	.kra_timeout                = &timeout,
	.kra_max_immediate          = &max_immediate,
};

#if CONFIG_SYSCTL && !CFS_SYSFS_MODULE_PARM
static ctl_table kranal_ctl_table[] = {
	{1, "n_connd", &n_connd, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{2, "min_reconnect_interval", &min_reconnect_interval, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{3, "max_reconnect_interval", &max_reconnect_interval, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{4, "ntx", &ntx, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{5, "credits", &credits, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{6, "peer_credits", &peer_credits, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{7, "fma_cq_size", &fma_cq_size, 
	 sizeof(int), 0444, NULL, &proc_dointvec},
	{8, "timeout", &timeout, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{9, "max_immediate", &max_immediate, 
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{0}
};

static ctl_table kranal_top_ctl_table[] = {
	{202, "ranal", NULL, 0, 0555, kranal_ctl_table},
	{0}
};

int
kranal_tunables_init ()
{
	kranal_tunables.kra_sysctl =
		cfs_register_sysctl_table(kranal_top_ctl_table, 0);

	if (kranal_tunables.kra_sysctl == NULL)
		CWARN("Can't setup /proc tunables\n");

	return 0;
}

void
kranal_tunables_fini ()
{
	if (kranal_tunables.kra_sysctl != NULL)
		cfs_unregister_sysctl_table(kranal_tunables.kra_sysctl);
}

#else

int
kranal_tunables_init ()
{
	return 0;
}

void
kranal_tunables_fini ()
{
}

#endif

