/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2015 by Delphix. All rights reserved.
 * Copyright 2015 RackTop Systems.
 * Copyright 2016 Nexenta Systems, Inc.
 */

/*
 * Pool import support functions.
 *
 * To import a pool, we rely on reading the configuration information from the
 * ZFS label of each device.  If we successfully read the label, then we
 * organize the configuration information in the following hierarchy:
 *
 * 	pool guid -> toplevel vdev guid -> label txg
 *
 * Duplicate entries matching this same tuple will be discarded.  Once we have
 * examined every device, we pick the best label txg config for each toplevel
 * vdev.  We then arrange these toplevel vdevs into a complete pool config, and
 * update any paths that have changed.  Finally, we attempt to import the pool
 * using our derived config, and record the results.
 */

#include <ctype.h>
#include <devid.h>
#include <dirent.h>
#include <errno.h>
#include <libintl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/vtoc.h>
#include <sys/dktp/fdisk.h>
#include <sys/efi_partition.h>

#include <sys/vdev_impl.h>
#ifdef HAVE_LIBBLKID
#include <blkid/blkid.h>
#endif

#include "libzfs.h"
#include "libzfs_impl.h"

/*
 * Intermediate structures used to gather configuration information.
 */
typedef struct config_entry {
	uint64_t		ce_txg;
	nvlist_t		*ce_config;
	struct config_entry	*ce_next;
} config_entry_t;

typedef struct vdev_entry {
	uint64_t		ve_guid;
	config_entry_t		*ve_configs;
	struct vdev_entry	*ve_next;
} vdev_entry_t;

typedef struct pool_entry {
	uint64_t		pe_guid;
	vdev_entry_t		*pe_vdevs;
	struct pool_entry	*pe_next;
} pool_entry_t;

typedef struct name_entry {
	char			*ne_name;
	uint64_t		ne_guid;
	uint64_t		ne_order;
	uint64_t		ne_num_labels;
	struct name_entry	*ne_next;
} name_entry_t;

typedef struct pool_list {
	pool_entry_t		*pools;
	name_entry_t		*names;
} pool_list_t;

static char *
get_devid(const char *path)
{
	int fd;
	ddi_devid_t devid;
	char *minor, *ret;

	if ((fd = open(path, O_RDONLY)) < 0)
		return (NULL);

	minor = NULL;
	ret = NULL;
	if (devid_get(fd, &devid) == 0) {
		if (devid_get_minor_name(fd, &minor) == 0)
			ret = devid_str_encode(devid, minor);
		if (minor != NULL)
			devid_str_free(minor);
		devid_free(devid);
	}
	(void) close(fd);

	return (ret);
}


/*
 * Go through and fix up any path and/or devid information for the given vdev
 * configuration.
 */
static int
fix_paths(nvlist_t *nv, name_entry_t *names)
{
	nvlist_t **child;
	uint_t c, children;
	uint64_t guid;
	name_entry_t *ne, *best;
	char *path, *devid;

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) == 0) {
		for (c = 0; c < children; c++)
			if (fix_paths(child[c], names) != 0)
				return (-1);
		return (0);
	}

	/*
	 * This is a leaf (file or disk) vdev.  In either case, go through
	 * the name list and see if we find a matching guid.  If so, replace
	 * the path and see if we can calculate a new devid.
	 *
	 * There may be multiple names associated with a particular guid, in
	 * which case we have overlapping partitions or multiple paths to the
	 * same disk.  In this case we prefer to use the path name which
	 * matches the ZPOOL_CONFIG_PATH.  If no matching entry is found we
	 * use the lowest order device which corresponds to the first match
	 * while traversing the ZPOOL_IMPORT_PATH search path.
	 */
	verify(nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID, &guid) == 0);
	if (nvlist_lookup_string(nv, ZPOOL_CONFIG_PATH, &path) != 0)
		path = NULL;

	best = NULL;
	for (ne = names; ne != NULL; ne = ne->ne_next) {
		if (ne->ne_guid == guid) {

			if (path == NULL) {
				best = ne;
				break;
			}

			if ((strlen(path) == strlen(ne->ne_name)) &&
			    strncmp(path, ne->ne_name, strlen(path)) == 0) {
				best = ne;
				break;
			}

			if (best == NULL) {
				best = ne;
				continue;
			}

			/* Prefer paths with move vdev labels. */
			if (ne->ne_num_labels > best->ne_num_labels) {
				best = ne;
				continue;
			}

			/* Prefer paths earlier in the search order. */
			if (ne->ne_num_labels == best->ne_num_labels &&
			    ne->ne_order < best->ne_order) {
				best = ne;
				continue;
			}
		}
	}

	if (best == NULL)
		return (0);

	if (nvlist_add_string(nv, ZPOOL_CONFIG_PATH, best->ne_name) != 0)
		return (-1);

	if ((devid = get_devid(best->ne_name)) == NULL) {
		(void) nvlist_remove_all(nv, ZPOOL_CONFIG_DEVID);
	} else {
		if (nvlist_add_string(nv, ZPOOL_CONFIG_DEVID, devid) != 0) {
			devid_str_free(devid);
			return (-1);
		}
		devid_str_free(devid);
	}

	return (0);
}

/*
 * Add the given configuration to the list of known devices.
 */
static int
add_config(libzfs_handle_t *hdl, pool_list_t *pl, const char *path,
    int order, int num_labels, nvlist_t *config)
{
	uint64_t pool_guid, vdev_guid, top_guid, txg, state;
	pool_entry_t *pe;
	vdev_entry_t *ve;
	config_entry_t *ce;
	name_entry_t *ne;

	/*
	 * If this is a hot spare not currently in use or level 2 cache
	 * device, add it to the list of names to translate, but don't do
	 * anything else.
	 */
	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_STATE,
	    &state) == 0 &&
	    (state == POOL_STATE_SPARE || state == POOL_STATE_L2CACHE) &&
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_GUID, &vdev_guid) == 0) {
		if ((ne = zfs_alloc(hdl, sizeof (name_entry_t))) == NULL)
			return (-1);

		if ((ne->ne_name = zfs_strdup(hdl, path)) == NULL) {
			free(ne);
			return (-1);
		}
		ne->ne_guid = vdev_guid;
		ne->ne_order = order;
		ne->ne_num_labels = num_labels;
		ne->ne_next = pl->names;
		pl->names = ne;
		return (0);
	}

	/*
	 * If we have a valid config but cannot read any of these fields, then
	 * it means we have a half-initialized label.  In vdev_label_init()
	 * we write a label with txg == 0 so that we can identify the device
	 * in case the user refers to the same disk later on.  If we fail to
	 * create the pool, we'll be left with a label in this state
	 * which should not be considered part of a valid pool.
	 */
	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
	    &pool_guid) != 0 ||
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_GUID,
	    &vdev_guid) != 0 ||
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_TOP_GUID,
	    &top_guid) != 0 ||
	    nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_TXG,
	    &txg) != 0 || txg == 0) {
		nvlist_free(config);
		return (0);
	}

	/*
	 * First, see if we know about this pool.  If not, then add it to the
	 * list of known pools.
	 */
	for (pe = pl->pools; pe != NULL; pe = pe->pe_next) {
		if (pe->pe_guid == pool_guid)
			break;
	}

	if (pe == NULL) {
		if ((pe = zfs_alloc(hdl, sizeof (pool_entry_t))) == NULL) {
			nvlist_free(config);
			return (-1);
		}
		pe->pe_guid = pool_guid;
		pe->pe_next = pl->pools;
		pl->pools = pe;
	}

	/*
	 * Second, see if we know about this toplevel vdev.  Add it if its
	 * missing.
	 */
	for (ve = pe->pe_vdevs; ve != NULL; ve = ve->ve_next) {
		if (ve->ve_guid == top_guid)
			break;
	}

	if (ve == NULL) {
		if ((ve = zfs_alloc(hdl, sizeof (vdev_entry_t))) == NULL) {
			nvlist_free(config);
			return (-1);
		}
		ve->ve_guid = top_guid;
		ve->ve_next = pe->pe_vdevs;
		pe->pe_vdevs = ve;
	}

	/*
	 * Third, see if we have a config with a matching transaction group.  If
	 * so, then we do nothing.  Otherwise, add it to the list of known
	 * configs.
	 */
	for (ce = ve->ve_configs; ce != NULL; ce = ce->ce_next) {
		if (ce->ce_txg == txg)
			break;
	}

	if (ce == NULL) {
		if ((ce = zfs_alloc(hdl, sizeof (config_entry_t))) == NULL) {
			nvlist_free(config);
			return (-1);
		}
		ce->ce_txg = txg;
		ce->ce_config = config;
		ce->ce_next = ve->ve_configs;
		ve->ve_configs = ce;
	} else {
		nvlist_free(config);
	}

	/*
	 * At this point we've successfully added our config to the list of
	 * known configs.  The last thing to do is add the vdev guid -> path
	 * mappings so that we can fix up the configuration as necessary before
	 * doing the import.
	 */
	if ((ne = zfs_alloc(hdl, sizeof (name_entry_t))) == NULL)
		return (-1);

	if ((ne->ne_name = zfs_strdup(hdl, path)) == NULL) {
		free(ne);
		return (-1);
	}

	ne->ne_guid = vdev_guid;
	ne->ne_order = order;
	ne->ne_num_labels = num_labels;
	ne->ne_next = pl->names;
	pl->names = ne;

	return (0);
}

/*
 * Returns true if the named pool matches the given GUID.
 */
static int
pool_active(libzfs_handle_t *hdl, const char *name, uint64_t guid,
    boolean_t *isactive)
{
	zpool_handle_t *zhp;
	uint64_t theguid;

	if (zpool_open_silent(hdl, name, &zhp) != 0)
		return (-1);

	if (zhp == NULL) {
		*isactive = B_FALSE;
		return (0);
	}

	verify(nvlist_lookup_uint64(zhp->zpool_config, ZPOOL_CONFIG_POOL_GUID,
	    &theguid) == 0);

	zpool_close(zhp);

	*isactive = (theguid == guid);
	return (0);
}

