/*
 * LiME - Linux Memory Extractor
 * Copyright (c) 2011-2014 Joe Sylve - 504ENSICS Labs
 *
 *
 * Author:
 * Joe Sylve       - joe.sylve@gmail.com, @jtsylve
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "lime.h"
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>

/* External references to main.c variables */
extern char *path;
extern int dio;
extern int port;
extern int localhostonly;
extern char *digest;

#ifdef LIME_SUPPORTS_TIMING
extern long timeout;
#endif

#ifdef LIME_SUPPORTS_DEFLATE
extern int compress;
#endif

/* External function to perform acquisition */
extern int lime_do_acquisition(void);

/* State management */
static int lime_state = LIME_STATE_IDLE;
static DEFINE_MUTEX(lime_state_mutex);

/* Local storage for sysfs-configured strings */
static char sysfs_path[LIME_MAX_FILENAME_SIZE] = "";
static char sysfs_format[16] = "";
static char sysfs_digest[32] = "";

/* Kobject for sysfs interface (only used in non-stealth mode) */
#ifndef CONFIG_LIME_STEALTH
static struct kobject *lime_kobj;
#endif

/* Format string storage (exposed for main.c) */
char *lime_format = NULL;

/* Target PID storage (exposed for main.c) */
int lime_target_pid = -1;  /* -1 means dump all physical memory */

#ifdef CONFIG_LIME_STEALTH
/*
 * ioctl-based stealth interface
 * Disguised as camera sensor driver to blend with existing lwis devices
 */

#define LIME_IOCTL_MAGIC 0x4C4D4531  /* "LME1" in hex */

/* Configuration structure passed via ioctl */
struct lime_ioctl_config {
	int target_pid;           /* -1 = full memory, >0 = process PID */
	char path[256];           /* Output path or tcp:port */
	char format[16];          /* raw, lime, or padded */
	char digest[32];          /* Hash algorithm (optional) */
	int dio;                  /* Direct I/O flag */
	int localhostonly;        /* TCP localhost only */
};

static long lime_device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lime_ioctl_config cfg;
	int ret;

	/* Only respond to our magic ioctl number */
	if (cmd != LIME_IOCTL_MAGIC)
		return -ENOTTY;

	/* Check if already acquiring */
	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		LIME_ERR("acquisition already in progress");
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	/* Copy configuration from userspace */
	if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
		return -EFAULT;

	/* Validate required parameters */
	if (!cfg.path[0]) {
		LIME_ERR("path parameter not set");
		return -EINVAL;
	}

	if (!cfg.format[0]) {
		LIME_ERR("format parameter not set");
		return -EINVAL;
	}

	if (cfg.target_pid < -1) {
		LIME_ERR("invalid target_pid: %d", cfg.target_pid);
		return -EINVAL;
	}

	/* Set parameters */
	lime_target_pid = cfg.target_pid;

	strncpy(sysfs_path, cfg.path, sizeof(sysfs_path) - 1);
	sysfs_path[sizeof(sysfs_path) - 1] = '\0';
	path = sysfs_path;

	strncpy(sysfs_format, cfg.format, sizeof(sysfs_format) - 1);
	sysfs_format[sizeof(sysfs_format) - 1] = '\0';
	lime_format = sysfs_format;

	if (cfg.digest[0]) {
		strncpy(sysfs_digest, cfg.digest, sizeof(sysfs_digest) - 1);
		sysfs_digest[sizeof(sysfs_digest) - 1] = '\0';
		digest = sysfs_digest;
	} else {
		digest = NULL;
	}

	dio = cfg.dio;
	localhostonly = cfg.localhostonly;

	/* Update state */
	mutex_lock(&lime_state_mutex);
	lime_state = LIME_STATE_ACQUIRING;
	mutex_unlock(&lime_state_mutex);

	LIME_INFO("acquisition triggered via ioctl: pid=%d path=%s format=%s",
		  lime_target_pid, path, lime_format);

	/* Perform acquisition */
	ret = lime_do_acquisition();

	/* Update final state */
	mutex_lock(&lime_state_mutex);
	if (ret == 0) {
		lime_state = LIME_STATE_COMPLETE;
		LIME_INFO("acquisition complete");
	} else {
		lime_state = LIME_STATE_ERROR;
		LIME_ERR("acquisition failed: %d", ret);
	}
	mutex_unlock(&lime_state_mutex);

	return ret;
}

static const struct file_operations lime_device_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = lime_device_ioctl,
	.compat_ioctl = lime_device_ioctl,
};

static struct miscdevice lime_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "lwis-sensor-imx461",  /* Disguised as Sony camera sensor */
	.fops = &lime_device_fops,
	.mode = 0600,  /* Root only */
};

int lime_sysfs_init(void)
{
	int ret;

	LIME_INFO("stealth mode: registering ioctl interface");

	ret = misc_register(&lime_misc_device);
	if (ret) {
		LIME_ERR("failed to register misc device: %d", ret);
		return ret;
	}

	LIME_INFO("device registered as /dev/lwis-sensor-imx461");
	LIME_INFO("  control via ioctl (magic: 0x4C4D4531)");

	return 0;
}

