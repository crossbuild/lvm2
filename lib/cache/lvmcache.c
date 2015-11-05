/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvmcache.h"
#include "toolcontext.h"
#include "dev-cache.h"
#include "locking.h"
#include "metadata.h"
#include "memlock.h"
#include "str_list.h"
#include "format-text.h"
#include "format_pool.h"
#include "format1.h"
#include "config.h"

#include "lvmetad.h"

/*

Plan for duplicate PVs

Step 1
------

Step 1 in improving duplicate handling is to remove
the existing duplicate/altdev handling from both lvm
commands and lvmetad.  The effect of this is that
commands and lvmetad use the first device they see
for a PV, and ignore any subsequent devices holding
that same PV.  This is a clean starting point for
rethinking how to handle duplicates.

With step 1 in place, given a duplicate PV pair dev1
and dev2, lvm sees dev1 first and uses it, and sees
dev2 second and ignores it.  dev1 appears to be the
PV and dev2 appears to be a non-PV.  In this condition,
dev2 can still be imported using vgimportclone.

Comparing the lvm behavior before and after step 1,
there are some differences:

1. When warnings about duplicates are printed.
   Before, lvm would print warnings about duplicates
   from any command that looked at them, for as long
   as the duplicates persisted.
   After, commands using lvmetad would not print warnings
   about duplicates.

2. How dev2 looks in 'pvs -a'.
   Before, dev1 and dev2 were both listed with the same
   PV and VG information.  After, the chosen device (dev1)
   is listed as the PV, and the ignored device (dev2) is
   listed like a non-PV.

3. How does 'pvs dev2' look?
   Before, 'pvs dev2' would display dev1, and
   'pvs dev1 dev2' would display both dev1 and dev2
   as the same PV/VG.
   After, 'pvs dev2' prints a "PV not found" error,
   since lvm handled dev2 as a non-PV.  'pvs dev1'
   displays the PV/VG information.

4. Using the ignored duplicate device.
   Before, running pvchange -u or pvcreate on the
   unused duplicate dev resulted in various failures.
   After, running pvcreate on the ignored duplicate
   dev creates a new PV on the device, consistent
   with the fact that lvm considers the ignored
   device a non-PV.

Changes to consider for step 2:

1. Persistent duplicate warnings when using lvmetad.
   This is already partly restored in a more general
   form, and can be expanded to give more specifics.

2. When the ignored duplicate dev2 is listed by pvs -a,
   it should not look the same as dev1, but should have
   some distinct output indicating it's an ignored
   duplicate device.  An attr flag?  I don't think it
   should display the VG name since it's not being used
   in the VG.  Only the device chosen as the PV should
   display the VG name.

3. Similarly, if we want 'pvs dev2' to display something
   other than "not a PV", we should follow the same
   distinct output as above.  We want the output to
   give a clear picture about which device/PV lvm is
   using, and which device lvm is not using.

4. Should we prevent pvcreate on an ignored duplicate
   device?

Step 2
------

Step 2 is a new design that reintroduces a more limited form of handling
duplicate devices.  Before step 1, duplicate handling appeared throughout
lvm in a variety of ways and workarounds.  After step 1, lvm ignored
duplicate devices, treating them as non-PVs.  This functions better, but
makes it hard to know about ignored devices.  In step 2, lvm is aware of
duplicate devices, providing some information about them, but treating
them largely as unusable.

In the new design, duplicate devices are conceptually treated as a special
class of devices.  They are isolated from the core set of metadata used by
lvm, and are not exposed to the core functions of lvm during normal
command processing.  Instances where lvm wants to do something with these
devices are considered special cases.

The first device seen in a duplicate pair is chosen to be the PV.  That
device is used in the core set of metadata, and is used by commands.  If
subsequent devices are found with the same pvid, those devices are added
to the special class of duplicates that is isolated from the core metadata
and not seen during normal processing.

For special cases, when lvm wants to do something with an isolated
duplicate, that device is treated individually, and not mixed in the
metadata or structures where the chosen device for the PV exists.  There
is never a chance that a duplicate pair will conflict during processing.
Once the first device is seen and chosen, that device is the PV and there
is no ambiguity about which device is the PV.

What can be done with duplicates:

1. They can be imported with vgimportclone.
2. They can be displayed with process_each_pv.

1 already works, so changes are focused on supporting 2.

To display duplicate devices with process_each_pv, we'll keep a special
list of duplicate devices in lvmcache or dev-cache.  These are the devices
being ignored by the core lvm metadata and functions.  At the end of
process_each_pv, a new loop will iterate through the list of duplicates
(when the command needs it), to process each entry in isolation.

The list of duplicates in lvmcache is copied from the lvmetad list when
lvmetad is used, or it is populated by lvmcache_add() during scanning when
lvmetad is not used.

If we want to prevent pvcreate from being run on one of the ignored
duplicate devices, then pvcreate would also check if the specified device
is in the list of duplicates.

*/


/*
 * Duplicate PV handling
 *
 * Duplicate PVs exist when lvm sees the same pvid on two different devices.
 *
 * Kinds of duplicates
 * -------------------
 *
 * The two different devices could refer to the same underlying storage, e.g.
 * multipath or a dm identity device wrapper, or they could refer to different
 * underlying storage, e.g. dd one device to another.
 *
 * When multipath is running correct, lvm should not see both underlying paths
 * so duplicates are not seen.
 *
 * When duplicate PVs exist on different underlying storage, the prescribed way
 * of resolving this is to run vgimportclone on one of the devices (generally
 * the new one or the one that's not used).
 *
 * When duplicate PVs exist for the same underlying storage, this should
 * generally not be a persistent condition.  If it is a persistent condition
 * for some reason, then all but one of the duplicate paths should be rejected
 * using the global_filter, keeping the preferred path as the one not removed
 * by the filter.
 *
 * Duplicate PVs are detected by lvm in two places
 * -----------------------------------------------
 *
 * 1. An lvm command performs a full device scan.  Device A with pvid X is
 * added to lvmcache, then device B with the same pvid X is added to lvmcache.
 * When adding B, lvmcache sees that A already exists with the same pvid.
 * lvmcache ignores device B.
 *
 * 2. 'pvscan --cache $dev' is run when the new device appears and generates a
 * uevent.  This command *only* reads the one dev, and sends the information
 * from the dev (PV and VG info) to lvmetad.  lvmetad sees that the pvid
 * already exists but on a different device and ignores the new device.
 *
 * When not using lvmetad, only 1 is possible, but when using lvmetad, both 1
 * and 2 occur.
 *
 * How lvm handles duplicate PVs
 * -----------------------------
 *
 * - When a full scan sees a duplicate device in lvmcache_add(), it prints a
 *   warning, and does not add that device/PV to lvmcache.  It leaves the
 *   previously seen device/PV in lvmcache.  Any PV/VG info from the newly seen
 *   duplicate is ignored.
 *
 * - If the full scan is being done to populate the lvmetad cache, an ignored
 *   device is not sent to lvmetad.
 *
 * - A scan of a single dev (pvscan --cache dev) does not read all devices so
 *   it does not detect duplicates.  But, sending the info from that dev can
 *   cause lvmetad to detect it's a duplicate based on existing devices lvmetad
 *   already knows about.  lvmetad ignores the new device if it has the same
 *   pvid as another existing device.
 *
 * - When a command gets PVs from lvmetad, it does not see any duplicates
 *   because the duplicates were ignored by lvmetad.  This means when a command
 *   adds devices to lvmcache from lvmetad, it will never encounter duplicates.
 *
 * So, both lvmcache and lvmetad ignore a device if its pvid is already known
 * from another device.  The ignored device will look like a non-PV when
 * displayed by pvs -a or when 'pvs $dev' is run on it.  In lvm commands, the
 * PV will appear to exist only on whichever device was seen first.
 *
 * When not using lvmetad, all commands scan all devices and will always see
 * and ignore duplicate devices in lvmcache, using only the first that they
 * happen to find.
 *
 * When using lvmetad, duplicates are seen only by 1. the command scanning all
 * devs to populate lvmetad, and 2. by lvmetad when a new duplicate dev is sent
 * by pvscan --cache dev.  lvmetad does not remember any duplicate devices, and
 * subseqent commands using lvmetad are unaware of the duplicate device that
 * was ignored.
 *
 * When not using lvmetad and a command is scanning all devices, warnings are
 * printed about the duplicates when they are seen and ignored by lvmcache.
 *
 * When lvmetad is used and scanning all devices sees a duplicate, it sets a
 * flag in lvmetad indicating that duplicates have been seen.  Also if lvmetad
 * is sent a duplicate PV from pvscan, it sets this flag also.  When subsequent
 * commands use lvmetad, they check for this flag and print a warning about
 * duplicates if it's set.
 */

#define CACHE_INVALID	0x00000001
#define CACHE_LOCKED	0x00000002

/* One per device */
struct lvmcache_info {
	struct dm_list list;	/* Join VG members together */
	struct dm_list mdas;	/* list head for metadata areas */
	struct dm_list das;	/* list head for data areas */
	struct dm_list bas;	/* list head for bootloader areas */
	struct lvmcache_vginfo *vginfo;	/* NULL == unknown */
	struct label *label;
	const struct format_type *fmt;
	struct device *dev;
	uint64_t device_size;	/* Bytes */
	uint32_t status;
};