static nvlist_t *
refresh_config(libzfs_handle_t *hdl, nvlist_t *config)
{
	nvlist_t *nvl;
	zfs_cmd_t zc = {"\0"};
	int err;

	if (zcmd_write_conf_nvlist(hdl, &zc, config) != 0)
		return (NULL);

	if (zcmd_alloc_dst_nvlist(hdl, &zc,
	    zc.zc_nvlist_conf_size * 2) != 0) {
		zcmd_free_nvlists(&zc);
		return (NULL);
	}

	while ((err = zfs_ioctl(hdl, ZFS_IOC_POOL_TRYIMPORT,
	    &zc)) != 0 && errno == ENOMEM) {
		if (zcmd_expand_dst_nvlist(hdl, &zc) != 0) {
			zcmd_free_nvlists(&zc);
			return (NULL);
		}
	}

	if (err) {
		zcmd_free_nvlists(&zc);
		return (NULL);
	}

	if (zcmd_read_dst_nvlist(hdl, &zc, &nvl) != 0) {
		zcmd_free_nvlists(&zc);
		return (NULL);
	}

	zcmd_free_nvlists(&zc);
	return (nvl);
}

/*
 * Determine if the vdev id is a hole in the namespace.
 */
boolean_t
vdev_is_hole(uint64_t *hole_array, uint_t holes, uint_t id)
{
	int c;

	for (c = 0; c < holes; c++) {

		/* Top-level is a hole */
		if (hole_array[c] == id)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Convert our list of pools into the definitive set of configurations.  We
 * start by picking the best config for each toplevel vdev.  Once that's done,
 * we assemble the toplevel vdevs into a full config for the pool.  We make a
 * pass to fix up any incorrect paths, and then add it to the main list to
 * return to the user.
 */
static nvlist_t *
get_configs(libzfs_handle_t *hdl, pool_list_t *pl, boolean_t active_ok)
{
	pool_entry_t *pe;
	vdev_entry_t *ve;
	config_entry_t *ce;
	nvlist_t *ret = NULL, *config = NULL, *tmp = NULL, *nvtop, *nvroot;
	nvlist_t **spares, **l2cache;
	uint_t i, nspares, nl2cache;
	boolean_t config_seen;
	uint64_t best_txg;
	char *name, *hostname = NULL;
	uint64_t guid;
	uint_t children = 0;
	nvlist_t **child = NULL;
	uint_t holes;
	uint64_t *hole_array, max_id;
	uint_t c;
	boolean_t isactive;
	uint64_t hostid;
	nvlist_t *nvl;
	boolean_t valid_top_config = B_FALSE;

	if (nvlist_alloc(&ret, 0, 0) != 0)
		goto nomem;

	for (pe = pl->pools; pe != NULL; pe = pe->pe_next) {
		uint64_t id, max_txg = 0;

		if (nvlist_alloc(&config, NV_UNIQUE_NAME, 0) != 0)
			goto nomem;
		config_seen = B_FALSE;

		/*
		 * Iterate over all toplevel vdevs.  Grab the pool configuration
		 * from the first one we find, and then go through the rest and
		 * add them as necessary to the 'vdevs' member of the config.
		 */
		for (ve = pe->pe_vdevs; ve != NULL; ve = ve->ve_next) {

			/*
			 * Determine the best configuration for this vdev by
			 * selecting the config with the latest transaction
			 * group.
			 */
			best_txg = 0;
			for (ce = ve->ve_configs; ce != NULL;
			    ce = ce->ce_next) {

				if (ce->ce_txg > best_txg) {
					tmp = ce->ce_config;
					best_txg = ce->ce_txg;
				}
			}

			/*
			 * We rely on the fact that the max txg for the
			 * pool will contain the most up-to-date information
			 * about the valid top-levels in the vdev namespace.
			 */
			if (best_txg > max_txg) {
				(void) nvlist_remove(config,
				    ZPOOL_CONFIG_VDEV_CHILDREN,
				    DATA_TYPE_UINT64);
				(void) nvlist_remove(config,
				    ZPOOL_CONFIG_HOLE_ARRAY,
				    DATA_TYPE_UINT64_ARRAY);

				max_txg = best_txg;
				hole_array = NULL;
				holes = 0;
				max_id = 0;
				valid_top_config = B_FALSE;

				if (nvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_VDEV_CHILDREN, &max_id) == 0) {
					verify(nvlist_add_uint64(config,
					    ZPOOL_CONFIG_VDEV_CHILDREN,
					    max_id) == 0);
					valid_top_config = B_TRUE;
				}

				if (nvlist_lookup_uint64_array(tmp,
				    ZPOOL_CONFIG_HOLE_ARRAY, &hole_array,
				    &holes) == 0) {
					verify(nvlist_add_uint64_array(config,
					    ZPOOL_CONFIG_HOLE_ARRAY,
					    hole_array, holes) == 0);
				}
			}

			if (!config_seen) {
				/*
				 * Copy the relevant pieces of data to the pool
				 * configuration:
				 *
				 *	version
				 *	pool guid
				 *	name
				 *	pool txg (if available)
				 *	comment (if available)
				 *	pool state
				 *	hostid (if available)
				 *	hostname (if available)
				 */
				uint64_t state, version, pool_txg;
				char *comment = NULL;

				version = fnvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_VERSION);
				fnvlist_add_uint64(config,
				    ZPOOL_CONFIG_VERSION, version);
				guid = fnvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_POOL_GUID);
				fnvlist_add_uint64(config,
				    ZPOOL_CONFIG_POOL_GUID, guid);
				name = fnvlist_lookup_string(tmp,
				    ZPOOL_CONFIG_POOL_NAME);
				fnvlist_add_string(config,
				    ZPOOL_CONFIG_POOL_NAME, name);
				if (nvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_POOL_TXG, &pool_txg) == 0)
					fnvlist_add_uint64(config,
					    ZPOOL_CONFIG_POOL_TXG, pool_txg);

				if (nvlist_lookup_string(tmp,
				    ZPOOL_CONFIG_COMMENT, &comment) == 0)
					fnvlist_add_string(config,
					    ZPOOL_CONFIG_COMMENT, comment);

				state = fnvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_POOL_STATE);
				fnvlist_add_uint64(config,
				    ZPOOL_CONFIG_POOL_STATE, state);

				hostid = 0;
				if (nvlist_lookup_uint64(tmp,
				    ZPOOL_CONFIG_HOSTID, &hostid) == 0) {
					fnvlist_add_uint64(config,
					    ZPOOL_CONFIG_HOSTID, hostid);
					hostname = fnvlist_lookup_string(tmp,
					    ZPOOL_CONFIG_HOSTNAME);
					fnvlist_add_string(config,
					    ZPOOL_CONFIG_HOSTNAME, hostname);
				}

				config_seen = B_TRUE;
			}

			/*
			 * Add this top-level vdev to the child array.
			 */
			verify(nvlist_lookup_nvlist(tmp,
			    ZPOOL_CONFIG_VDEV_TREE, &nvtop) == 0);
			verify(nvlist_lookup_uint64(nvtop, ZPOOL_CONFIG_ID,
			    &id) == 0);

			if (id >= children) {
				nvlist_t **newchild;

				newchild = zfs_alloc(hdl, (id + 1) *
				    sizeof (nvlist_t *));
				if (newchild == NULL)
					goto nomem;

				for (c = 0; c < children; c++)
					newchild[c] = child[c];

				free(child);
				child = newchild;
				children = id + 1;
			}
			if (nvlist_dup(nvtop, &child[id], 0) != 0)
				goto nomem;

		}

		/*
		 * If we have information about all the top-levels then
		 * clean up the nvlist which we've constructed. This
		 * means removing any extraneous devices that are
		 * beyond the valid range or adding devices to the end
		 * of our array which appear to be missing.
		 */
		if (valid_top_config) {
			if (max_id < children) {
				for (c = max_id; c < children; c++)
					nvlist_free(child[c]);
				children = max_id;
			} else if (max_id > children) {
				nvlist_t **newchild;

				newchild = zfs_alloc(hdl, (max_id) *
				    sizeof (nvlist_t *));
				if (newchild == NULL)
					goto nomem;

				for (c = 0; c < children; c++)
					newchild[c] = child[c];

				free(child);
				child = newchild;
				children = max_id;
			}
		}

		verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
		    &guid) == 0);

		/*
		 * The vdev namespace may contain holes as a result of
		 * device removal. We must add them back into the vdev
		 * tree before we process any missing devices.
		 */
		if (holes > 0) {
			ASSERT(valid_top_config);

			for (c = 0; c < children; c++) {
				nvlist_t *holey;

				if (child[c] != NULL ||
				    !vdev_is_hole(hole_array, holes, c))
					continue;

				if (nvlist_alloc(&holey, NV_UNIQUE_NAME,
				    0) != 0)
					goto nomem;

				/*
				 * Holes in the namespace are treated as
				 * "hole" top-level vdevs and have a
				 * special flag set on them.
				 */
				if (nvlist_add_string(holey,
				    ZPOOL_CONFIG_TYPE,
				    VDEV_TYPE_HOLE) != 0 ||
				    nvlist_add_uint64(holey,
				    ZPOOL_CONFIG_ID, c) != 0 ||
				    nvlist_add_uint64(holey,
				    ZPOOL_CONFIG_GUID, 0ULL) != 0) {
					nvlist_free(holey);
					goto nomem;
				}
				child[c] = holey;
			}
		}

		/*
		 * Look for any missing top-level vdevs.  If this is the case,
		 * create a faked up 'missing' vdev as a placeholder.  We cannot
		 * simply compress the child array, because the kernel performs
		 * certain checks to make sure the vdev IDs match their location
		 * in the configuration.
		 */
		for (c = 0; c < children; c++) {
			if (child[c] == NULL) {
				nvlist_t *missing;
				if (nvlist_alloc(&missing, NV_UNIQUE_NAME,
				    0) != 0)
					goto nomem;
				if (nvlist_add_string(missing,
				    ZPOOL_CONFIG_TYPE,
				    VDEV_TYPE_MISSING) != 0 ||
				    nvlist_add_uint64(missing,
				    ZPOOL_CONFIG_ID, c) != 0 ||
				    nvlist_add_uint64(missing,
				    ZPOOL_CONFIG_GUID, 0ULL) != 0) {
					nvlist_free(missing);
					goto nomem;
				}
				child[c] = missing;
			}
		}

		/*
		 * Put all of this pool's top-level vdevs into a root vdev.
		 */
		if (nvlist_alloc(&nvroot, NV_UNIQUE_NAME, 0) != 0)
			goto nomem;
		if (nvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE,
		    VDEV_TYPE_ROOT) != 0 ||
		    nvlist_add_uint64(nvroot, ZPOOL_CONFIG_ID, 0ULL) != 0 ||
		    nvlist_add_uint64(nvroot, ZPOOL_CONFIG_GUID, guid) != 0 ||
		    nvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
		    child, children) != 0) {
			nvlist_free(nvroot);
			goto nomem;
		}

		for (c = 0; c < children; c++)
			nvlist_free(child[c]);
		free(child);
		children = 0;
		child = NULL;

		/*
		 * Go through and fix up any paths and/or devids based on our
		 * known list of vdev GUID -> path mappings.
		 */
		if (fix_paths(nvroot, pl->names) != 0) {
			nvlist_free(nvroot);
			goto nomem;
		}

		/*
		 * Add the root vdev to this pool's configuration.
		 */
		if (nvlist_add_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
		    nvroot) != 0) {
			nvlist_free(nvroot);
			goto nomem;
		}
		nvlist_free(nvroot);

		/*
		 * zdb uses this path to report on active pools that were
		 * imported or created using -R.
		 */
		if (active_ok)
			goto add_pool;

		/*
		 * Determine if this pool is currently active, in which case we
		 * can't actually import it.
		 */
		verify(nvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME,
		    &name) == 0);
		verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
		    &guid) == 0);

		if (pool_active(hdl, name, guid, &isactive) != 0)
			goto error;

		if (isactive) {
			nvlist_free(config);
			config = NULL;
			continue;
		}

		if ((nvl = refresh_config(hdl, config)) == NULL) {
			nvlist_free(config);
			config = NULL;
			continue;
		}

		nvlist_free(config);
		config = nvl;

		/*
		 * Go through and update the paths for spares, now that we have
		 * them.
		 */
		verify(nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
		    &nvroot) == 0);
		if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_SPARES,
		    &spares, &nspares) == 0) {
			for (i = 0; i < nspares; i++) {
				if (fix_paths(spares[i], pl->names) != 0)
					goto nomem;
			}
		}

		/*
		 * Update the paths for l2cache devices.
		 */
		if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_L2CACHE,
		    &l2cache, &nl2cache) == 0) {
			for (i = 0; i < nl2cache; i++) {
				if (fix_paths(l2cache[i], pl->names) != 0)
					goto nomem;
			}
		}

		/*
		 * Restore the original information read from the actual label.
		 */
		(void) nvlist_remove(config, ZPOOL_CONFIG_HOSTID,
		    DATA_TYPE_UINT64);
		(void) nvlist_remove(config, ZPOOL_CONFIG_HOSTNAME,
		    DATA_TYPE_STRING);
		if (hostid != 0) {
			verify(nvlist_add_uint64(config, ZPOOL_CONFIG_HOSTID,
			    hostid) == 0);
			verify(nvlist_add_string(config, ZPOOL_CONFIG_HOSTNAME,
			    hostname) == 0);
		}

