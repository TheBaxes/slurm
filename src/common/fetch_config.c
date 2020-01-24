/*****************************************************************************\
 *  fetch_config.c - functions for "configless" slurm operation
 *****************************************************************************
 *  Copyright (C) 2020 SchedMD LLC.
 *  Written by Tim Wickberg <tim@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#define _GNU_SOURCE

#include <sys/mman.h>	/* memfd_create */

#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xstring.h"
#include "src/common/xmalloc.h"

extern int fetch_configs(uint32_t flags, config_response_msg_t **configs)
{
	int rc;
	slurm_msg_t req_msg;
	slurm_msg_t resp_msg;
	config_request_msg_t req;
	config_response_msg_t *resp;

	slurm_msg_t_init(&req_msg);
	slurm_msg_t_init(&resp_msg);

	memset(&req, 0, sizeof(req));
	req.flags = flags;
	req_msg.msg_type = REQUEST_CONFIG;
	req_msg.data = &req;

	if (slurm_send_recv_controller_msg(&req_msg, &resp_msg,
					   working_cluster_rec) < 0)
		return SLURM_ERROR;

	switch (resp_msg.msg_type) {
	case RESPONSE_CONFIG:
		resp = (config_response_msg_t *) resp_msg.data;
		break;
	case RESPONSE_SLURM_RC:
		rc = ((return_code_msg_t *) resp_msg.data)->return_code;
		slurm_free_return_code_msg(resp_msg.data);
		slurm_seterrno_ret(rc);
		break;
	default:
		slurm_seterrno_ret(SLURM_UNEXPECTED_MSG_ERROR);
		break;
	}

	*configs = resp;
	return SLURM_SUCCESS;
}

int dump_to_memfd(char *type, char *config, char **filename)
{
#ifdef HAVE_MEMFD_CREATE
	pid_t pid = getpid();

	int fd = memfd_create(type, MFD_CLOEXEC);
	if (fd < 0)
		fatal("%s: failed memfd_create: %m", __func__);

	xfree(*filename);
	xstrfmtcat(*filename, "/proc/%lu/fd/%d", (unsigned long) pid, fd);

	safe_write(fd, config, strlen(config));

	return fd;

rwfail:
	fatal("%s: could not write conf file, likely out of memory", __func__);
	return SLURM_ERROR;
#else
	error("%s: memfd_create() not found at compile time");
	return SLURM_ERROR;
#endif
}

extern void init_minimal_config_server_config(char *config_server)
{
	// if (!config_server) then DNS lookup
	char *conf = NULL, *filename = NULL;
	char *server = xstrdup(config_server);
	char *port = strchr(server, ':');
	int fd;

	if (port) {
		*port = '\0';
		port++;
	}

	xstrfmtcat(conf, "ClusterName=CONFIGLESS\n");
	xstrfmtcat(conf, "SlurmctldHost=%s\n", server);
	if (port)
		xstrfmtcat(conf, "SlurmctldPort=%s\n", port);

	xfree(server); /* do not free port, it is inside this allocation */

	if ((fd = dump_to_memfd("slurm.conf", conf, &filename)) < 0)
		fatal("%s: could not write temporary config", __func__);
	xfree(conf);

	slurm_conf_init(filename);

	close(fd);
}

static int _write_conf(const char *dir, const char *name, const char *content)
{
	char *file = NULL;
	int fd;

	if (!content)
		return SLURM_SUCCESS;

	xstrfmtcat(file, "%s/%s", dir, name);
	if ((fd = open(file, O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC, 0644)) < 0) {
		error("%s: could not open config file `%s`", __func__, file);
		xfree(file);
		return SLURM_ERROR;
	}
	safe_write(fd, content, strlen(content));

	xfree(file);
	return SLURM_SUCCESS;

rwfail:
	error("%s: error writing config to %s: %m", __func__, file);
	xfree(file);
	close(fd);
	return SLURM_ERROR;
}

extern int write_configs_to_config_cache(config_response_msg_t *msg,
					 const char *dir)
{
	if (_write_conf(dir, "slurm.conf", msg->config))
		return SLURM_ERROR;
	if (_write_conf(dir, "acct_gather.conf", msg->acct_gather_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "cgroup.conf", msg->cgroup_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "cgroup_allowed_devices_file.conf",
			msg->cgroup_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "ext_sensors.conf", msg->ext_sensors_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "gres.conf", msg->gres_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "knl_cray.conf", msg->knl_cray_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "knl_generic.conf", msg->knl_generic_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "plugstack.conf", msg->topology_config))
		return SLURM_ERROR;
	if (_write_conf(dir, "topology.conf", msg->topology_config))
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}

static void _load_conf(const char *dir, const char *name, char **target)
{
	char *file = NULL;
	buf_t *config;

	xstrfmtcat(file, "%s/%s", dir, name);
	config = create_mmap_buf(file);
	xfree(file);

	/*
	 * If we can't load a given config, then assume that one isn't required
	 * on this system.
	 */
	if (config)
		*target = xstrndup(config->head, config->size);

	free_buf(config);
}

extern void load_config_response_msg(config_response_msg_t *msg,
				     const char *dir, int flags)
{
	xassert(msg);

	_load_conf(dir, "slurm.conf", &msg->config);
	_load_conf(dir, "acct_gather.conf", &msg->acct_gather_config);
	_load_conf(dir, "cgroup.conf", &msg->cgroup_config);
	_load_conf(dir, "cgroup_allowed_devices_file.conf",
		   &msg->cgroup_allowed_devices_file_config);
	_load_conf(dir, "ext_sensors.conf", &msg->ext_sensors_config);
	_load_conf(dir, "gres.conf", &msg->gres_config);
	_load_conf(dir, "knl_cray.conf", &msg->knl_cray_config);
	_load_conf(dir, "knl_generic.conf", &msg->knl_generic_config);
	_load_conf(dir, "plugstack.conf", &msg->plugstack_config);
	_load_conf(dir, "topology.conf", &msg->topology_config);
}