/* One per VG */
struct lvmcache_vginfo {
	struct dm_list list;	/* Join these vginfos together */
	struct dm_list infos;	/* List head for lvmcache_infos */
	const struct format_type *fmt;
	char *vgname;		/* "" == orphan */
	uint32_t status;
	char vgid[ID_LEN + 1];
	char _padding[7];
	struct lvmcache_vginfo *next; /* Another VG with same name? */
	char *creation_host;
	char *lock_type;
	uint32_t mda_checksum;
	size_t mda_size;
	size_t vgmetadata_size;
	char *vgmetadata;	/* Copy of VG metadata as format_text string */
	struct dm_config_tree *cft; /* Config tree created from vgmetadata */
				    /* Lifetime is directly tied to vgmetadata */
	struct volume_group *cached_vg;
	unsigned holders;
	unsigned vg_use_count;	/* Counter of vg reusage */
	unsigned precommitted;	/* Is vgmetadata live or precommitted? */
	unsigned cached_vg_invalidated;	/* Signal to regenerate cached_vg */
};

static struct dm_hash_table *_pvid_hash = NULL;
static struct dm_hash_table *_vgid_hash = NULL;
static struct dm_hash_table *_vgname_hash = NULL;
static struct dm_hash_table *_lock_hash = NULL;
static DM_LIST_INIT(_vginfos);
static int _scanning_in_progress = 0;
static int _has_scanned = 0;
static int _vgs_locked = 0;
static int _vg_global_lock_held = 0;	/* Global lock held when cache wiped? */
static int _found_duplicates = 0;

int lvmcache_init(void)
{
	/*
	 * FIXME add a proper lvmcache_locking_reset() that
	 * resets the cache so no previous locks are locked
	 */
	_vgs_locked = 0;

	dm_list_init(&_vginfos);

	if (!(_vgname_hash = dm_hash_create(128)))
		return 0;

	if (!(_vgid_hash = dm_hash_create(128)))
		return 0;

	if (!(_pvid_hash = dm_hash_create(128)))
		return 0;

	if (!(_lock_hash = dm_hash_create(128)))
		return 0;

	/*
	 * Reinitialising the cache clears the internal record of
	 * which locks are held.  The global lock can be held during
	 * this operation so its state must be restored afterwards.
	 */
	if (_vg_global_lock_held) {
		lvmcache_lock_vgname(VG_GLOBAL, 0);
		_vg_global_lock_held = 0;
	}

	return 1;
}

void lvmcache_seed_infos_from_lvmetad(struct cmd_context *cmd)
{
	if (!lvmetad_active() || _has_scanned)
		return;

	if (!lvmetad_pv_list_to_lvmcache(cmd)) {
		stack;
		return;
	}

	_has_scanned = 1;
}

/* Volume Group metadata cache functions */
static void _free_cached_vgmetadata(struct lvmcache_vginfo *vginfo)
{
	if (!vginfo || !vginfo->vgmetadata)
		return;

	dm_free(vginfo->vgmetadata);

	vginfo->vgmetadata = NULL;

	/* Release also cached config tree */
	if (vginfo->cft) {
		dm_config_destroy(vginfo->cft);
		vginfo->cft = NULL;
	}

	log_debug_cache("Metadata cache: VG %s wiped.", vginfo->vgname);

	release_vg(vginfo->cached_vg);
}

/*
 * Cache VG metadata against the vginfo with matching vgid.
 */
static void _store_metadata(struct volume_group *vg, unsigned precommitted)
{
	char uuid[64] __attribute__((aligned(8)));
	struct lvmcache_vginfo *vginfo;
	char *data;
	size_t size;

	if (!(vginfo = lvmcache_vginfo_from_vgid((const char *)&vg->id))) {
		stack;
		return;
	}

	if (!(size = export_vg_to_buffer(vg, &data))) {
		stack;
		_free_cached_vgmetadata(vginfo);
		return;
	}

	/* Avoid reparsing of the same data string */
	if (vginfo->vgmetadata && vginfo->vgmetadata_size == size &&
	    strcmp(vginfo->vgmetadata, data) == 0)
		dm_free(data);
	else {
		_free_cached_vgmetadata(vginfo);
		vginfo->vgmetadata_size = size;
		vginfo->vgmetadata = data;
	}

	vginfo->precommitted = precommitted;

	if (!id_write_format((const struct id *)vginfo->vgid, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_debug_cache("Metadata cache: VG %s (%s) stored (%" PRIsize_t " bytes%s).",
			vginfo->vgname, uuid, size,
			precommitted ? ", precommitted" : "");
}

static void _update_cache_info_lock_state(struct lvmcache_info *info,
					  int locked,
					  int *cached_vgmetadata_valid)
{
	int was_locked = (info->status & CACHE_LOCKED) ? 1 : 0;

	/*
	 * Cache becomes invalid whenever lock state changes unless
	 * exclusive VG_GLOBAL is held (i.e. while scanning).
	 */
	if (!lvmcache_vgname_is_locked(VG_GLOBAL) && (was_locked != locked)) {
		info->status |= CACHE_INVALID;
		*cached_vgmetadata_valid = 0;
	}

	if (locked)
		info->status |= CACHE_LOCKED;
	else
		info->status &= ~CACHE_LOCKED;
}

static void _update_cache_vginfo_lock_state(struct lvmcache_vginfo *vginfo,
					    int locked)
{
	struct lvmcache_info *info;
	int cached_vgmetadata_valid = 1;

	dm_list_iterate_items(info, &vginfo->infos)
		_update_cache_info_lock_state(info, locked,
					      &cached_vgmetadata_valid);

	if (!cached_vgmetadata_valid)
		_free_cached_vgmetadata(vginfo);
}

static void _update_cache_lock_state(const char *vgname, int locked)
{
	struct lvmcache_vginfo *vginfo;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return;

	_update_cache_vginfo_lock_state(vginfo, locked);
}

static void _drop_metadata(const char *vgname, int drop_precommitted)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return;

	/*
	 * Invalidate cached PV labels.
	 * If cached precommitted metadata exists that means we
	 * already invalidated the PV labels (before caching it)
	 * and we must not do it again.
	 */
	if (!drop_precommitted && vginfo->precommitted && !vginfo->vgmetadata)
		log_error(INTERNAL_ERROR "metadata commit (or revert) missing before "
			  "dropping metadata from cache.");

	if (drop_precommitted || !vginfo->precommitted)
		dm_list_iterate_items(info, &vginfo->infos)
			info->status |= CACHE_INVALID;

	_free_cached_vgmetadata(vginfo);

	/* VG revert */
	if (drop_precommitted)
		vginfo->precommitted = 0;
}

/*
 * Remote node uses this to upgrade precommitted metadata to commited state
 * when receives vg_commit notification.
 * (Note that devices can be suspended here, if so, precommitted metadata are already read.)
 */
void lvmcache_commit_metadata(const char *vgname)
{
	struct lvmcache_vginfo *vginfo;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return;

	if (vginfo->precommitted) {
		log_debug_cache("Precommitted metadata cache: VG %s upgraded to committed.",
				vginfo->vgname);
		vginfo->precommitted = 0;
	}
}

void lvmcache_drop_metadata(const char *vgname, int drop_precommitted)
{
	if (lvmcache_vgname_is_locked(VG_GLOBAL) && !vg_write_lock_held())
		return;

	/* For VG_ORPHANS, we need to invalidate all labels on orphan PVs. */
	if (!strcmp(vgname, VG_ORPHANS)) {
		_drop_metadata(FMT_TEXT_ORPHAN_VG_NAME, 0);
		_drop_metadata(FMT_LVM1_ORPHAN_VG_NAME, 0);
		_drop_metadata(FMT_POOL_ORPHAN_VG_NAME, 0);

		/* Indicate that PVs could now be missing from the cache */
		init_full_scan_done(0);
	} else
		_drop_metadata(vgname, drop_precommitted);
}

/*
 * Ensure vgname2 comes after vgname1 alphabetically.
 * Orphan locks come last.
 * VG_GLOBAL comes first.
 */
static int _vgname_order_correct(const char *vgname1, const char *vgname2)
{
	if (is_global_vg(vgname1))
		return 1;

	if (is_global_vg(vgname2))
		return 0;

	if (is_orphan_vg(vgname1))
		return 0;

	if (is_orphan_vg(vgname2))
		return 1;

	if (strcmp(vgname1, vgname2) < 0)
		return 1;

	return 0;
}

/*
 * Ensure VG locks are acquired in alphabetical order.
 */
int lvmcache_verify_lock_order(const char *vgname)
{
	struct dm_hash_node *n;
	const char *vgname2;

	if (!_lock_hash)
		return_0;

	dm_hash_iterate(n, _lock_hash) {
		if (!dm_hash_get_data(_lock_hash, n))
			return_0;

		if (!(vgname2 = dm_hash_get_key(_lock_hash, n))) {
			log_error(INTERNAL_ERROR "VG lock %s hits NULL.",
				 vgname);
			return 0;
		}

		if (!_vgname_order_correct(vgname2, vgname)) {
			log_errno(EDEADLK, INTERNAL_ERROR "VG lock %s must "
				  "be requested before %s, not after.",
				  vgname, vgname2);
			return 0;
		}
	}

	return 1;
}

void lvmcache_lock_vgname(const char *vgname, int read_only __attribute__((unused)))
{
	if (!_lock_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		return;
	}

	if (dm_hash_lookup(_lock_hash, vgname))
		log_error(INTERNAL_ERROR "Nested locking attempted on VG %s.",
			  vgname);

	if (!dm_hash_insert(_lock_hash, vgname, (void *) 1))
		log_error("Cache locking failure for %s", vgname);

	if (strcmp(vgname, VG_GLOBAL)) {
		_update_cache_lock_state(vgname, 1);
		_vgs_locked++;
	}
}

int lvmcache_vgname_is_locked(const char *vgname)
{
	if (!_lock_hash)
		return 0;

	return dm_hash_lookup(_lock_hash, is_orphan_vg(vgname) ? VG_ORPHANS : vgname) ? 1 : 0;
}

void lvmcache_unlock_vgname(const char *vgname)
{
	if (!dm_hash_lookup(_lock_hash, vgname))
		log_error(INTERNAL_ERROR "Attempt to unlock unlocked VG %s.",
			  vgname);

	if (strcmp(vgname, VG_GLOBAL))
		_update_cache_lock_state(vgname, 0);

	dm_hash_remove(_lock_hash, vgname);

	/* FIXME Do this per-VG */
	if (strcmp(vgname, VG_GLOBAL) && !--_vgs_locked)
		dev_close_all();
}

int lvmcache_vgs_locked(void)
{
	return _vgs_locked;
}

static void _vginfo_attach_info(struct lvmcache_vginfo *vginfo,
				struct lvmcache_info *info)
{
	if (!vginfo)
		return;

	info->vginfo = vginfo;
	dm_list_add(&vginfo->infos, &info->list);
}

static void _vginfo_detach_info(struct lvmcache_info *info)
{
	if (!dm_list_empty(&info->list)) {
		dm_list_del(&info->list);
		dm_list_init(&info->list);
	}

	info->vginfo = NULL;
}

/* If vgid supplied, require a match. */
struct lvmcache_vginfo *lvmcache_vginfo_from_vgname(const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;

	if (!vgname)
		return lvmcache_vginfo_from_vgid(vgid);

	if (!_vgname_hash) {
		log_debug_cache(INTERNAL_ERROR "Internal cache is no yet initialized.");
		return NULL;
	}

	if (!(vginfo = dm_hash_lookup(_vgname_hash, vgname))) {
		log_debug_cache("Metadata cache has no info for vgname: \"%s\"", vgname);
		return NULL;
	}

	if (vgid)
		do
			if (!strncmp(vgid, vginfo->vgid, ID_LEN))
				return vginfo;
		while ((vginfo = vginfo->next));

	if  (!vginfo)
		log_debug_cache("Metadata cache has not found vgname \"%s\" with vgid \"%."
				DM_TO_STRING(ID_LEN) "s\".", vgname, vgid ? : "");

	return vginfo;
}

const struct format_type *lvmcache_fmt_from_vgname(struct cmd_context *cmd,
						   const char *vgname, const char *vgid,
						   unsigned revalidate_labels)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	struct label *label;
	struct dm_list *devh, *tmp;
	struct dm_list devs;
	struct device_list *devl;
	struct volume_group *vg;
	const struct format_type *fmt;
	char vgid_found[ID_LEN + 1] __attribute__((aligned(8)));

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		if (!lvmetad_active())
			return NULL; /* too bad */
		/* If we don't have the info but we have lvmetad, we can ask
		 * there before failing. */
		if ((vg = lvmetad_vg_lookup(cmd, vgname, vgid))) {
			fmt = vg->fid->fmt;
			release_vg(vg);
			return fmt;
		}
		return NULL;
	}

	/*
	 * If this function is called repeatedly, only the first one needs to revalidate.
	 */
	if (!revalidate_labels)
		goto out;

	/*
	 * This function is normally called before reading metadata so
 	 * we check cached labels here. Unfortunately vginfo is volatile.
 	 */
	dm_list_init(&devs);
	dm_list_iterate_items(info, &vginfo->infos) {
		if (!(devl = dm_malloc(sizeof(*devl)))) {
			log_error("device_list element allocation failed");
			return NULL;
		}
		devl->dev = info->dev;
		dm_list_add(&devs, &devl->list);
	}

	memcpy(vgid_found, vginfo->vgid, sizeof(vgid_found));

	dm_list_iterate_safe(devh, tmp, &devs) {
		devl = dm_list_item(devh, struct device_list);
		(void) label_read(devl->dev, &label, UINT64_C(0));
		dm_list_del(&devl->list);
		dm_free(devl);
	}

	/* If vginfo changed, caller needs to rescan */
	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid_found)) ||
	    strncmp(vginfo->vgid, vgid_found, ID_LEN))
		return NULL;