add_pool:
		/*
		 * Add this pool to the list of configs.
		 */
		verify(nvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME,
		    &name) == 0);
		if (nvlist_add_nvlist(ret, name, config) != 0)
			goto nomem;

		nvlist_free(config);
		config = NULL;
	}

	return (ret);

nomem:
	(void) no_memory(hdl);
error:
	nvlist_free(config);
	nvlist_free(ret);
	for (c = 0; c < children; c++)
		nvlist_free(child[c]);
	free(child);

	return (NULL);
}

/*
 * Return the offset of the given label.
 */
static uint64_t
label_offset(uint64_t size, int l)
{
	ASSERT(P2PHASE_TYPED(size, sizeof (vdev_label_t), uint64_t) == 0);
	return (l * sizeof (vdev_label_t) + (l < VDEV_LABELS / 2 ?
	    0 : size - VDEV_LABELS * sizeof (vdev_label_t)));
}

/*
 * Given a file descriptor, read the label information and return an nvlist
 * describing the configuration, if there is one.
 * Return 0 on success, or -1 on failure
 */
int
zpool_read_label(int fd, nvlist_t **config, int *num_labels)
{
	struct _stat64 statbuf;
	int l, count = 0;
	vdev_label_t *label;
	nvlist_t *expected_config = NULL;
	uint64_t expected_guid = 0, size;

	*config = NULL;

	if (fstat(fd, &statbuf) == -1)
		return (-1);

	size = P2ALIGN_TYPED(statbuf.st_size, sizeof (vdev_label_t), uint64_t);

	if ((label = malloc(sizeof (vdev_label_t))) == NULL)
		return (-1);

	for (l = 0; l < VDEV_LABELS; l++) {
		uint64_t state, guid, txg;

		if (pread(fd, label, sizeof (vdev_label_t),
		    label_offset(size, l)) != sizeof (vdev_label_t))
			continue;

		if (nvlist_unpack(label->vl_vdev_phys.vp_nvlist,
		    sizeof (label->vl_vdev_phys.vp_nvlist), config, 0) != 0)
			continue;

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_GUID,
		    &guid) != 0 || guid == 0) {
			nvlist_free(*config);
			continue;
		}

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_STATE,
		    &state) != 0 || state > POOL_STATE_L2CACHE) {
			nvlist_free(*config);
			continue;
		}

		if (state != POOL_STATE_SPARE && state != POOL_STATE_L2CACHE &&
		    (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_TXG,
		    &txg) != 0 || txg == 0)) {
			nvlist_free(*config);
			continue;
		}

		if (expected_guid) {
			if (expected_guid == guid)
				count++;

			nvlist_free(*config);
		} else {
			expected_config = *config;
			expected_guid = guid;
			count++;
		}
	}

	if (num_labels != NULL)
		*num_labels = count;

	free(label);
	*config = expected_config;

	return (0);
}

int
zpool_read_label_win(HANDLE h, nvlist_t **config, int *num_labels)
{
	int l, count = 0;
	vdev_label_t *label;
	nvlist_t *expected_config = NULL;
	uint64_t expected_guid = 0, size;
	LARGE_INTEGER large;
	uint64_t drivesize;

	*config = NULL;

	drivesize = GetFileDriveSize(h);
	size = P2ALIGN_TYPED(drivesize, sizeof(vdev_label_t), uint64_t);

	if ((label = malloc(sizeof(vdev_label_t))) == NULL)
		return (-1);

	for (l = 0; l < VDEV_LABELS; l++) {
		uint64_t state, guid, txg;

		if (pread_win(h, label, sizeof(vdev_label_t),
			label_offset(size, l)) != sizeof(vdev_label_t))
			continue;

		if (nvlist_unpack(label->vl_vdev_phys.vp_nvlist,
			sizeof(label->vl_vdev_phys.vp_nvlist), config, 0) != 0)
			continue;

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_GUID,
			&guid) != 0 || guid == 0) {
			nvlist_free(*config);
			continue;
		}

		if (nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_STATE,
			&state) != 0 || state > POOL_STATE_L2CACHE) {
			nvlist_free(*config);
			continue;
		}

		if (state != POOL_STATE_SPARE && state != POOL_STATE_L2CACHE &&
			(nvlist_lookup_uint64(*config, ZPOOL_CONFIG_POOL_TXG,
				&txg) != 0 || txg == 0)) {
			nvlist_free(*config);
			continue;
		}

		if (expected_guid) {
			if (expected_guid == guid)
				count++;

			nvlist_free(*config);
		}
		else {
			expected_config = *config;
			expected_guid = guid;
			count++;
		}
	}

	if (num_labels != NULL)
		*num_labels = count;

	free(label);
	*config = expected_config;

	return (0);
}

typedef struct rdsk_node {
	char *rn_name;
	int rn_num_labels;
	int rn_dfd;
	libzfs_handle_t *rn_hdl;
	nvlist_t *rn_config;
	avl_tree_t *rn_avl;
	avl_node_t rn_node;
	boolean_t rn_nozpool;
#ifdef _WIN32
	HANDLE rn_handle;
#endif
} rdsk_node_t;

static int
slice_cache_compare(const void *arg1, const void *arg2)
{
	const char  *nm1 = ((rdsk_node_t *)arg1)->rn_name;
	const char  *nm2 = ((rdsk_node_t *)arg2)->rn_name;
	char *nm1slice, *nm2slice;
	int rv;

	/*
	 * partitions one and three (slices zero and two) are the most
	 * likely to provide results, so put those first
	 */
	nm1slice = strstr(nm1, "part1");
	nm2slice = strstr(nm2, "part1");
	if (nm1slice && !nm2slice) {
		return (-1);
	}
	if (!nm1slice && nm2slice) {
		return (1);
	}
	nm1slice = strstr(nm1, "part3");
	nm2slice = strstr(nm2, "part3");
	if (nm1slice && !nm2slice) {
		return (-1);
	}
	if (!nm1slice && nm2slice) {
		return (1);
	}

	rv = strcmp(nm1, nm2);
	if (rv == 0)
		return (0);
	return (rv > 0 ? 1 : -1);
}