void lime_sysfs_cleanup(void)
{
	misc_deregister(&lime_misc_device);
	LIME_INFO("device unregistered");
}

#else  /* !CONFIG_LIME_STEALTH - Standard sysfs interface */

/*
 * Helper to get current state as string
 */
static const char *lime_state_str(int state)
{
	switch (state) {
	case LIME_STATE_IDLE:
		return "idle";
	case LIME_STATE_ACQUIRING:
		return "acquiring";
	case LIME_STATE_COMPLETE:
		return "complete";
	case LIME_STATE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

/*
 * Path attribute - output destination
 */
static ssize_t path_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sprintf(buf, "%s\n", path ? path : "");
}

static ssize_t path_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	size_t len;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	len = min(count, sizeof(sysfs_path) - 1);
	memcpy(sysfs_path, buf, len);
	/* Remove trailing newline */
	if (len > 0 && sysfs_path[len - 1] == '\n')
		len--;
	sysfs_path[len] = '\0';
	path = sysfs_path;

	return count;
}

static struct kobj_attribute path_attr = __ATTR_RW(path);

/*
 * Format attribute - output format (raw, lime, padded)
 */
static ssize_t format_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sprintf(buf, "%s\n", lime_format ? lime_format : "");
}

static ssize_t format_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	size_t len;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	len = min(count, sizeof(sysfs_format) - 1);
	memcpy(sysfs_format, buf, len);
	/* Remove trailing newline */
	if (len > 0 && sysfs_format[len - 1] == '\n')
		len--;
	sysfs_format[len] = '\0';

	/* Validate format */
	if (strcmp(sysfs_format, "raw") != 0 &&
	    strcmp(sysfs_format, "lime") != 0 &&
	    strcmp(sysfs_format, "padded") != 0) {
		sysfs_format[0] = '\0';
		lime_format = NULL;
		return -EINVAL;
	}

	lime_format = sysfs_format;
	return count;
}

static struct kobj_attribute format_attr = __ATTR_RW(format);

/*
 * DIO attribute - direct I/O flag
 */
static ssize_t dio_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d\n", dio);
}

static ssize_t dio_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int val;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	dio = val ? 1 : 0;
	return count;
}

static struct kobj_attribute dio_attr = __ATTR_RW(dio);

/*
 * Localhostonly attribute - TCP binding flag
 */
static ssize_t localhostonly_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", localhostonly);
}

static ssize_t localhostonly_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int val;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	localhostonly = val ? 1 : 0;
	return count;
}

static struct kobj_attribute localhostonly_attr = __ATTR_RW(localhostonly);

/*
 * Digest attribute - hash algorithm
 */
static ssize_t digest_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sprintf(buf, "%s\n", digest ? digest : "");
}

static ssize_t digest_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	size_t len;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	if (count == 0 || (count == 1 && buf[0] == '\n')) {
		sysfs_digest[0] = '\0';
		digest = NULL;
		return count;
	}

	len = min(count, sizeof(sysfs_digest) - 1);
	memcpy(sysfs_digest, buf, len);
	/* Remove trailing newline */
	if (len > 0 && sysfs_digest[len - 1] == '\n')
		len--;
	sysfs_digest[len] = '\0';
	digest = sysfs_digest;

	return count;
}

static struct kobj_attribute digest_attr = __ATTR_RW(digest);

/*
 * Target PID attribute - process to dump (-1 = full memory)
 */
static ssize_t target_pid_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%d\n", lime_target_pid);
}

static ssize_t target_pid_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int val, ret;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val < -1)
		return -EINVAL;

	lime_target_pid = val;
	DBG("target_pid set to %d", lime_target_pid);

	return count;
}

static struct kobj_attribute target_pid_attr = __ATTR_RW(target_pid);

#ifdef LIME_SUPPORTS_TIMING
/*
 * Timeout attribute - page read timeout in ms
 */
static ssize_t timeout_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%ld\n", timeout);
}

static ssize_t timeout_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	long val;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	timeout = val;
	return count;
}

static struct kobj_attribute timeout_attr = __ATTR_RW(timeout);
#endif /* LIME_SUPPORTS_TIMING */

#ifdef LIME_SUPPORTS_DEFLATE
/*
 * Compress attribute - compression flag
 */
static ssize_t compress_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%d\n", compress);
}

static ssize_t compress_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	int val;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	mutex_unlock(&lime_state_mutex);

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	compress = val ? 1 : 0;
	return count;
}

static struct kobj_attribute compress_attr = __ATTR_RW(compress);
#endif /* LIME_SUPPORTS_DEFLATE */

/*
 * State attribute - current acquisition state (read-only)
 */
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	int state;

	mutex_lock(&lime_state_mutex);
	state = lime_state;
	mutex_unlock(&lime_state_mutex);

	return sprintf(buf, "%s\n", lime_state_str(state));
}