out:
	return vginfo->fmt;
}

struct lvmcache_vginfo *lvmcache_vginfo_from_vgid(const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	char id[ID_LEN + 1] __attribute__((aligned(8)));

	if (!_vgid_hash || !vgid) {
		log_debug_cache(INTERNAL_ERROR "Internal cache cannot lookup vgid.");
		return NULL;
	}

	/* vgid not necessarily NULL-terminated */
	strncpy(&id[0], vgid, ID_LEN);
	id[ID_LEN] = '\0';

	if (!(vginfo = dm_hash_lookup(_vgid_hash, id))) {
		log_debug_cache("Metadata cache has no info for vgid \"%s\"", id);
		return NULL;
	}

	return vginfo;
}

const char *lvmcache_vgname_from_vgid(struct dm_pool *mem, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	const char *vgname = NULL;

	if ((vginfo = lvmcache_vginfo_from_vgid(vgid)))
		vgname = vginfo->vgname;

	if (mem && vgname)
		return dm_pool_strdup(mem, vgname);

	return vgname;
}

static int _info_is_valid(struct lvmcache_info *info)
{
	if (info->status & CACHE_INVALID)
		return 0;

	/*
	 * The caller must hold the VG lock to manipulate metadata.
	 * In a cluster, remote nodes sometimes read metadata in the
	 * knowledge that the controlling node is holding the lock.
	 * So if the VG appears to be unlocked here, it should be safe
	 * to use the cached value.
	 */
	if (info->vginfo && !lvmcache_vgname_is_locked(info->vginfo->vgname))
		return 1;

	if (!(info->status & CACHE_LOCKED))
		return 0;

	return 1;
}

static int _vginfo_is_valid(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_info *info;

	/* Invalid if any info is invalid */
	dm_list_iterate_items(info, &vginfo->infos)
		if (!_info_is_valid(info))
			return 0;

	return 1;
}

/* vginfo is invalid if it does not contain at least one valid info */
static int _vginfo_is_invalid(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_info *info;

	dm_list_iterate_items(info, &vginfo->infos)
		if (_info_is_valid(info))
			return 0;

	return 1;
}

/*
 * If valid_only is set, data will only be returned if the cached data is
 * known still to be valid.
 */
struct lvmcache_info *lvmcache_info_from_pvid(const char *pvid, int valid_only)
{
	struct lvmcache_info *info;
	char id[ID_LEN + 1] __attribute__((aligned(8)));

	if (!_pvid_hash || !pvid)
		return NULL;

	strncpy(&id[0], pvid, ID_LEN);
	id[ID_LEN] = '\0';

	if (!(info = dm_hash_lookup(_pvid_hash, id)))
		return NULL;

	if (valid_only && !_info_is_valid(info))
		return NULL;

	return info;
}

const char *lvmcache_vgname_from_info(struct lvmcache_info *info)
{
	if (info->vginfo)
		return info->vginfo->vgname;
	return NULL;
}

char *lvmcache_vgname_from_pvid(struct cmd_context *cmd, const char *pvid)
{
	struct lvmcache_info *info;
	char *vgname;

	if (!lvmcache_device_from_pvid(cmd, (const struct id *)pvid, NULL, NULL)) {
		log_error("Couldn't find device with uuid %s.", pvid);
		return NULL;
	}

	info = lvmcache_info_from_pvid(pvid, 0);
	if (!info)
		return_NULL;

	if (!(vgname = dm_pool_strdup(cmd->mem, info->vginfo->vgname))) {
		log_errno(ENOMEM, "vgname allocation failed");
		return NULL;
	}
	return vgname;
}

static void _rescan_entry(struct lvmcache_info *info)
{
	struct label *label;

	if (info->status & CACHE_INVALID)
		(void) label_read(info->dev, &label, UINT64_C(0));
}

static int _scan_invalid(void)
{
	dm_hash_iter(_pvid_hash, (dm_hash_iterate_fn) _rescan_entry);

	return 1;
}