#ifndef __linux__
static void
check_one_slice(avl_tree_t *r, char *diskname, uint_t partno,
    diskaddr_t size, uint_t blksz)
{
	rdsk_node_t tmpnode;
	rdsk_node_t *node;
	char sname[MAXNAMELEN];

	tmpnode.rn_name = &sname[0];
	(void) snprintf(tmpnode.rn_name, MAXNAMELEN, "%s%u",
	    diskname, partno);
	/* too small to contain a zpool? */
	if ((size < (SPA_MINDEVSIZE / blksz)) &&
	    (node = avl_find(r, &tmpnode, NULL)))
		node->rn_nozpool = B_TRUE;
}
#endif

static void
nozpool_all_slices(avl_tree_t *r, const char *sname)
{
#ifndef __linux__
	char diskname[MAXNAMELEN];
	char *ptr;
	int i;

	(void) strncpy(diskname, sname, MAXNAMELEN);
	if (((ptr = strrchr(diskname, 's')) == NULL) &&
	    ((ptr = strrchr(diskname, 'p')) == NULL))
		return;
	ptr[0] = 's';
	ptr[1] = '\0';
	for (i = 0; i < NDKMAP; i++)
		check_one_slice(r, diskname, i, 0, 1);
	ptr[0] = 'p';
	for (i = 0; i <= FD_NUMPART; i++)
		check_one_slice(r, diskname, i, 0, 1);
#endif
}

static void
check_slices(avl_tree_t *r, int fd, const char *sname)
{
#ifdef sun
	struct extvtoc vtoc;
	struct dk_gpt *gpt;
	char diskname[MAXNAMELEN];
	char *ptr;
	int i;

	(void) strncpy(diskname, sname, MAXNAMELEN);
	if ((ptr = strrchr(diskname, 's')) == NULL || !isdigit(ptr[1]))
		return;
	ptr[1] = '\0';

	if (read_extvtoc(fd, &vtoc) >= 0) {
		for (i = 0; i < NDKMAP; i++)
			check_one_slice(r, diskname, i,
			    vtoc.v_part[i].p_size, vtoc.v_sectorsz);
	} else if (efi_alloc_and_read(fd, &gpt) >= 0) {
		/*
		 * on x86 we'll still have leftover links that point
		 * to slices s[9-15], so use NDKMAP instead
		 */
		for (i = 0; i < NDKMAP; i++)
			check_one_slice(r, diskname, i,
			    gpt->efi_parts[i].p_size, gpt->efi_lbasize);
		/* nodes p[1-4] are never used with EFI labels */
		ptr[0] = 'p';
		for (i = 1; i <= FD_NUMPART; i++)
			check_one_slice(r, diskname, i, 0, 1);
		efi_free(gpt);
	}
#endif
}


#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOCDMedia.h>
#include <IOKit/storage/IODVDMedia.h>

/*
 * Given disk2s1, look up "disk2" is IOKit and attempt to determine if
 * it is an optical device.
 */
int is_optical_media(const char *bsdname)
{
	CFMutableDictionaryRef matchingDict;
	int ret = 0;
	io_service_t service, start;
    kern_return_t   kernResult;
    io_iterator_t   iter;

	if ((matchingDict = IOBSDNameMatching(kIOMasterPortDefault, 0, bsdname))  == NULL)
        return(0);

	start = IOServiceGetMatchingService(kIOMasterPortDefault, matchingDict);
	if (IO_OBJECT_NULL == start)
		return (0);

	service = start;

	// Create an iterator across all parents of the service object passed in.
	// since only disk2 would match with ConfirmsTo, and not disk2s1, so
    // we search the parents until we find "Whole", ie, disk2.
	kernResult = IORegistryEntryCreateIterator(service,
                       kIOServicePlane,
                       kIORegistryIterateRecursively | kIORegistryIterateParents,
                       &iter);

	if (KERN_SUCCESS == kernResult) {
        Boolean isWholeMedia = false;
        IOObjectRetain(service);
        do {

			// Lookup "Whole" if we can
			if (IOObjectConformsTo(service, kIOMediaClass)) {
				CFTypeRef wholeMedia;
				wholeMedia = IORegistryEntryCreateCFProperty(service,
													 CFSTR(kIOMediaWholeKey),
                                                     kCFAllocatorDefault,
                                                     0);
				if (wholeMedia) {
					isWholeMedia = CFBooleanGetValue(wholeMedia);
					CFRelease(wholeMedia);
				}
			}

			// If we found "Whole", check the service type.
			if (isWholeMedia &&
				( (IOObjectConformsTo(service, kIOCDMediaClass)) ||
				  (IOObjectConformsTo(service, kIODVDMediaClass)) )) {
				ret = 1; // Is optical, skip
			}

            IOObjectRelease(service);
        } while ((service = IOIteratorNext(iter)) && !isWholeMedia);
        IOObjectRelease(iter);
	}

	IOObjectRelease(start);
	return ret;
}

jmp_buf buffer;

void signal_alarm(int foo)
{
	longjmp(buffer, 1);
}
#endif

static void
zpool_open_func(void *arg)
{
	rdsk_node_t *rn = arg;
#ifdef _WIN32
	struct _stat64 statbuf;
#else
	struct stat64 statbuf;
#endif
	nvlist_t *config;
	int num_labels;
	int fd;
fprintf(stderr, "%s: enter\n", __func__); fflush(stderr);
	if (rn->rn_nozpool)
		return;
#if defined (__linux__) || defined (_WIN32)
	/*
	 * Skip devices with well known prefixes there can be side effects
	 * when opening devices which need to be avoided.
	 *
	 * core     - Symlink to /proc/kcore
	 * fd*      - Floppy interface.
	 * fuse     - Fuse control device.
	 * hpet     - High Precision Event Timer
	 * lp*      - Printer interface.
	 * parport* - Parallel port interface.
	 * ppp      - Generic PPP driver.
	 * random   - Random device
	 * rtc      - Real Time Clock
	 * tty*     - Generic serial interface.
	 * urandom  - Random device.
	 * usbmon*  - USB IO monitor.
	 * vcs*     - Virtual console memory.
	 * watchdog - Watchdog must be closed in a special way.
	 */
	if ((strncmp(rn->rn_name, "core", 4) == 0) ||
	    (strncmp(rn->rn_name, "fd", 2) == 0) ||
	    (strncmp(rn->rn_name, "fuse", 4) == 0) ||
	    (strncmp(rn->rn_name, "hpet", 4) == 0) ||
	    (strncmp(rn->rn_name, "lp", 2) == 0) ||
	    (strncmp(rn->rn_name, "parport", 7) == 0) ||
	    (strncmp(rn->rn_name, "ppp", 3) == 0) ||
	    (strncmp(rn->rn_name, "random", 6) == 0) ||
	    (strncmp(rn->rn_name, "rtc", 3) == 0) ||
	    (strncmp(rn->rn_name, "tty", 3) == 0) ||
	    (strncmp(rn->rn_name, "urandom", 7) == 0) ||
	    (strncmp(rn->rn_name, "usbmon", 6) == 0) ||
	    (strncmp(rn->rn_name, "vcs", 3) == 0) ||
#ifdef __APPLE__
		(strncmp(rn->rn_name, "pty", 3) == 0) || // lots, skip for speed
		(strncmp(rn->rn_name, "com", 3) == 0) || // /dev/com_digidesign_semiface
#endif
	    (strncmp(rn->rn_name, "watchdog", 8) == 0))
		return;

	/*
	 * Ignore failed stats.  We only want regular files and block devices.
	 */
	if (fstatat64(rn->rn_dfd, rn->rn_name, &statbuf, 0) != 0 ||
	    (!S_ISREG(statbuf.st_mode) && !S_ISBLK(statbuf.st_mode)))
		return;

#ifdef __APPLE__
	/* It is desirable to skip optical media as well, as they are
	 * also called /dev/diskX
	 */
	if (is_optical_media((char *)rn->rn_name))
		return;
#endif

	if ((fd = openat64(rn->rn_dfd, rn->rn_name, O_RDONLY)) < 0) {
		/* symlink to a device that's no longer there */
		if (errno == ENOENT)
			nozpool_all_slices(rn->rn_avl, rn->rn_name);
		return;
	}

#else /* LINUX, APPLE -> IllumOS */

	if ((fd = openat64(rn->rn_dfd, rn->rn_name, O_RDONLY)) < 0) {
		/* symlink to a device that's no longer there */
		if (errno == ENOENT)
			nozpool_all_slices(rn->rn_avl, rn->rn_name);
		return;
	}
	/*
	 * Ignore failed stats.  We only want regular
	 * files, character devs and block devs.
	 */
	if (fstat64(fd, &statbuf) != 0 ||
	    (!S_ISREG(statbuf.st_mode) &&
	    !S_ISCHR(statbuf.st_mode) &&
	    !S_ISBLK(statbuf.st_mode))) {
		(void) close(fd);
		return;
	}
#endif
	/* this file is too small to hold a zpool */
	if (S_ISREG(statbuf.st_mode) &&
	    statbuf.st_size < SPA_MINDEVSIZE) {
		(void) close(fd);
		return;
	} else if (!S_ISREG(statbuf.st_mode)) {
		/*
		 * Try to read the disk label first so we don't have to
		 * open a bunch of minor nodes that can't have a zpool.
		 */
		check_slices(rn->rn_avl, fd, rn->rn_name);
	}

#ifdef __APPLE__
	int32_t blksz = 0;
	if (S_ISBLK(statbuf.st_mode) &&
		(ioctl(fd, DKIOCGETBLOCKSIZE, &blksz) || blksz == 0)) {
		if (strncmp(rn->rn_name, "vn", 2) != 0)
			fprintf(stderr, "device '%s' failed to report blocksize -- skipping\r\n",
					rn->rn_name);
		close(fd);
		return;
	}

	struct sigaction sact;
	sigemptyset(&sact.sa_mask);
	sact.sa_flags = 0;
	sact.sa_handler = signal_alarm;
	sigaction(SIGALRM, &sact, NULL);

	if (setjmp(buffer) != 0) {
		printf("ZFS: Warning, timeout reading device '%s'\n", rn->rn_name);
		close(fd);
		return;
	}

	alarm(20);
#endif

	if ((zpool_read_label(fd, &config, &num_labels)) != 0) {
#ifdef __APPLE__
		alarm(0);
#endif
		(void) close(fd);
		(void) no_memory(rn->rn_hdl);
		return;
	}

#ifdef __APPLE__
	alarm(0);
#endif

	if (num_labels == 0) {
		(void) close(fd);
		nvlist_free(config);
		return;
	}

	(void) close(fd);

	rn->rn_config = config;
	rn->rn_num_labels = num_labels;
}