static struct kobj_attribute state_attr = __ATTR_RO(state);

/*
 * Trigger attribute - start acquisition (write 1 to start)
 * Reading returns current state
 */
static ssize_t trigger_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	int state;

	mutex_lock(&lime_state_mutex);
	state = lime_state;
	mutex_unlock(&lime_state_mutex);

	return sprintf(buf, "%s\n", lime_state_str(state));
}

static ssize_t trigger_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	int val, ret;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val != 1)
		return -EINVAL;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		LIME_ERR("acquisition already in progress");
		return -EBUSY;
	}

	/* Validate required parameters */
	if (!path || !path[0]) {
		mutex_unlock(&lime_state_mutex);
		LIME_ERR("path parameter not set");
		return -EINVAL;
	}

	if (!lime_format || !lime_format[0]) {
		mutex_unlock(&lime_state_mutex);
		LIME_ERR("format parameter not set");
		return -EINVAL;
	}

	lime_state = LIME_STATE_ACQUIRING;
	mutex_unlock(&lime_state_mutex);

	LIME_INFO("acquisition triggered: path=%s format=%s", path, lime_format);

	/* Perform acquisition */
	ret = lime_do_acquisition();

	mutex_lock(&lime_state_mutex);
	if (ret == 0) {
		lime_state = LIME_STATE_COMPLETE;
		LIME_INFO("acquisition complete");
	} else {
		lime_state = LIME_STATE_ERROR;
		LIME_ERR("acquisition failed: %d", ret);
	}
	mutex_unlock(&lime_state_mutex);

	return (ret == 0) ? count : ret;
}

static struct kobj_attribute trigger_attr = __ATTR_RW(trigger);

/*
 * Reset attribute - reset state to idle (write 1 to reset)
 */
static ssize_t reset_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sprintf(buf, "Write 1 to reset state to idle\n");
}

static ssize_t reset_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val != 1)
		return -EINVAL;

	mutex_lock(&lime_state_mutex);
	if (lime_state == LIME_STATE_ACQUIRING) {
		mutex_unlock(&lime_state_mutex);
		return -EBUSY;
	}
	lime_state = LIME_STATE_IDLE;
	mutex_unlock(&lime_state_mutex);

	return count;
}

static struct kobj_attribute reset_attr = __ATTR_RW(reset);

/*
 * Attribute group
 */
static struct attribute *lime_attrs[] = {
	&path_attr.attr,
	&format_attr.attr,
	&dio_attr.attr,
	&localhostonly_attr.attr,
	&digest_attr.attr,
	&target_pid_attr.attr,
#ifdef LIME_SUPPORTS_TIMING
	&timeout_attr.attr,
#endif
#ifdef LIME_SUPPORTS_DEFLATE
	&compress_attr.attr,
#endif
	&state_attr.attr,
	&trigger_attr.attr,
	&reset_attr.attr,
	NULL,
};

static struct attribute_group lime_attr_group = {
	.attrs = lime_attrs,
};

/*
 * Initialize sysfs interface
 */
int lime_sysfs_init(void)
{
	int ret;

	/* Create kobject under /sys/kernel/lime */
	lime_kobj = kobject_create_and_add("lime", kernel_kobj);
	if (!lime_kobj) {
		LIME_ERR("failed to create kobject");
		return -ENOMEM;
	}

	/* Create sysfs group */
	ret = sysfs_create_group(lime_kobj, &lime_attr_group);
	if (ret) {
		LIME_ERR("failed to create sysfs attributes: %d", ret);
		kobject_put(lime_kobj);
		lime_kobj = NULL;
		return ret;
	}

	LIME_INFO("sysfs interface registered at /sys/kernel/lime/");
	LIME_INFO("  attributes: path, format, dio, localhostonly, digest, target_pid"
#ifdef LIME_SUPPORTS_TIMING
		  ", timeout"
#endif
#ifdef LIME_SUPPORTS_DEFLATE
		  ", compress"
#endif
		  ", state, trigger, reset");
	DBG("LiME sysfs interface initialized at /sys/kernel/lime");
	return 0;
}

/*
 * Cleanup sysfs interface
 */
void lime_sysfs_cleanup(void)
{
	if (lime_kobj) {
		sysfs_remove_group(lime_kobj, &lime_attr_group);
		kobject_put(lime_kobj);
		lime_kobj = NULL;
		LIME_INFO("sysfs interface unregistered");
	}
	DBG("LiME sysfs interface removed");
}

#endif  /* CONFIG_LIME_STEALTH */

/*
 * Get current state (for external use)
 */
int lime_get_state(void)
{
	int state;

	mutex_lock(&lime_state_mutex);
	state = lime_state;
	mutex_unlock(&lime_state_mutex);

	return state;
}

/*
 * Set state (for external use, e.g., from module init)
 */
void lime_set_state(int state)
{
	mutex_lock(&lime_state_mutex);
	lime_state = state;
	mutex_unlock(&lime_state_mutex);
}