int lvmcache_label_scan(struct cmd_context *cmd, int full_scan)
{
	struct label *label;
	struct dev_iter *iter;
	struct device *dev;
	struct format_type *fmt;

	int r = 0;

	if (lvmetad_active())
		return 1;

	/* Avoid recursion when a PVID can't be found! */
	if (_scanning_in_progress)
		return 0;

	_scanning_in_progress = 1;

	if (!_vgname_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		goto out;
	}

	if (_has_scanned && !full_scan) {
		r = _scan_invalid();
		goto out;
	}

	if (full_scan == 2 && (cmd->full_filter && !cmd->full_filter->use_count) && !refresh_filters(cmd))
		goto_out;

	if (!cmd->full_filter || !(iter = dev_iter_create(cmd->full_filter, (full_scan == 2) ? 1 : 0))) {
		log_error("dev_iter creation failed");
		goto out;
	}

	while ((dev = dev_iter_get(iter)))
		(void) label_read(dev, &label, UINT64_C(0));

	dev_iter_destroy(iter);

	_has_scanned = 1;

	/* Perform any format-specific scanning e.g. text files */
	if (cmd->independent_metadata_areas)
		dm_list_iterate_items(fmt, &cmd->formats)
			if (fmt->ops->scan && !fmt->ops->scan(fmt, NULL))
				goto out;

	/*
	 * If we are a long-lived process, write out the updated persistent
	 * device cache for the benefit of short-lived processes.
	 */
	if (full_scan == 2 && cmd->is_long_lived &&
	    cmd->dump_filter && cmd->full_filter && cmd->full_filter->dump &&
	    !cmd->full_filter->dump(cmd->full_filter, 0))
		stack;

	r = 1;

      out:
	_scanning_in_progress = 0;

	return r;
}

struct volume_group *lvmcache_get_vg(struct cmd_context *cmd, const char *vgname,
				     const char *vgid, unsigned precommitted)
{
	struct lvmcache_vginfo *vginfo;
	struct volume_group *vg = NULL;
	struct format_instance *fid;
	struct format_instance_ctx fic;

	/*
	 * We currently do not store precommitted metadata in lvmetad at
	 * all. This means that any request for precommitted metadata is served
	 * using the classic scanning mechanics, and read from disk or from
	 * lvmcache.
	 */
	if (lvmetad_active() && !precommitted) {
		/* Still serve the locally cached VG if available */
		if (vgid && (vginfo = lvmcache_vginfo_from_vgid(vgid)) &&
		    vginfo->vgmetadata && (vg = vginfo->cached_vg))
			goto out;
		return lvmetad_vg_lookup(cmd, vgname, vgid);
	}

	if (!vgid || !(vginfo = lvmcache_vginfo_from_vgid(vgid)) || !vginfo->vgmetadata)
		return NULL;

	if (!_vginfo_is_valid(vginfo))
		return NULL;

	/*
	 * Don't return cached data if either:
	 * (i)  precommitted metadata is requested but we don't have it cached
	 *      - caller should read it off disk;
	 * (ii) live metadata is requested but we have precommitted metadata cached
	 *      and no devices are suspended so caller may read it off disk.
	 *
	 * If live metadata is requested but we have precommitted metadata cached
	 * and devices are suspended, we assume this precommitted metadata has
	 * already been preloaded and committed so it's OK to return it as live.
	 * Note that we do not clear the PRECOMMITTED flag.
	 */
	if ((precommitted && !vginfo->precommitted) ||
	    (!precommitted && vginfo->precommitted && !critical_section()))
		return NULL;

	/* Use already-cached VG struct when available */
	if ((vg = vginfo->cached_vg) && !vginfo->cached_vg_invalidated)
		goto out;

	release_vg(vginfo->cached_vg);

	fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = vginfo->vgname;
	fic.context.vg_ref.vg_id = vgid;
	if (!(fid = vginfo->fmt->ops->create_instance(vginfo->fmt, &fic)))
		return_NULL;

	/* Build config tree from vgmetadata, if not yet cached */
	if (!vginfo->cft &&
	    !(vginfo->cft =
	      dm_config_from_string(vginfo->vgmetadata)))
		goto_bad;

	if (!(vg = import_vg_from_config_tree(vginfo->cft, fid)))
		goto_bad;

	/* Cache VG struct for reuse */
	vginfo->cached_vg = vg;
	vginfo->holders = 1;
	vginfo->vg_use_count = 0;
	vginfo->cached_vg_invalidated = 0;
	vg->vginfo = vginfo;

	if (!dm_pool_lock(vg->vgmem, detect_internal_vg_cache_corruption()))
		goto_bad;

out:
	vginfo->holders++;
	vginfo->vg_use_count++;
	log_debug_cache("Using cached %smetadata for VG %s with %u holder(s).",
			vginfo->precommitted ? "pre-committed " : "",
			vginfo->vgname, vginfo->holders);

	return vg;

bad:
	_free_cached_vgmetadata(vginfo);
	return NULL;
}

// #if 0
int lvmcache_vginfo_holders_dec_and_test_for_zero(struct lvmcache_vginfo *vginfo)
{
	log_debug_cache("VG %s decrementing %d holder(s) at %p.",
			vginfo->cached_vg->name, vginfo->holders, vginfo->cached_vg);

	if (--vginfo->holders)
		return 0;

	if (vginfo->vg_use_count > 1)
		log_debug_cache("VG %s reused %d times.",
				vginfo->cached_vg->name, vginfo->vg_use_count);

	/* Debug perform crc check only when it's been used more then once */
	if (!dm_pool_unlock(vginfo->cached_vg->vgmem,
			    detect_internal_vg_cache_corruption() &&
			    (vginfo->vg_use_count > 1)))
		stack;

	vginfo->cached_vg->vginfo = NULL;
	vginfo->cached_vg = NULL;

	return 1;
}
// #endif

int lvmcache_get_vgnameids(struct cmd_context *cmd, int include_internal,
			   struct dm_list *vgnameids)
{
	struct vgnameid_list *vgnl;
	struct lvmcache_vginfo *vginfo;

	lvmcache_label_scan(cmd, 0);

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (!include_internal && is_orphan_vg(vginfo->vgname))
			continue;

		if (!(vgnl = dm_pool_alloc(cmd->mem, sizeof(*vgnl)))) {
			log_error("vgnameid_list allocation failed.");
			return 0;
		}

		vgnl->vgid = dm_pool_strdup(cmd->mem, vginfo->vgid);
		vgnl->vg_name = dm_pool_strdup(cmd->mem, vginfo->vgname);

		if (!vgnl->vgid || !vgnl->vg_name) {
			log_error("vgnameid_list member allocation failed.");
			return 0;
		}

		dm_list_add(vgnameids, &vgnl->list);
	}

	return 1;
}

struct dm_list *lvmcache_get_vgids(struct cmd_context *cmd,
				   int include_internal)
{
	struct dm_list *vgids;
	struct lvmcache_vginfo *vginfo;

	// TODO plug into lvmetad here automagically?
	lvmcache_label_scan(cmd, 0);

	if (!(vgids = str_list_create(cmd->mem))) {
		log_error("vgids list allocation failed");
		return NULL;
	}

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (!include_internal && is_orphan_vg(vginfo->vgname))
			continue;

		if (!str_list_add(cmd->mem, vgids,
				  dm_pool_strdup(cmd->mem, vginfo->vgid))) {
			log_error("strlist allocation failed");
			return NULL;
		}
	}

	return vgids;
}

struct dm_list *lvmcache_get_vgnames(struct cmd_context *cmd,
				     int include_internal)
{
	struct dm_list *vgnames;
	struct lvmcache_vginfo *vginfo;

	lvmcache_label_scan(cmd, 0);

	if (!(vgnames = str_list_create(cmd->mem))) {
		log_errno(ENOMEM, "vgnames list allocation failed");
		return NULL;
	}

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (!include_internal && is_orphan_vg(vginfo->vgname))
			continue;

		if (!str_list_add(cmd->mem, vgnames,
				  dm_pool_strdup(cmd->mem, vginfo->vgname))) {
			log_errno(ENOMEM, "strlist allocation failed");
			return NULL;
		}
	}

	return vgnames;
}

struct dm_list *lvmcache_get_pvids(struct cmd_context *cmd, const char *vgname,
				const char *vgid)
{
	struct dm_list *pvids;
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	if (!(pvids = str_list_create(cmd->mem))) {
		log_error("pvids list allocation failed");
		return NULL;
	}

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid)))
		return pvids;

	dm_list_iterate_items(info, &vginfo->infos) {
		if (!str_list_add(cmd->mem, pvids,
				  dm_pool_strdup(cmd->mem, info->dev->pvid))) {
			log_error("strlist allocation failed");
			return NULL;
		}
	}

	return pvids;
}

static struct device *_device_from_pvid(const struct id *pvid,
					uint64_t *label_sector)
{
	struct lvmcache_info *info;
	struct label *label;

	if ((info = lvmcache_info_from_pvid((const char *) pvid, 0))) {
		if (lvmetad_active()) {
			if (info->label && label_sector)
				*label_sector = info->label->sector;
			return info->dev;
		}

		if (label_read(info->dev, &label, UINT64_C(0))) {
			info = (struct lvmcache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid)) {
				if (label_sector)
					*label_sector = label->sector;
				return info->dev;
                        }
		}
	}
	return NULL;
}