static void
zpool_open_func_win(void *arg)
{
	rdsk_node_t *rn = arg;
	struct _stat64 statbuf;
	nvlist_t *config;
	int num_labels;
	HANDLE fd;
	fprintf(stderr, "%s: enter\n", __func__); fflush(stderr);
	if (rn->rn_nozpool)
		return;
	/*
	* Skip devices with well known prefixes there can be side effects
	* when opening devices which need to be avoided.
	*
	* <insert list of windows ones here>
	* watchdog - Watchdog must be closed in a special way.
	*/
	if ((strncmp(rn->rn_name, "core", 4) == 0) ||
		(strncmp(rn->rn_name, "watchdog", 8) == 0))
		return;

	/*
	* Ignore failed stats.  We only want regular files and block devices.
	*/
	//if (fstatat64(rn->rn_dfd, rn->rn_name, &statbuf, 0) != 0 ||
	//	(!S_ISREG(statbuf.st_mode) && !S_ISBLK(statbuf.st_mode)))
	//	return;

	// Check if this filename is encoded with "#start#len#name"
	if (rn->rn_name[0] == '#') {
		uint64_t offset;
		uint64_t len;
		char *end = NULL;

		offset = strtoull(&rn->rn_name[1], &end, 10);
		while (end && *end == '#') end++;
		len = strtoull(end, &end, 10);
		while (end && *end == '#') end++;
		fd = CreateFile(end,
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL /*| FILE_FLAG_OVERLAPPED*/,
			NULL);
		if (fd == INVALID_HANDLE_VALUE) {
			int error = GetLastError();
			return;
		}
		LARGE_INTEGER place;
		place.QuadPart = offset;
		SetFilePointerEx(fd, place, NULL, FILE_BEGIN); // If it fails, we cant read label


	} else {

		fd = CreateFile(rn->rn_name,
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL /*| FILE_FLAG_OVERLAPPED*/,
			NULL);
		if (fd == INVALID_HANDLE_VALUE) {
			int error = GetLastError();
			return;
		}
	}

	DWORD type = GetFileType(fd);
	fprintf(stderr, "device '%s' filetype %d 0x%x\n", rn->rn_name, type);
	
	type = GetDriveType(rn->rn_name);
	fprintf(stderr, "device '%s' filetype %d 0x%x\n", rn->rn_name, type);
	//if ((fd = openat64(rn->rn_dfd, rn->rn_name, O_RDONLY)) < 0) {
	//	/* symlink to a device that's no longer there */
	//	if (errno == ENOENT)
	//		nozpool_all_slices(rn->rn_avl, rn->rn_name);
	//	return;
	//}

	
	/* this file is too small to hold a zpool */
	if (type == FILE_TYPE_DISK &&
		GetFileDriveSize(fd) < SPA_MINDEVSIZE) {
		CloseHandle(fd);
		return;
	}

//	if (S_ISREG(statbuf.st_mode) &&
//		statbuf.st_size < SPA_MINDEVSIZE) {
//		(void)close(fd);
//		return;
//	}
//	else if (!S_ISREG(statbuf.st_mode)) {
	else if (type != FILE_TYPE_DISK) {
		/*
		* Try to read the disk label first so we don't have to
		* open a bunch of minor nodes that can't have a zpool.
		*/
		check_slices(rn->rn_avl, fd, rn->rn_name);
	}

	if ((zpool_read_label_win(fd, &config, &num_labels)) != 0) {
		CloseHandle(fd);
		(void)no_memory(rn->rn_hdl);
		return;
	}

	if (num_labels == 0) {
		CloseHandle(fd);
		nvlist_free(config);
		return;
	}

	CloseHandle(fd);

	rn->rn_config = config;
	rn->rn_num_labels = num_labels;
}

/*
 * Given a file descriptor, clear (zero) the label information.
 */
int
zpool_clear_label(int fd)
{
	struct _stat64 statbuf;
	int l;
	vdev_label_t *label;
	uint64_t size;

	if (fstat_blk(fd, &statbuf) == -1)
		return (0);
	size = P2ALIGN_TYPED(statbuf.st_size, sizeof (vdev_label_t), uint64_t);

	if ((label = calloc(sizeof (vdev_label_t), 1)) == NULL)
		return (-1);

	for (l = 0; l < VDEV_LABELS; l++) {
		if (pwrite64(fd, label, sizeof (vdev_label_t),
		    label_offset(size, l)) != sizeof (vdev_label_t)) {
			free(label);
			return (-1);
		}
	}

	free(label);
	return (0);
}

#ifdef HAVE_LIBBLKID
/*
 * Use libblkid to quickly search for zfs devices
 */
static int
zpool_find_import_blkid(libzfs_handle_t *hdl, pool_list_t *pools)
{
	blkid_cache cache;
	blkid_dev_iterate iter;
	blkid_dev dev;
	const char *devname;
	nvlist_t *config;
	int fd, err, num_labels;

	err = blkid_get_cache(&cache, NULL);
	if (err != 0) {
		(void) zfs_error_fmt(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "blkid_get_cache() %d"), err);
		goto err_blkid1;
	}

	err = blkid_probe_all(cache);
	if (err != 0) {
		(void) zfs_error_fmt(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "blkid_probe_all() %d"), err);
		goto err_blkid2;
	}

	iter = blkid_dev_iterate_begin(cache);
	if (iter == NULL) {
		(void) zfs_error_fmt(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "blkid_dev_iterate_begin()"));
		goto err_blkid2;
	}

	err = blkid_dev_set_search(iter, "TYPE", "zfs_member");
	if (err != 0) {
		(void) zfs_error_fmt(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "blkid_dev_set_search() %d"), err);
		goto err_blkid3;
	}

	while (blkid_dev_next(iter, &dev) == 0) {
		devname = blkid_dev_devname(dev);
		if ((fd = open(devname, O_RDONLY)) < 0)
			continue;

		err = zpool_read_label(fd, &config, &num_labels);
		(void) close(fd);

		if (err != 0) {
			(void) no_memory(hdl);
			goto err_blkid3;
		}

		if (config != NULL) {
			err = add_config(hdl, pools, devname, 0,
			    num_labels, config);
			if (err != 0)
				goto err_blkid3;
		}
	}

err_blkid3:
	blkid_dev_iterate_end(iter);
err_blkid2:
	blkid_put_cache(cache);
err_blkid1:
	return (err);
}
#endif /* HAVE_LIBBLKID */


char *
zpool_default_import_path[DEFAULT_IMPORT_PATH_SIZE] = {
#ifdef __LINUX__
	"/dev/disk/by-vdev",	/* Custom rules, use first if they exist */
	"/dev/mapper",		/* Use multipath devices before components */
	"/dev/disk/by-uuid",	/* Single unique entry and persistent */
	"/dev/disk/by-id",	/* May be multiple entries and persistent */
	"/dev/disk/by-path",	/* Encodes physical location and persistent */
	"/dev/disk/by-label",	/* Custom persistent labels */
	"/dev"			
#elif defined(_WIN32)
	"/dev"
#endif /* __LINUX__ */
};



/*
 * Given a list of directories to search, find all pools stored on disk.  This
 * includes partial pools which are not available to import.  If no args are
 * given (argc is 0), then the default directory (/dev/dsk) is searched.
 * poolname or guid (but not both) are provided by the caller when trying
 * to import a specific pool.
 */
static nvlist_t *
zpool_find_import_impl(libzfs_handle_t *hdl, importargs_t *iarg)
{
	int i, dirs = iarg->paths;
	struct dirent *dp;
	char path[MAXPATHLEN];
	char *end, **dir = iarg->path;
	size_t pathleft;
	nvlist_t *ret = NULL;
	pool_list_t pools = { 0 };
	pool_entry_t *pe, *penext;
	vdev_entry_t *ve, *venext;
	config_entry_t *ce, *cenext;
	name_entry_t *ne, *nenext;
	avl_tree_t slice_cache;
	rdsk_node_t *slice;
	void *cookie;

	verify(iarg->poolname == NULL || iarg->guid == 0);

	if (dirs == 0) {
#ifdef HAVE_LIBBLKID
		/* Use libblkid to scan all device for their type */
		if (zpool_find_import_blkid(hdl, &pools) == 0)
			goto skip_scanning;

		(void) zfs_error_fmt(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "blkid failure falling back "
		    "to manual probing"));
#endif /* HAVE_LIBBLKID */

		dir = zpool_default_import_path;
		dirs = DEFAULT_IMPORT_PATH_SIZE;
	}

	/*
	 * Go through and read the label configuration information from every
	 * possible device, organizing the information according to pool GUID
	 * and toplevel GUID.
	 */
	for (i = 0; i < dirs; i++) {
		taskq_t *t;
		char rdsk[MAXPATHLEN];
		int dfd;
		boolean_t config_failed = B_FALSE;
		DIR *dirp;

		/* use realpath to normalize the path */
		if (realpath(dir[i], path) == 0) {

			/* it is safe to skip missing search paths */
			if (errno == ENOENT)
				continue;

			zfs_error_aux(hdl, strerror(errno));
			(void) zfs_error_fmt(hdl, EZFS_BADPATH,
			    dgettext(TEXT_DOMAIN, "cannot open '%s'"), dir[i]);
			goto error;
		}
		end = &path[strlen(path)];
		*end++ = '/';
		*end = 0;
		pathleft = &path[sizeof (path)] - end;

		/*
		 * Using raw devices instead of block devices when we're
		 * reading the labels skips a bunch of slow operations during
		 * close(2) processing, so we replace /dev/dsk with /dev/rdsk.
		 */
		if (strcmp(path, ZFS_DISK_ROOTD) == 0)
			(void) strlcpy(rdsk, ZFS_RDISK_ROOTD, sizeof (rdsk));
		else
			(void) strlcpy(rdsk, path, sizeof (rdsk));

		if ((dfd = open(rdsk, O_RDONLY)) < 0 ||
		    (dirp = fdopendir(dfd)) == NULL) {
			if (dfd >= 0)
				(void) close(dfd);
			zfs_error_aux(hdl, strerror(errno));
			(void) zfs_error_fmt(hdl, EZFS_BADPATH,
			    dgettext(TEXT_DOMAIN, "cannot open '%s'"),
			    rdsk);
			goto error;
		}

		avl_create(&slice_cache, slice_cache_compare,
		    sizeof (rdsk_node_t), offsetof(rdsk_node_t, rn_node));

		/*
		 * This is not MT-safe, but we have no MT consumers of libzfs
		 */
		while ((dp = readdir(dirp)) != NULL) {
			const char *name = dp->d_name;
			if (name[0] == '.' &&
			    (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
				continue;

			slice = zfs_alloc(hdl, sizeof (rdsk_node_t));
			slice->rn_name = zfs_strdup(hdl, name);
			slice->rn_avl = &slice_cache;
			slice->rn_dfd = dfd;
			slice->rn_hdl = hdl;
			slice->rn_nozpool = B_FALSE;
			avl_add(&slice_cache, slice);
		}

		/*
		 * create a thread pool to do all of this in parallel;
		 * rn_nozpool is not protected, so this is racy in that
		 * multiple tasks could decide that the same slice can
		 * not hold a zpool, which is benign.  Also choose
		 * double the number of processors; we hold a lot of
		 * locks in the kernel, so going beyond this doesn't
		 * buy us much.
		 */
		t = taskq_create("z_import", 2 * max_ncpus, defclsyspri,
		    2 * max_ncpus, INT_MAX, TASKQ_PREPOPULATE);
		for (slice = avl_first(&slice_cache); slice;
		    (slice = avl_walk(&slice_cache, slice,
		    AVL_AFTER)))
			(void) taskq_dispatch(t, zpool_open_func, slice,
			    TQ_SLEEP);
		taskq_wait(t);
		taskq_destroy(t);

		cookie = NULL;
		while ((slice = avl_destroy_nodes(&slice_cache,
		    &cookie)) != NULL) {
			if (slice->rn_config != NULL && !config_failed) {
				nvlist_t *config = slice->rn_config;
				boolean_t matched = B_TRUE;

				if (iarg->poolname != NULL) {
					char *pname;

					matched = nvlist_lookup_string(config,
					    ZPOOL_CONFIG_POOL_NAME,
					    &pname) == 0 &&
					    strcmp(iarg->poolname, pname) == 0;
				} else if (iarg->guid != 0) {
					uint64_t this_guid;

					matched = nvlist_lookup_uint64(config,
					    ZPOOL_CONFIG_POOL_GUID,
					    &this_guid) == 0 &&
					    iarg->guid == this_guid;
				}
				if (!matched) {
					nvlist_free(config);
				} else {
					/*
					 * use the non-raw path for the config
					 */
					(void) strlcpy(end, slice->rn_name,
					    pathleft);
					if (add_config(hdl, &pools, path, i+1,
					    slice->rn_num_labels, config) != 0)
						config_failed = B_TRUE;
				}
			}
			free(slice->rn_name);
			free(slice);
		}
		avl_destroy(&slice_cache);

		(void) closedir(dirp);

		if (config_failed)
			goto error;
	}

#ifdef HAVE_LIBBLKID
skip_scanning:
#endif
	ret = get_configs(hdl, &pools, iarg->can_be_active);

error:
	for (pe = pools.pools; pe != NULL; pe = penext) {
		penext = pe->pe_next;
		for (ve = pe->pe_vdevs; ve != NULL; ve = venext) {
			venext = ve->ve_next;
			for (ce = ve->ve_configs; ce != NULL; ce = cenext) {
				cenext = ce->ce_next;
				if (ce->ce_config)
					nvlist_free(ce->ce_config);
				free(ce);
			}
			free(ve);
		}
		free(pe);
	}

	for (ne = pools.names; ne != NULL; ne = nenext) {
		nenext = ne->ne_next;
		free(ne->ne_name);
		free(ne);
	}

	return (ret);
}


#ifdef _WIN32
/*
 * Call Windows API to get list of physical disks, and iterate through them
 * finding partitions.
*/
static nvlist_t *
zpool_find_import_win(libzfs_handle_t *hdl, importargs_t *iarg)
{
	int i, dirs = iarg->paths;
	struct dirent *dp;
	char path[MAXPATHLEN];
	char *end, **dir = iarg->path;
	size_t pathleft;
	nvlist_t *ret = NULL;
	pool_list_t pools = { 0 };
	pool_entry_t *pe, *penext;
	vdev_entry_t *ve, *venext;
	config_entry_t *ce, *cenext;
	name_entry_t *ne, *nenext;
	avl_tree_t slice_cache;
	rdsk_node_t *slice;
	void *cookie;

	verify(iarg->poolname == NULL || iarg->guid == 0);

	avl_create(&slice_cache, slice_cache_compare,
		sizeof(rdsk_node_t), offsetof(rdsk_node_t, rn_node));
	/*
	* Go through and read the label configuration information from every
	* possible device, organizing the information according to pool GUID
	* and toplevel GUID.
	*/
	{
		taskq_t *t;
		char rdsk[MAXPATHLEN];
		int dfd;
		boolean_t config_failed = B_FALSE;
		DIR *dirp;

#define _WIN32_MEAN_AND_LEAN
#include <Windows.h>
#include <Setupapi.h>
#include <Ntddstor.h>
#pragma comment( lib, "setupapi.lib" )
		
		/* First, open all raw physical devices */
		HDEVINFO diskClassDevices;
		GUID diskClassDeviceInterfaceGuid = GUID_DEVINTERFACE_DISK;
		SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
		PSP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData;
		DWORD requiredSize;
		DWORD deviceIndex;
		
		HANDLE disk = INVALID_HANDLE_VALUE;
		STORAGE_DEVICE_NUMBER diskNumber;
		DWORD bytesReturned;
		
		diskClassDevices = SetupDiGetClassDevs(&diskClassDeviceInterfaceGuid,
			NULL,
			NULL,
			DIGCF_PRESENT |
			DIGCF_DEVICEINTERFACE);
		//CHK(INVALID_HANDLE_VALUE != diskClassDevices,
		//	"SetupDiGetClassDevs");

		ZeroMemory(&deviceInterfaceData, sizeof(SP_DEVICE_INTERFACE_DATA));
		deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
		deviceIndex = 0;

		while (SetupDiEnumDeviceInterfaces(diskClassDevices,
			NULL,
			&diskClassDeviceInterfaceGuid,
			deviceIndex,
			&deviceInterfaceData)) {

			++deviceIndex;

			SetupDiGetDeviceInterfaceDetail(diskClassDevices,
				&deviceInterfaceData,
				NULL,
				0,
				&requiredSize,
				NULL);
			//CHK(ERROR_INSUFFICIENT_BUFFER == GetLastError(),
			//	"SetupDiGetDeviceInterfaceDetail - 1");

			deviceInterfaceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);
			//CHK(NULL != deviceInterfaceDetailData,
			//	"malloc");

			ZeroMemory(deviceInterfaceDetailData, requiredSize);
			deviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

			SetupDiGetDeviceInterfaceDetail(diskClassDevices,
				&deviceInterfaceData,
				deviceInterfaceDetailData,
				requiredSize,
				NULL,
				NULL);

			// Here, the device path is something like
			// " \\?\ide#diskvmware_virtual_ide_hard_drive___________00000001#5&1778b74b&0&0.0.0#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}"
			// and we create a path like
			// "\\?\PhysicalDrive0"
			// but perhaps it is better to use the full name of the device.
			disk = CreateFile(deviceInterfaceDetailData->DevicePath,
				0/*GENERIC_READ*/,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL /*| FILE_FLAG_OVERLAPPED*/,
				NULL);
			if (disk == INVALID_HANDLE_VALUE)
				continue;

			DeviceIoControl(disk,
				IOCTL_STORAGE_GET_DEVICE_NUMBER,
				NULL,
				0,
				&diskNumber,
				sizeof(STORAGE_DEVICE_NUMBER),
				&bytesReturned,
				NULL);

			fprintf(stderr, "path '%s'\n and '\\\\?\\PhysicalDrive%d'\n", deviceInterfaceDetailData->DevicePath,
				diskNumber.DeviceNumber); fflush(stderr);
			snprintf(rdsk, MAXPATHLEN, "\\\\.\\PHYSICALDRIVE%d", diskNumber.DeviceNumber);

			//CloseHandle(disk);

#if 0
			// This debug code was here to skip the boot disk,
			// but it assumes the first disk is boot, which is wrong.
			if (diskNumber.DeviceNumber == 0) {
				CloseHandle(disk);
				continue;
			}
#endif
			DWORD ior;
			PDRIVE_LAYOUT_INFORMATION_EX partitions;
			DWORD partitionsSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127 * sizeof(PARTITION_INFORMATION_EX);
			partitions = (PDRIVE_LAYOUT_INFORMATION_EX)malloc(partitionsSize);
			if (DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, partitions, partitionsSize, &ior, NULL)) {
				fprintf(stderr, "read partitions ok %d\n", partitions->PartitionCount); fflush(stderr);

				for (int i = 0; i < partitions->PartitionCount; i++) {
					switch (partitions->PartitionEntry[i].PartitionStyle) {
					case PARTITION_STYLE_MBR:
						fprintf(stderr, "    mbr %d: type %x off 0x%llx len 0x%llx\n", i, 
							partitions->PartitionEntry[i].Mbr.PartitionType,
							partitions->PartitionEntry[i].StartingOffset.QuadPart,
							partitions->PartitionEntry[i].PartitionLength.QuadPart); fflush(stderr);
						break;
					case PARTITION_STYLE_GPT:
						fprintf(stderr, "    gpt %d: type %x off 0x%llx len 0x%llx\n", i,
							partitions->PartitionEntry[i].Gpt.PartitionType,
							partitions->PartitionEntry[i].StartingOffset.QuadPart,
							partitions->PartitionEntry[i].PartitionLength.QuadPart); fflush(stderr);
						break;
					}
				}
				free(partitions);
			}
			else {
				fprintf(stderr, "read partitions ng\n"); fflush(stderr);
			}

			CloseHandle(disk);

			slice = zfs_alloc(hdl, sizeof(rdsk_node_t));
			slice->rn_name = zfs_strdup(hdl, deviceInterfaceDetailData->DevicePath);
			slice->rn_avl = &slice_cache;

			slice->rn_hdl = hdl;
			slice->rn_nozpool = B_FALSE;
			avl_add(&slice_cache, slice);


			// Add the whole physical device, but lets also try to read EFI off it.

			disk = CreateFile(deviceInterfaceDetailData->DevicePath,
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL /*| FILE_FLAG_OVERLAPPED*/,
				NULL);

			// On standard OsX created zpool, we expect:
			// offset     name
			// 0x200      MBR partition protective GPT
			// 0x400      EFI partition, s0 as ZFS
			// 0x8410     "version" "name" "testpool" ZFS label

			if (disk != INVALID_HANDLE_VALUE) {
				fprintf(stderr, "asking libefi to read label\n"); fflush(stderr);
				int error;
				struct dk_gpt *vtoc;
				error = efi_alloc_and_read(disk, &vtoc);
				if (error >= 0) {
					fprintf(stderr, "EFI read OK, max partitions %d\n", vtoc->efi_nparts); fflush(stderr);
					for (int i = 0; i < vtoc->efi_nparts; i++) {

						if (vtoc->efi_parts[i].p_start == 0 &&
							vtoc->efi_parts[i].p_size == 0) continue;

						fprintf(stderr, "    part %d:  offset %llx:    len %llx:    tag: %x    name: '%s'\n", i, vtoc->efi_parts[i].p_start, vtoc->efi_parts[i].p_size,
							vtoc->efi_parts[i].p_tag, vtoc->efi_parts[i].p_name); fflush(stderr);
						if (vtoc->efi_parts[i].p_start != 0 &&
							vtoc->efi_parts[i].p_size != 0 &&
							(strstr(vtoc->efi_parts[i].p_name, "ZFS") != NULL || strstr(vtoc->efi_parts[i].p_name, "zfs") != NULL)) {
							// Lets invent a naming scheme with start, and len in it.
							snprintf(rdsk, sizeof(rdsk), "#%llu#%llu#%s",
								vtoc->efi_parts[i].p_start * vtoc->efi_lbasize, vtoc->efi_parts[i].p_size * vtoc->efi_lbasize, deviceInterfaceDetailData->DevicePath);
				
							slice = zfs_alloc(hdl, sizeof(rdsk_node_t));
							slice->rn_name = zfs_strdup(hdl, rdsk);
							slice->rn_avl = &slice_cache;
							slice->rn_hdl = hdl;
							slice->rn_nozpool = B_FALSE;
							avl_add(&slice_cache, slice);
						}
					}
				}
				efi_free(vtoc);
				CloseHandle(disk);
			}
		} // while SetupDiEnumDeviceInterfaces


		/* Now lets iterate the partitions (volumes) */
		HANDLE vol;
		vol = FindFirstVolume(rdsk, sizeof(rdsk));
		while (vol != INVALID_HANDLE_VALUE) {

			// If it ends with a \, we need to eat it.
			char *r;
			r = &rdsk[strlen(rdsk) - 1];
			if (*r == '\\' || *r == '/')
				*r = 0;

			fprintf(stderr, "Processing volume '%s'\n", rdsk); fflush(stderr);

				slice = zfs_alloc(hdl, sizeof(rdsk_node_t));
				slice->rn_name = zfs_strdup(hdl, rdsk);
				slice->rn_avl = &slice_cache;
				slice->rn_hdl = hdl;
				slice->rn_nozpool = B_FALSE;
				avl_add(&slice_cache, slice);

			if (!FindNextVolume(vol, rdsk, sizeof(rdsk))) {
				FindVolumeClose(vol);
				vol = INVALID_HANDLE_VALUE;
			}
		}


		/*
		* create a thread pool to do all of this in parallel;
		* rn_nozpool is not protected, so this is racy in that
		* multiple tasks could decide that the same slice can
		* not hold a zpool, which is benign.  Also choose
		* double the number of processors; we hold a lot of
		* locks in the kernel, so going beyond this doesn't
		* buy us much.
		*/
		t = taskq_create("z_import", 2 * max_ncpus, defclsyspri,
			2 * max_ncpus, INT_MAX, TASKQ_PREPOPULATE);
		for (slice = avl_first(&slice_cache); slice;
			(slice = avl_walk(&slice_cache, slice,
				AVL_AFTER)))
				(void) taskq_dispatch(t, zpool_open_func_win, slice,
					TQ_SLEEP);
		taskq_wait(t);
		taskq_destroy(t);

		cookie = NULL;
		while ((slice = avl_destroy_nodes(&slice_cache,
			&cookie)) != NULL) {
			if (slice->rn_config != NULL && !config_failed) {
				nvlist_t *config = slice->rn_config;
				boolean_t matched = B_TRUE;

				if (iarg->poolname != NULL) {
					char *pname;

					matched = nvlist_lookup_string(config,
						ZPOOL_CONFIG_POOL_NAME,
						&pname) == 0 &&
						strcmp(iarg->poolname, pname) == 0;
				}
				else if (iarg->guid != 0) {
					uint64_t this_guid;

					matched = nvlist_lookup_uint64(config,
						ZPOOL_CONFIG_POOL_GUID,
						&this_guid) == 0 &&
						iarg->guid == this_guid;
				}
				if (!matched) {
					nvlist_free(config);
				}
				else {
					/*
					* use the non-raw path for the config
					*/
//					(void)strlcpy(end, slice->rn_name,
//						pathleft);
					if (add_config(hdl, &pools, /*path*/ slice->rn_name, /*order*/ 1,
						slice->rn_num_labels, config) != 0)
						config_failed = B_TRUE;
				}
			}
			free(slice->rn_name);
			free(slice);
		}
		avl_destroy(&slice_cache);

//		(void)closedir(dirp);

		if (config_failed)
			goto error;
	}