struct device *lvmcache_device_from_pvid(struct cmd_context *cmd, const struct id *pvid,
				unsigned *scan_done_once, uint64_t *label_sector)
{
	struct device *dev;

	/* Already cached ? */
	dev = _device_from_pvid(pvid, label_sector);
	if (dev)
		return dev;

	lvmcache_label_scan(cmd, 0);

	/* Try again */
	dev = _device_from_pvid(pvid, label_sector);
	if (dev)
		return dev;

	if (critical_section() || (scan_done_once && *scan_done_once))
		return NULL;

	lvmcache_label_scan(cmd, 2);
	if (scan_done_once)
		*scan_done_once = 1;

	/* Try again */
	dev = _device_from_pvid(pvid, label_sector);
	if (dev)
		return dev;

	return NULL;
}

const char *lvmcache_pvid_from_devname(struct cmd_context *cmd,
			      const char *devname)
{
	struct device *dev;
	struct label *label;

	if (!(dev = dev_cache_get(devname, cmd->filter))) {
		log_error("%s: Couldn't find device.  Check your filters?",
			  devname);
		return NULL;
	}

	if (!(label_read(dev, &label, UINT64_C(0))))
		return NULL;

	return dev->pvid;
}


static int _free_vginfo(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_vginfo *primary_vginfo, *vginfo2;
	int r = 1;

	_free_cached_vgmetadata(vginfo);

	vginfo2 = primary_vginfo = lvmcache_vginfo_from_vgname(vginfo->vgname, NULL);

	if (vginfo == primary_vginfo) {
		dm_hash_remove(_vgname_hash, vginfo->vgname);
		if (vginfo->next && !dm_hash_insert(_vgname_hash, vginfo->vgname,
						    vginfo->next)) {
			log_error("_vgname_hash re-insertion for %s failed",
				  vginfo->vgname);
			r = 0;
		}
	} else
		while (vginfo2) {
			if (vginfo2->next == vginfo) {
				vginfo2->next = vginfo->next;
				break;
			}
			vginfo2 = vginfo2->next;
		}

	dm_free(vginfo->vgname);
	dm_free(vginfo->creation_host);

	if (*vginfo->vgid && _vgid_hash &&
	    lvmcache_vginfo_from_vgid(vginfo->vgid) == vginfo)
		dm_hash_remove(_vgid_hash, vginfo->vgid);

	dm_list_del(&vginfo->list);

	dm_free(vginfo);

	return r;
}

/*
 * vginfo must be info->vginfo unless info is NULL
 */
static int _drop_vginfo(struct lvmcache_info *info, struct lvmcache_vginfo *vginfo)
{
	if (info)
		_vginfo_detach_info(info);

	/* vginfo still referenced? */
	if (!vginfo || is_orphan_vg(vginfo->vgname) ||
	    !dm_list_empty(&vginfo->infos))
		return 1;

	if (!_free_vginfo(vginfo))
		return_0;

	return 1;
}

void lvmcache_del(struct lvmcache_info *info)
{
	if (info->dev->pvid[0] && _pvid_hash)
		dm_hash_remove(_pvid_hash, info->dev->pvid);

	_drop_vginfo(info, info->vginfo);

	info->label->labeller->ops->destroy_label(info->label->labeller,
						  info->label);
	dm_free(info);

	return;
}

static int _lvmcache_update_pvid(struct lvmcache_info *info, const char *pvid)
{
	/*
	 * Nothing to do if already stored with same pvid.
	 */

	if (((dm_hash_lookup(_pvid_hash, pvid)) == info) &&
	    !strcmp(info->dev->pvid, pvid))
		return 1;
	if (*info->dev->pvid)
		dm_hash_remove(_pvid_hash, info->dev->pvid);
	strncpy(info->dev->pvid, pvid, sizeof(info->dev->pvid));
	if (!dm_hash_insert(_pvid_hash, pvid, info)) {
		log_error("_lvmcache_update: pvid insertion failed: %s", pvid);
		return 0;
	}

	return 1;
}

/*
 * vginfo must be info->vginfo unless info is NULL (orphans)
 */
static int _lvmcache_update_vgid(struct lvmcache_info *info,
				 struct lvmcache_vginfo *vginfo,
				 const char *vgid)
{
	if (!vgid || !vginfo ||
	    !strncmp(vginfo->vgid, vgid, ID_LEN))
		return 1;

	if (vginfo && *vginfo->vgid)
		dm_hash_remove(_vgid_hash, vginfo->vgid);
	if (!vgid) {
		/* FIXME: unreachable code path */
		log_debug_cache("lvmcache: %s: clearing VGID", info ? dev_name(info->dev) : vginfo->vgname);
		return 1;
	}

	strncpy(vginfo->vgid, vgid, ID_LEN);
	vginfo->vgid[ID_LEN] = '\0';
	if (!dm_hash_insert(_vgid_hash, vginfo->vgid, vginfo)) {
		log_error("_lvmcache_update: vgid hash insertion failed: %s",
			  vginfo->vgid);
		return 0;
	}

	if (!is_orphan_vg(vginfo->vgname))
		log_debug_cache("lvmcache: %s: setting %s VGID to %s",
				(info) ? dev_name(info->dev) : "",
				vginfo->vgname, vginfo->vgid);

	return 1;
}

static int _insert_vginfo(struct lvmcache_vginfo *new_vginfo, const char *vgid,
			  uint32_t vgstatus, const char *creation_host,
			  struct lvmcache_vginfo *primary_vginfo)
{
	struct lvmcache_vginfo *last_vginfo = primary_vginfo;
	char uuid_primary[64] __attribute__((aligned(8)));
	char uuid_new[64] __attribute__((aligned(8)));
	int use_new = 0;

	/* Pre-existing VG takes precedence. Unexported VG takes precedence. */
	if (primary_vginfo) {
		if (!id_write_format((const struct id *)vgid, uuid_new, sizeof(uuid_new)))
			return_0;

		if (!id_write_format((const struct id *)&primary_vginfo->vgid, uuid_primary,
				     sizeof(uuid_primary)))
			return_0;

		/*
		 * If   Primary not exported, new exported => keep
		 * Else Primary exported, new not exported => change
		 * Else Primary has hostname for this machine => keep
		 * Else Primary has no hostname, new has one => change
		 * Else New has hostname for this machine => change
		 * Else Keep primary.
		 */
		if (!(primary_vginfo->status & EXPORTED_VG) &&
		    (vgstatus & EXPORTED_VG))
			log_warn("WARNING: Duplicate VG name %s: "
				 "Existing %s takes precedence over "
				 "exported %s", new_vginfo->vgname,
				 uuid_primary, uuid_new);
		else if ((primary_vginfo->status & EXPORTED_VG) &&
			   !(vgstatus & EXPORTED_VG)) {
			log_warn("WARNING: Duplicate VG name %s: "
				 "%s takes precedence over exported %s",
				 new_vginfo->vgname, uuid_new,
				 uuid_primary);
			use_new = 1;
		} else if (primary_vginfo->creation_host &&
			   !strcmp(primary_vginfo->creation_host,
				   primary_vginfo->fmt->cmd->hostname))
			log_warn("WARNING: Duplicate VG name %s: "
				 "Existing %s (created here) takes precedence "
				 "over %s", new_vginfo->vgname, uuid_primary,
				 uuid_new);
		else if (!primary_vginfo->creation_host && creation_host) {
			log_warn("WARNING: Duplicate VG name %s: "
				 "%s (with creation_host) takes precedence over %s",
				 new_vginfo->vgname, uuid_new,
				 uuid_primary);
			use_new = 1;
		} else if (creation_host &&
			   !strcmp(creation_host,
				   primary_vginfo->fmt->cmd->hostname)) {
			log_warn("WARNING: Duplicate VG name %s: "
				 "%s (created here) takes precedence over %s",
				 new_vginfo->vgname, uuid_new,
				 uuid_primary);
			use_new = 1;
		}

		if (!use_new) {
			while (last_vginfo->next)
				last_vginfo = last_vginfo->next;
			last_vginfo->next = new_vginfo;
			return 1;
		}

		dm_hash_remove(_vgname_hash, primary_vginfo->vgname);
	}

	if (!dm_hash_insert(_vgname_hash, new_vginfo->vgname, new_vginfo)) {
		log_error("cache_update: vg hash insertion failed: %s",
		  	new_vginfo->vgname);
		return 0;
	}

	if (primary_vginfo)
		new_vginfo->next = primary_vginfo;

	return 1;
}