#ifdef HAVE_LIBBLKID
	skip_scanning :
#endif
				  ret = get_configs(hdl, &pools, iarg->can_be_active);

			  error:
				  for (pe = pools.pools; pe != NULL; pe = penext) {
					  penext = pe->pe_next;
					  for (ve = pe->pe_vdevs; ve != NULL; ve = venext) {
						  venext = ve->ve_next;
						  for (ce = ve->ve_configs; ce != NULL; ce = cenext) {
							  cenext = ce->ce_next;
							  if (ce->ce_config)
								  nvlist_free(ce->ce_config);
							  free(ce);
						  }
						  free(ve);
					  }
					  free(pe);
				  }

				  for (ne = pools.names; ne != NULL; ne = nenext) {
					  nenext = ne->ne_next;
					  free(ne->ne_name);
					  free(ne);
				  }

				  return (ret);
}
#endif // WIN32

nvlist_t *
zpool_find_import(libzfs_handle_t *hdl, int argc, char **argv)
{
	importargs_t iarg = { 0 };

	iarg.paths = argc;
	iarg.path = argv;

#ifdef _WIN32
	if (iarg.paths == 0)
		return zpool_find_import_win(hdl, &iarg);
#endif
	return (zpool_find_import_impl(hdl, &iarg));
}

/*
 * Given a cache file, return the contents as a list of importable pools.
 * poolname or guid (but not both) are provided by the caller when trying
 * to import a specific pool.
 */