static int _lvmcache_update_vgname(struct lvmcache_info *info,
				   const char *vgname, const char *vgid,
				   uint32_t vgstatus, const char *creation_host,
				   const struct format_type *fmt)
{
	struct lvmcache_vginfo *vginfo, *primary_vginfo, *orphan_vginfo;
	struct lvmcache_info *info2, *info3;
	char mdabuf[32];
	// struct lvmcache_vginfo  *old_vginfo, *next;

	if (!vgname || (info && info->vginfo && !strcmp(info->vginfo->vgname, vgname)))
		return 1;

	/* Remove existing vginfo entry */
	if (info)
		_drop_vginfo(info, info->vginfo);

	/* Get existing vginfo or create new one */
	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
/*** FIXME - vginfo ends up duplicated instead of renamed.
		// Renaming?  This lookup fails.
		if ((vginfo = vginfo_from_vgid(vgid))) {
			next = vginfo->next;
			old_vginfo = vginfo_from_vgname(vginfo->vgname, NULL);
			if (old_vginfo == vginfo) {
				dm_hash_remove(_vgname_hash, old_vginfo->vgname);
				if (old_vginfo->next) {
					if (!dm_hash_insert(_vgname_hash, old_vginfo->vgname, old_vginfo->next)) {
						log_error("vg hash re-insertion failed: %s",
							  old_vginfo->vgname);
						return 0;
					}
				}
			} else do {
				if (old_vginfo->next == vginfo) {
					old_vginfo->next = vginfo->next;
					break;
				}
			} while ((old_vginfo = old_vginfo->next));
			vginfo->next = NULL;

			dm_free(vginfo->vgname);
			if (!(vginfo->vgname = dm_strdup(vgname))) {
				log_error("cache vgname alloc failed for %s", vgname);
				return 0;
			}

			// Rename so can assume new name does not already exist
			if (!dm_hash_insert(_vgname_hash, vginfo->vgname, vginfo->next)) {
				log_error("vg hash re-insertion failed: %s",
					  vginfo->vgname);
		      		return 0;
			}
		} else {
***/
		if (!(vginfo = dm_zalloc(sizeof(*vginfo)))) {
			log_error("lvmcache_update_vgname: list alloc failed");
			return 0;
		}
		if (!(vginfo->vgname = dm_strdup(vgname))) {
			dm_free(vginfo);
			log_error("cache vgname alloc failed for %s", vgname);
			return 0;
		}
		dm_list_init(&vginfo->infos);

		/*
		 * If we're scanning and there's an invalidated entry, remove it.
		 * Otherwise we risk bogus warnings of duplicate VGs.
		 */
		while ((primary_vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)) &&
		       _scanning_in_progress && _vginfo_is_invalid(primary_vginfo)) {
			orphan_vginfo = lvmcache_vginfo_from_vgname(primary_vginfo->fmt->orphan_vg_name, NULL);
			if (!orphan_vginfo) {
				log_error(INTERNAL_ERROR "Orphan vginfo %s lost from cache.",
					  primary_vginfo->fmt->orphan_vg_name);
				dm_free(vginfo->vgname);
				dm_free(vginfo);
				return 0;
			}
			dm_list_iterate_items_safe(info2, info3, &primary_vginfo->infos) {
				_vginfo_detach_info(info2);
				_vginfo_attach_info(orphan_vginfo, info2);
				if (info2->mdas.n)
					sprintf(mdabuf, " with %u mdas",
						dm_list_size(&info2->mdas));
				else
					mdabuf[0] = '\0';
				log_debug_cache("lvmcache: %s: now in VG %s%s%s%s%s",
						dev_name(info2->dev),
						vgname, orphan_vginfo->vgid[0] ? " (" : "",
						orphan_vginfo->vgid[0] ? orphan_vginfo->vgid : "",
						orphan_vginfo->vgid[0] ? ")" : "", mdabuf);
			}

			if (!_drop_vginfo(NULL, primary_vginfo))
				return_0;
		}

		if (!_insert_vginfo(vginfo, vgid, vgstatus, creation_host,
				    primary_vginfo)) {
			dm_free(vginfo->vgname);
			dm_free(vginfo);
			return 0;
		}
		/* Ensure orphans appear last on list_iterate */
		if (is_orphan_vg(vgname))
			dm_list_add(&_vginfos, &vginfo->list);
		else
			dm_list_add_h(&_vginfos, &vginfo->list);
/***
		}
***/
	}

	if (info)
		_vginfo_attach_info(vginfo, info);
	else if (!_lvmcache_update_vgid(NULL, vginfo, vgid)) /* Orphans */
		return_0;

	_update_cache_vginfo_lock_state(vginfo, lvmcache_vgname_is_locked(vgname));

	/* FIXME Check consistency of list! */
	vginfo->fmt = fmt;

	if (info) {
		if (info->mdas.n)
			sprintf(mdabuf, " with %u mdas", dm_list_size(&info->mdas));
		else
			mdabuf[0] = '\0';
		log_debug_cache("lvmcache: %s: now in VG %s%s%s%s%s",
				dev_name(info->dev),
				vgname, vginfo->vgid[0] ? " (" : "",
				vginfo->vgid[0] ? vginfo->vgid : "",
				vginfo->vgid[0] ? ")" : "", mdabuf);
	} else
		log_debug_cache("lvmcache: initialised VG %s", vgname);

	return 1;
}

static int _lvmcache_update_vgstatus(struct lvmcache_info *info, uint32_t vgstatus,
				     const char *creation_host, const char *lock_type)
{
	if (!info || !info->vginfo)
		return 1;

	if ((info->vginfo->status & EXPORTED_VG) != (vgstatus & EXPORTED_VG))
		log_debug_cache("lvmcache: %s: VG %s %s exported",
				dev_name(info->dev), info->vginfo->vgname,
				vgstatus & EXPORTED_VG ? "now" : "no longer");

	info->vginfo->status = vgstatus;

	if (!creation_host)
		goto set_lock_type;

	if (info->vginfo->creation_host && !strcmp(creation_host,
						   info->vginfo->creation_host))
		goto set_lock_type;

	if (info->vginfo->creation_host)
		dm_free(info->vginfo->creation_host);

	if (!(info->vginfo->creation_host = dm_strdup(creation_host))) {
		log_error("cache creation host alloc failed for %s",
			  creation_host);
		return 0;
	}

	log_debug_cache("lvmcache: %s: VG %s: Set creation host to %s.",
			dev_name(info->dev), info->vginfo->vgname, creation_host);

set_lock_type:

	if (!lock_type)
		goto out;

	if (info->vginfo->lock_type && !strcmp(lock_type, info->vginfo->lock_type))
		goto out;

	if (info->vginfo->lock_type)
		dm_free(info->vginfo->lock_type);

	if (!(info->vginfo->lock_type = dm_strdup(lock_type))) {
		log_error("cache creation host alloc failed for %s",
			  lock_type);
		return 0;
	}

out:
	return 1;
}

static int _lvmcache_update_vg_mda_info(struct lvmcache_info *info, uint32_t mda_checksum,
					size_t mda_size)
{
	if (!info || !info->vginfo || !mda_size)
		return 1;

	if (info->vginfo->mda_checksum == mda_checksum || info->vginfo->mda_size == mda_size) 
		return 1;

	info->vginfo->mda_checksum = mda_checksum;
	info->vginfo->mda_size = mda_size;

	/* FIXME Add checksum index */

	log_debug_cache("lvmcache: %s: VG %s: Stored metadata checksum %" PRIu32 " with size %" PRIsize_t ".",
			dev_name(info->dev), info->vginfo->vgname, mda_checksum, mda_size);

	return 1;
}

int lvmcache_add_orphan_vginfo(const char *vgname, struct format_type *fmt)
{
	if (!_lock_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		return 0;
	}

	return _lvmcache_update_vgname(NULL, vgname, vgname, 0, "", fmt);
}

int lvmcache_update_vgname_and_id(struct lvmcache_info *info, struct lvmcache_vgsummary *vgsummary)
{
	const char *vgname = vgsummary->vgname;
	const char *vgid = (char *)&vgsummary->vgid;

	if (!vgname && !info->vginfo) {
		log_error(INTERNAL_ERROR "NULL vgname handed to cache");
		/* FIXME Remove this */
		vgname = info->fmt->orphan_vg_name;
		vgid = vgname;
	}

	/* If PV without mdas is already in a real VG, don't make it orphan */
	if (is_orphan_vg(vgname) && info->vginfo &&
	    mdas_empty_or_ignored(&info->mdas) &&
	    !is_orphan_vg(info->vginfo->vgname) && critical_section())
		return 1;

	/* If making a PV into an orphan, any cached VG metadata may become
	 * invalid, incorrectly still referencing device structs.
	 * (Example: pvcreate -ff) */
	if (is_orphan_vg(vgname) && info->vginfo && !is_orphan_vg(info->vginfo->vgname))
		info->vginfo->cached_vg_invalidated = 1;

	/* If moving PV from orphan to real VG, always mark it valid */
	if (!is_orphan_vg(vgname))
		info->status &= ~CACHE_INVALID;

	if (!_lvmcache_update_vgname(info, vgname, vgid, vgsummary->vgstatus,
				     vgsummary->creation_host, info->fmt) ||
	    !_lvmcache_update_vgid(info, info->vginfo, vgid) ||
	    !_lvmcache_update_vgstatus(info, vgsummary->vgstatus, vgsummary->creation_host, vgsummary->lock_type) ||
	    !_lvmcache_update_vg_mda_info(info, vgsummary->mda_checksum, vgsummary->mda_size))
		return_0;

	return 1;
}

int lvmcache_update_vg(struct volume_group *vg, unsigned precommitted)
{
	struct pv_list *pvl;
	struct lvmcache_info *info;
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	struct lvmcache_vgsummary vgsummary = {
		.vgname = vg->name,
		.vgstatus = vg->status,
		.vgid = vg->id,
		.lock_type = vg->lock_type
	};

	pvid_s[sizeof(pvid_s) - 1] = '\0';

	dm_list_iterate_items(pvl, &vg->pvs) {
		strncpy(pvid_s, (char *) &pvl->pv->id, sizeof(pvid_s) - 1);
		/* FIXME Could pvl->pv->dev->pvid ever be different? */
		if ((info = lvmcache_info_from_pvid(pvid_s, 0)) &&
		    !lvmcache_update_vgname_and_id(info, &vgsummary))
			return_0;
	}

	/* store text representation of vg to cache */
	if (vg->cmd->current_settings.cache_vgmetadata)
		_store_metadata(vg, precommitted);

	return 1;
}

/*
 * When scanning all devices, lvmcache detects duplicate PVs exist if it sees
 * two different devices with the same pvid.  When it sees this, it ignores the
 * duplicate device, and sets _found_duplicates.  After the scan is done, we
 * set or clear a flag in lvmetad indicating duplicate PVs, so that subsequent
 * commands can print warnings about thatm.
 */
int lvmcache_found_duplicates(void)
{
	return _found_duplicates;
}

void lvmcache_clear_found_duplicates(void)
{
	_found_duplicates = 0;
}

struct lvmcache_info *lvmcache_add(struct labeller *labeller, const char *pvid,
				   struct device *dev,
				   const char *vgname, const char *vgid,
				   uint32_t vgstatus)
{
	struct label *label;
	struct lvmcache_info *existing, *info;
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	struct lvmcache_vgsummary vgsummary = {
		.vgname = vgname,
		.vgstatus = vgstatus,
	};

	/* N.B. vgid is not NUL-terminated when called from _text_pv_write */
	if (vgid)
		strncpy((char *)&vgsummary.vgid, vgid, sizeof(vgsummary.vgid));

	if (!_vgname_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		return NULL;
	}

	strncpy(pvid_s, pvid, sizeof(pvid_s) - 1);
	pvid_s[sizeof(pvid_s) - 1] = '\0';

	if (!(existing = lvmcache_info_from_pvid(pvid_s, 0)) &&
	    !(existing = lvmcache_info_from_pvid(dev->pvid, 0))) {
		if (!(label = label_create(labeller)))
			return_NULL;
		if (!(info = dm_zalloc(sizeof(*info)))) {
			log_error("lvmcache_info allocation failed");
			label_destroy(label);
			return NULL;
		}

		label->info = info;
		info->label = label;
		dm_list_init(&info->list);
		info->dev = dev;

		lvmcache_del_mdas(info);
		lvmcache_del_das(info);
		lvmcache_del_bas(info);
	} else {
		if (existing->dev != dev) {
			log_warn("Ignore duplicate PV on device %s. Already using PV from device %s. (%s)",
				 dev_name(dev), dev_name(existing->dev), pvid_s);
			log_warn("Use the global_filter to select a different device.");
			_found_duplicates = 1;
			return NULL;
		} else {
			/*
			 * The new dev is the same as the existing dev.
			 *
			 * FIXME: Why can't we just return NULL here if the
			 * device already exists?  Things don't seem to work
			 * if we do that for some reason.
			 */
			log_debug_cache("Found same device %s with same pvid %s",
					dev_name(existing->dev), pvid_s);
		}

		/*
		 * This happens when running pvcreate on an existing PV.
		 */
		if (strcmp(pvid_s, existing->dev->pvid))  {
			log_verbose("Replacing dev %s pvid %s with dev %s pvid %s",
				    dev_name(existing->dev), existing->dev->pvid,
				    dev_name(dev), pvid_s);
		}

		/*
		 * Switch over to new preferred device.
		 */
		existing->dev = dev;
		info = existing;
		/* Has labeller changed? */
		if (info->label->labeller != labeller) {
			label_destroy(info->label);
			if (!(info->label = label_create(labeller)))
				/* FIXME leaves info without label! */
				return_NULL;
			info->label->info = info;
		}
		label = info->label;
	}

	info->fmt = labeller->fmt;
	info->status |= CACHE_INVALID;

	if (!_lvmcache_update_pvid(info, pvid_s)) {
		if (!existing) {
			dm_free(info);
			label_destroy(label);
		}
		return NULL;
	}

	if (!lvmcache_update_vgname_and_id(info, &vgsummary)) {
		if (!existing) {
			dm_hash_remove(_pvid_hash, pvid_s);
			strcpy(info->dev->pvid, "");
			dm_free(info);
			label_destroy(label);
		}
		return NULL;
	}

	return info;
}

static void _lvmcache_destroy_entry(struct lvmcache_info *info)
{
	_vginfo_detach_info(info);
	info->dev->pvid[0] = 0;
	label_destroy(info->label);
	dm_free(info);
}

static void _lvmcache_destroy_vgnamelist(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_vginfo *next;

	do {
		next = vginfo->next;
		if (!_free_vginfo(vginfo))
			stack;
	} while ((vginfo = next));
}

static void _lvmcache_destroy_lockname(struct dm_hash_node *n)
{
	char *vgname;

	if (!dm_hash_get_data(_lock_hash, n))
		return;

	vgname = dm_hash_get_key(_lock_hash, n);

	if (!strcmp(vgname, VG_GLOBAL))
		_vg_global_lock_held = 1;
	else
		log_error(INTERNAL_ERROR "Volume Group %s was not unlocked",
			  dm_hash_get_key(_lock_hash, n));
}

void lvmcache_destroy(struct cmd_context *cmd, int retain_orphans, int reset)
{
	struct dm_hash_node *n;
	log_verbose("Wiping internal VG cache");

	_has_scanned = 0;

	if (_vgid_hash) {
		dm_hash_destroy(_vgid_hash);
		_vgid_hash = NULL;
	}

	if (_pvid_hash) {
		dm_hash_iter(_pvid_hash, (dm_hash_iterate_fn) _lvmcache_destroy_entry);
		dm_hash_destroy(_pvid_hash);
		_pvid_hash = NULL;
	}

	if (_vgname_hash) {
		dm_hash_iter(_vgname_hash,
			  (dm_hash_iterate_fn) _lvmcache_destroy_vgnamelist);
		dm_hash_destroy(_vgname_hash);
		_vgname_hash = NULL;
	}

	if (_lock_hash) {
		if (reset)
			_vg_global_lock_held = 0;
		else
			dm_hash_iterate(n, _lock_hash)
				_lvmcache_destroy_lockname(n);
		dm_hash_destroy(_lock_hash);
		_lock_hash = NULL;
	}

	if (!dm_list_empty(&_vginfos))
		log_error(INTERNAL_ERROR "_vginfos list should be empty");
	dm_list_init(&_vginfos);

	if (retain_orphans)
		if (!init_lvmcache_orphans(cmd))
			stack;
}

int lvmcache_pvid_is_locked(const char *pvid) {
	struct lvmcache_info *info;
	info = lvmcache_info_from_pvid(pvid, 0);
	if (!info || !info->vginfo)
		return 0;

	return lvmcache_vgname_is_locked(info->vginfo->vgname);
}

int lvmcache_fid_add_mdas(struct lvmcache_info *info, struct format_instance *fid,
			  const char *id, int id_len)
{
	return fid_add_mdas(fid, &info->mdas, id, id_len);
}

int lvmcache_fid_add_mdas_pv(struct lvmcache_info *info, struct format_instance *fid)
{
	return lvmcache_fid_add_mdas(info, fid, info->dev->pvid, ID_LEN);
}

int lvmcache_fid_add_mdas_vg(struct lvmcache_vginfo *vginfo, struct format_instance *fid)
{
	struct lvmcache_info *info;
	dm_list_iterate_items(info, &vginfo->infos) {
		if (!lvmcache_fid_add_mdas_pv(info, fid))
			return_0;
	}
	return 1;
}

static int _get_pv_if_in_vg(struct lvmcache_info *info,
			    struct physical_volume *pv)
{
	char vgname[NAME_LEN + 1];
	char vgid[ID_LEN + 1];

	if (info->vginfo && info->vginfo->vgname &&
	    !is_orphan_vg(info->vginfo->vgname)) {
		/*
		 * get_pv_from_vg_by_id() may call
		 * lvmcache_label_scan() and drop cached
		 * vginfo so make a local copy of string.
		 */
		(void) dm_strncpy(vgname, info->vginfo->vgname, sizeof(vgname));
		memcpy(vgid, info->vginfo->vgid, sizeof(vgid));

		if (get_pv_from_vg_by_id(info->fmt, vgname, vgid,
					 info->dev->pvid, pv))
			return 1;
	}

	return 0;
}