nvlist_t *
zpool_find_import_cached(libzfs_handle_t *hdl, const char *cachefile,
    char *poolname, uint64_t guid)
{
	char *buf;
	int fd;
	struct _stat64 statbuf;
	nvlist_t *raw, *src, *dst;
	nvlist_t *pools;
	nvpair_t *elem;
	char *name;
	uint64_t this_guid;
	boolean_t active;

	verify(poolname == NULL || guid == 0);

	if ((fd = open(cachefile, O_RDONLY)) < 0) {
		zfs_error_aux(hdl, "%s", strerror(errno));
		(void) zfs_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "failed to open cache file"));
		return (NULL);
	}

	if (fstat(fd, &statbuf) != 0) {
		zfs_error_aux(hdl, "%s", strerror(errno));
		(void) close(fd);
		(void) zfs_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN, "failed to get size of cache file"));
		return (NULL);
	}

	if ((buf = zfs_alloc(hdl, statbuf.st_size)) == NULL) {
		(void) close(fd);
		return (NULL);
	}

	if (read(fd, buf, statbuf.st_size) != statbuf.st_size) {
		(void) close(fd);
		free(buf);
		(void) zfs_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN,
		    "failed to read cache file contents"));
		return (NULL);
	}

	(void) close(fd);

	if (nvlist_unpack(buf, statbuf.st_size, &raw, 0) != 0) {
		free(buf);
		(void) zfs_error(hdl, EZFS_BADCACHE,
		    dgettext(TEXT_DOMAIN,
		    "invalid or corrupt cache file contents"));
		return (NULL);
	}

	free(buf);

	/*
	 * Go through and get the current state of the pools and refresh their
	 * state.
	 */
	if (nvlist_alloc(&pools, 0, 0) != 0) {
		(void) no_memory(hdl);
		nvlist_free(raw);
		return (NULL);
	}

	elem = NULL;
	while ((elem = nvlist_next_nvpair(raw, elem)) != NULL) {
		src = fnvpair_value_nvlist(elem);

		name = fnvlist_lookup_string(src, ZPOOL_CONFIG_POOL_NAME);
		if (poolname != NULL && strcmp(poolname, name) != 0)
			continue;

		this_guid = fnvlist_lookup_uint64(src, ZPOOL_CONFIG_POOL_GUID);
		if (guid != 0 && guid != this_guid)
			continue;

		if (pool_active(hdl, name, this_guid, &active) != 0) {
			nvlist_free(raw);
			nvlist_free(pools);
			return (NULL);
		}

		if (active)
			continue;

		if ((dst = refresh_config(hdl, src)) == NULL) {
			nvlist_free(raw);
			nvlist_free(pools);
			return (NULL);
		}

		if (nvlist_add_nvlist(pools, nvpair_name(elem), dst) != 0) {
			(void) no_memory(hdl);
			nvlist_free(dst);
			nvlist_free(raw);
			nvlist_free(pools);
			return (NULL);
		}
		nvlist_free(dst);
	}

	nvlist_free(raw);
	return (pools);
}

static int
name_or_guid_exists(zpool_handle_t *zhp, void *data)
{
	importargs_t *import = data;
	int found = 0;

	if (import->poolname != NULL) {
		char *pool_name;

		verify(nvlist_lookup_string(zhp->zpool_config,
		    ZPOOL_CONFIG_POOL_NAME, &pool_name) == 0);
		if (strcmp(pool_name, import->poolname) == 0)
			found = 1;
	} else {
		uint64_t pool_guid;

		verify(nvlist_lookup_uint64(zhp->zpool_config,
		    ZPOOL_CONFIG_POOL_GUID, &pool_guid) == 0);
		if (pool_guid == import->guid)
			found = 1;
	}

	zpool_close(zhp);
	return (found);
}

nvlist_t *
zpool_search_import(libzfs_handle_t *hdl, importargs_t *import)
{
	verify(import->poolname == NULL || import->guid == 0);

	if (import->unique)
		import->exists = zpool_iter(hdl, name_or_guid_exists, import);

	if (import->cachefile != NULL)
		return (zpool_find_import_cached(hdl, import->cachefile,
		    import->poolname, import->guid));
#ifdef _WIN32
	if (import->paths == 0)
		return zpool_find_import_win(hdl, import);
#endif
	return (zpool_find_import_impl(hdl, import));
}

boolean_t
find_guid(nvlist_t *nv, uint64_t guid)
{
	uint64_t tmp;
	nvlist_t **child;
	uint_t c, children;

	verify(nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID, &tmp) == 0);
	if (tmp == guid)
		return (B_TRUE);

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) == 0) {
		for (c = 0; c < children; c++)
			if (find_guid(child[c], guid))
				return (B_TRUE);
	}

	return (B_FALSE);
}

typedef struct aux_cbdata {
	const char	*cb_type;
	uint64_t	cb_guid;
	zpool_handle_t	*cb_zhp;
} aux_cbdata_t;

static int
find_aux(zpool_handle_t *zhp, void *data)
{
	aux_cbdata_t *cbp = data;
	nvlist_t **list;
	uint_t i, count;
	uint64_t guid;
	nvlist_t *nvroot;

	verify(nvlist_lookup_nvlist(zhp->zpool_config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) == 0);

	if (nvlist_lookup_nvlist_array(nvroot, cbp->cb_type,
	    &list, &count) == 0) {
		for (i = 0; i < count; i++) {
			verify(nvlist_lookup_uint64(list[i],
			    ZPOOL_CONFIG_GUID, &guid) == 0);
			if (guid == cbp->cb_guid) {
				cbp->cb_zhp = zhp;
				return (1);
			}
		}
	}

	zpool_close(zhp);
	return (0);
}

/*
 * Determines if the pool is in use.  If so, it returns true and the state of
 * the pool as well as the name of the pool.  Both strings are allocated and
 * must be freed by the caller.
 */
int
zpool_in_use(libzfs_handle_t *hdl, int fd, pool_state_t *state, char **namestr,
    boolean_t *inuse)
{
	nvlist_t *config;
	char *name;
	boolean_t ret;
	uint64_t guid, vdev_guid;
	zpool_handle_t *zhp;
	nvlist_t *pool_config;
	uint64_t stateval, isspare;
	aux_cbdata_t cb = { 0 };
	boolean_t isactive;

	*inuse = B_FALSE;

	if (zpool_read_label(fd, &config, NULL) != 0 && errno == ENOMEM) {
		(void) no_memory(hdl);
		return (-1);
	}

	if (config == NULL)
		return (0);

	verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_STATE,
	    &stateval) == 0);
	verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_GUID,
	    &vdev_guid) == 0);

	if (stateval != POOL_STATE_SPARE && stateval != POOL_STATE_L2CACHE) {
		verify(nvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME,
		    &name) == 0);
		verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
		    &guid) == 0);
	}

	switch (stateval) {
	case POOL_STATE_EXPORTED:
		/*
		 * A pool with an exported state may in fact be imported
		 * read-only, so check the in-core state to see if it's
		 * active and imported read-only.  If it is, set
		 * its state to active.
		 */
		if (pool_active(hdl, name, guid, &isactive) == 0 && isactive &&
		    (zhp = zpool_open_canfail(hdl, name)) != NULL) {
			if (zpool_get_prop_int(zhp, ZPOOL_PROP_READONLY, NULL))
				stateval = POOL_STATE_ACTIVE;

			/*
			 * All we needed the zpool handle for is the
			 * readonly prop check.
			 */
			zpool_close(zhp);
		}

		ret = B_TRUE;
		break;

	case POOL_STATE_ACTIVE:
		/*
		 * For an active pool, we have to determine if it's really part
		 * of a currently active pool (in which case the pool will exist
		 * and the guid will be the same), or whether it's part of an
		 * active pool that was disconnected without being explicitly
		 * exported.
		 */
		if (pool_active(hdl, name, guid, &isactive) != 0) {
			nvlist_free(config);
			return (-1);
		}

		if (isactive) {
			/*
			 * Because the device may have been removed while
			 * offlined, we only report it as active if the vdev is
			 * still present in the config.  Otherwise, pretend like
			 * it's not in use.
			 */
			if ((zhp = zpool_open_canfail(hdl, name)) != NULL &&
			    (pool_config = zpool_get_config(zhp, NULL))
			    != NULL) {
				nvlist_t *nvroot;

				verify(nvlist_lookup_nvlist(pool_config,
				    ZPOOL_CONFIG_VDEV_TREE, &nvroot) == 0);
				ret = find_guid(nvroot, vdev_guid);
			} else {
				ret = B_FALSE;
			}

			/*
			 * If this is an active spare within another pool, we
			 * treat it like an unused hot spare.  This allows the
			 * user to create a pool with a hot spare that currently
			 * in use within another pool.  Since we return B_TRUE,
			 * libdiskmgt will continue to prevent generic consumers
			 * from using the device.
			 */
			if (ret && nvlist_lookup_uint64(config,
			    ZPOOL_CONFIG_IS_SPARE, &isspare) == 0 && isspare)
				stateval = POOL_STATE_SPARE;

			if (zhp != NULL)
				zpool_close(zhp);
		} else {
			stateval = POOL_STATE_POTENTIALLY_ACTIVE;
			ret = B_TRUE;
		}
		break;

	case POOL_STATE_SPARE:
		/*
		 * For a hot spare, it can be either definitively in use, or
		 * potentially active.  To determine if it's in use, we iterate
		 * over all pools in the system and search for one with a spare
		 * with a matching guid.
		 *
		 * Due to the shared nature of spares, we don't actually report
		 * the potentially active case as in use.  This means the user
		 * can freely create pools on the hot spares of exported pools,
		 * but to do otherwise makes the resulting code complicated, and
		 * we end up having to deal with this case anyway.
		 */
		cb.cb_zhp = NULL;
		cb.cb_guid = vdev_guid;
		cb.cb_type = ZPOOL_CONFIG_SPARES;
		if (zpool_iter(hdl, find_aux, &cb) == 1) {
			name = (char *)zpool_get_name(cb.cb_zhp);
			ret = B_TRUE;
		} else {
			ret = B_FALSE;
		}
		break;

	case POOL_STATE_L2CACHE:

		/*
		 * Check if any pool is currently using this l2cache device.
		 */
		cb.cb_zhp = NULL;
		cb.cb_guid = vdev_guid;
		cb.cb_type = ZPOOL_CONFIG_L2CACHE;
		if (zpool_iter(hdl, find_aux, &cb) == 1) {
			name = (char *)zpool_get_name(cb.cb_zhp);
			ret = B_TRUE;
		} else {
			ret = B_FALSE;
		}
		break;

	default:
		ret = B_FALSE;
	}


	if (ret) {
		if ((*namestr = zfs_strdup(hdl, name)) == NULL) {
			if (cb.cb_zhp)
				zpool_close(cb.cb_zhp);
			nvlist_free(config);
			return (-1);
		}
		*state = (pool_state_t)stateval;
	}

	if (cb.cb_zhp)
		zpool_close(cb.cb_zhp);

	nvlist_free(config);
	*inuse = ret;
	return (0);
}