int lvmcache_populate_pv_fields(struct lvmcache_info *info,
				struct physical_volume *pv,
				int scan_label_only)
{
	struct data_area_list *da;

	/* Have we already cached vgname? */
	if (!scan_label_only && _get_pv_if_in_vg(info, pv))
		return 1;

	/* Perform full scan (just the first time) and try again */
	if (!scan_label_only && !critical_section() && !full_scan_done()) {
		lvmcache_label_scan(info->fmt->cmd, 2);

		if (_get_pv_if_in_vg(info, pv))
			return 1;
	}

	/* Orphan */
	pv->dev = info->dev;
	pv->fmt = info->fmt;
	pv->size = info->device_size >> SECTOR_SHIFT;
	pv->vg_name = FMT_TEXT_ORPHAN_VG_NAME;
	memcpy(&pv->id, &info->dev->pvid, sizeof(pv->id));

	/* Currently only support exactly one data area */
	if (dm_list_size(&info->das) != 1) {
		log_error("Must be exactly one data area (found %d) on PV %s",
			  dm_list_size(&info->das), dev_name(info->dev));
		return 0;
	}

	/* Currently only support one bootloader area at most */
	if (dm_list_size(&info->bas) > 1) {
		log_error("Must be at most one bootloader area (found %d) on PV %s",
			  dm_list_size(&info->bas), dev_name(info->dev));
		return 0;
	}

	dm_list_iterate_items(da, &info->das)
		pv->pe_start = da->disk_locn.offset >> SECTOR_SHIFT;

	dm_list_iterate_items(da, &info->bas) {
		pv->ba_start = da->disk_locn.offset >> SECTOR_SHIFT;
		pv->ba_size = da->disk_locn.size >> SECTOR_SHIFT;
	}

	return 1;
}

int lvmcache_check_format(struct lvmcache_info *info, const struct format_type *fmt)
{
	if (info->fmt != fmt) {
		log_error("PV %s is a different format (seqno %s)",
			  dev_name(info->dev), info->fmt->name);
		return 0;
	}
	return 1;
}

void lvmcache_del_mdas(struct lvmcache_info *info)
{
	if (info->mdas.n)
		del_mdas(&info->mdas);
	dm_list_init(&info->mdas);
}

void lvmcache_del_das(struct lvmcache_info *info)
{
	if (info->das.n)
		del_das(&info->das);
	dm_list_init(&info->das);
}

void lvmcache_del_bas(struct lvmcache_info *info)
{
	if (info->bas.n)
		del_bas(&info->bas);
	dm_list_init(&info->bas);
}

int lvmcache_add_mda(struct lvmcache_info *info, struct device *dev,
		     uint64_t start, uint64_t size, unsigned ignored)
{
	return add_mda(info->fmt, NULL, &info->mdas, dev, start, size, ignored);
}

int lvmcache_add_da(struct lvmcache_info *info, uint64_t start, uint64_t size)
{
	return add_da(NULL, &info->das, start, size);
}

int lvmcache_add_ba(struct lvmcache_info *info, uint64_t start, uint64_t size)
{
	return add_ba(NULL, &info->bas, start, size);
}

void lvmcache_update_pv(struct lvmcache_info *info, struct physical_volume *pv,
			const struct format_type *fmt)
{
	info->device_size = pv->size << SECTOR_SHIFT;
	info->fmt = fmt;
}

int lvmcache_update_das(struct lvmcache_info *info, struct physical_volume *pv)
{
	struct data_area_list *da;
	if (info->das.n) {
		if (!pv->pe_start)
			dm_list_iterate_items(da, &info->das)
				pv->pe_start = da->disk_locn.offset >> SECTOR_SHIFT;
		del_das(&info->das);
	} else
		dm_list_init(&info->das);

	if (!add_da(NULL, &info->das, pv->pe_start << SECTOR_SHIFT, 0 /*pv->size << SECTOR_SHIFT*/))
		return_0;

	return 1;
}

int lvmcache_update_bas(struct lvmcache_info *info, struct physical_volume *pv)
{
	struct data_area_list *ba;
	if (info->bas.n) {
		if (!pv->ba_start && !pv->ba_size)
			dm_list_iterate_items(ba, &info->bas) {
				pv->ba_start = ba->disk_locn.offset >> SECTOR_SHIFT;
				pv->ba_size = ba->disk_locn.size >> SECTOR_SHIFT;
			}
		del_das(&info->bas);
	} else
		dm_list_init(&info->bas);

	if (!add_ba(NULL, &info->bas, pv->ba_start << SECTOR_SHIFT, pv->ba_size << SECTOR_SHIFT))
		return_0;

	return 1;
}

int lvmcache_foreach_pv(struct lvmcache_vginfo *vginfo,
			int (*fun)(struct lvmcache_info *, void *),
			void *baton)
{
	struct lvmcache_info *info;
	dm_list_iterate_items(info, &vginfo->infos) {
		if (!fun(info, baton))
			return_0;
	}

	return 1;
}

int lvmcache_foreach_mda(struct lvmcache_info *info,
			 int (*fun)(struct metadata_area *, void *),
			 void *baton)
{
	struct metadata_area *mda;
	dm_list_iterate_items(mda, &info->mdas) {
		if (!fun(mda, baton))
			return_0;
	}

	return 1;
}

unsigned lvmcache_mda_count(struct lvmcache_info *info)
{
	return dm_list_size(&info->mdas);
}

int lvmcache_foreach_da(struct lvmcache_info *info,
			int (*fun)(struct disk_locn *, void *),
			void *baton)
{
	struct data_area_list *da;
	dm_list_iterate_items(da, &info->das) {
		if (!fun(&da->disk_locn, baton))
			return_0;
	}

	return 1;
}

int lvmcache_foreach_ba(struct lvmcache_info *info,
			 int (*fun)(struct disk_locn *, void *),
			 void *baton)
{
	struct data_area_list *ba;
	dm_list_iterate_items(ba, &info->bas) {
		if (!fun(&ba->disk_locn, baton))
			return_0;
	}

	return 1;
}

/*
 * The lifetime of the label returned is tied to the lifetime of the
 * lvmcache_info which is the same as lvmcache itself.
 */
struct label *lvmcache_get_label(struct lvmcache_info *info) {
	return info->label;
}

void lvmcache_make_valid(struct lvmcache_info *info) {
	info->status &= ~CACHE_INVALID;
}

uint64_t lvmcache_device_size(struct lvmcache_info *info) {
	return info->device_size;
}

void lvmcache_set_device_size(struct lvmcache_info *info, uint64_t size) {
	info->device_size = size;
}

struct device *lvmcache_device(struct lvmcache_info *info) {
	return info->dev;
}

int lvmcache_is_orphan(struct lvmcache_info *info) {
	if (!info->vginfo)
		return 1; /* FIXME? */
	return is_orphan_vg(info->vginfo->vgname);
}

int lvmcache_vgid_is_cached(const char *vgid) {
	struct lvmcache_vginfo *vginfo;

	if (lvmetad_active())
		return 1;

	vginfo = lvmcache_vginfo_from_vgid(vgid);

	if (!vginfo || !vginfo->vgname)
		return 0;

	if (is_orphan_vg(vginfo->vgname))
		return 0;

	return 1;
}

/*
 * Return true iff it is impossible to find out from this info alone whether the
 * PV in question is or is not an orphan.
 */
int lvmcache_uncertain_ownership(struct lvmcache_info *info) {
	return mdas_empty_or_ignored(&info->mdas);
}

uint64_t lvmcache_smallest_mda_size(struct lvmcache_info *info)
{
	if (!info)
		return UINT64_C(0);

	return find_min_mda_size(&info->mdas);
}

const struct format_type *lvmcache_fmt(struct lvmcache_info *info) {
	return info->fmt;
}

int lvmcache_lookup_mda(struct lvmcache_vgsummary *vgsummary)
{
	struct lvmcache_vginfo *vginfo;

	if (!vgsummary->mda_size)
		return 0;

	/* FIXME Index the checksums */
	dm_list_iterate_items(vginfo, &_vginfos) {
		if (vgsummary->mda_checksum == vginfo->mda_checksum &&
		    vgsummary->mda_size == vginfo->mda_size &&
		    !is_orphan_vg(vginfo->vgname)) {
			vgsummary->vgname = vginfo->vgname;
			vgsummary->creation_host = vginfo->creation_host;
			vgsummary->vgstatus = vginfo->status;
			/* vginfo->vgid has 1 extra byte then vgsummary->vgid */
			memcpy(&vgsummary->vgid, vginfo->vgid, sizeof(vgsummary->vgid));

			return 1;
		}
	}

	return 0;
}

int lvmcache_contains_lock_type_sanlock(struct cmd_context *cmd)
{
	struct lvmcache_vginfo *vginfo;

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (vginfo->lock_type && !strcmp(vginfo->lock_type, "sanlock"))
			return 1;
	}

	return 0;
}

void lvmcache_get_max_name_lengths(struct cmd_context *cmd,
				   unsigned *pv_max_name_len,
				   unsigned *vg_max_name_len)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	unsigned len;

	*vg_max_name_len = 0;
	*pv_max_name_len = 0;

	dm_list_iterate_items(vginfo, &_vginfos) {
		len = strlen(vginfo->vgname);
		if (*vg_max_name_len < len)
			*vg_max_name_len = len;

		dm_list_iterate_items(info, &vginfo->infos) {
			len = strlen(dev_name(info->dev));
			if (*pv_max_name_len < len)
				*pv_max_name_len = len;
		}
	}
}

