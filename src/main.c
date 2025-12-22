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

// This file
static ssize_t write_lime_header(struct resource *);
static ssize_t write_padding(size_t);
static void write_range(struct resource *);
static int init(void);
static ssize_t write_vaddr(void *, size_t);
static ssize_t write_flush(void);
static ssize_t try_write(void *, ssize_t);
static int setup(void);
static void cleanup(void);

// External
extern ssize_t write_vaddr_tcp(void *, size_t);
extern int setup_tcp(void);
extern void cleanup_tcp(void);

extern ssize_t write_vaddr_disk(void *, size_t);
extern int setup_disk(char *, int);
extern void cleanup_disk(void);

extern int ldigest_init(void);
extern int ldigest_update(void *, size_t);
extern int ldigest_final(void);
extern int ldigest_write_tcp(void);
extern int ldigest_write_disk(void);
extern int ldigest_clean(void);

#ifdef LIME_SUPPORTS_DEFLATE
extern int deflate_begin_stream(void *, size_t);
extern int deflate_end_stream(void);
extern ssize_t deflate(const void *, size_t);
#endif

static char *format = NULL;
static int mode = 0;
static int method = 0;

static void *vpage;

#ifdef LIME_SUPPORTS_DEFLATE
static void *deflate_page_buf;
#endif

char *path = NULL;
int dio = 0;
int port = 0;
int localhostonly = 0;

char *digest = NULL;
int compute_digest = 0;

int no_overlap = 0;

extern struct resource iomem_resource;

/*
 * Module parameters - these allow traditional insmod-based configuration
 * When built-in, use the sysfs interface at /sys/kernel/lime/ instead
 */
module_param(path, charp, S_IRUGO);
module_param(dio, int, S_IRUGO);
module_param(format, charp, S_IRUGO);
module_param(localhostonly, int, S_IRUGO);
module_param(digest, charp, S_IRUGO);

#ifdef LIME_SUPPORTS_TIMING
long timeout = 1000;
module_param(timeout, long, S_IRUGO);
#endif

#ifdef LIME_SUPPORTS_DEFLATE
int compress = 0;
module_param(compress, int, S_IRUGO);
#endif

/*
 * When sysfs_only=1 or built-in, don't auto-start acquisition
 * Instead, use /sys/kernel/lime/trigger to start
 */
static int sysfs_only = 0;
module_param(sysfs_only, int, S_IRUGO);

/*
 * Parse format string and set mode
 * Returns 0 on success, -EINVAL on error
 */
static int parse_format(const char *fmt)
{
    if (!fmt || !fmt[0])
        return -EINVAL;

    if (!strcmp(fmt, "raw"))
        mode = LIME_MODE_RAW;
    else if (!strcmp(fmt, "lime"))
        mode = LIME_MODE_LIME;
    else if (!strcmp(fmt, "padded"))
        mode = LIME_MODE_PADDED;
    else
        return -EINVAL;

    return 0;
}

/*
 * Dump memory of a specific process using its virtual address space
 * This is stealthier than userspace /proc/<pid>/mem access
 */
int lime_dump_process(int pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    unsigned long vaddr, nr_pages = 0, nr_dumped = 0;
    int ret = 0, err;
    void *page_buf;

    LIME_INFO("dumping process memory: pid=%d", pid);

    /* Setup output method (disk/tcp) */
    if ((err = setup())) {
        LIME_ERR("setup failed for process dump");
        cleanup();
        return err;
    }

    /* Allocate page buffer */
    page_buf = (void *)__get_free_page(GFP_KERNEL);
    if (!page_buf) {
        LIME_ERR("failed to allocate page buffer");
        cleanup();
        return -ENOMEM;
    }

    /* Initialize digest if requested */
    if (digest) {
        compute_digest = ldigest_init();
    }

    /* Find the task */
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        LIME_ERR("process %d not found", pid);
        free_page((unsigned long)page_buf);
        cleanup();
        return -ESRCH;
    }

    /* Get reference to task's mm */
    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm) {
        LIME_ERR("process %d has no mm_struct", pid);
        free_page((unsigned long)page_buf);
        cleanup();
        return -EINVAL;
    }

    LIME_INFO("dumping process %d (%s)", pid, task->comm);

    /* Lock the mm for reading */
    if (mmap_read_lock_killable(mm)) {
        mmput(mm);
        free_page((unsigned long)page_buf);
        cleanup();
        return -EINTR;
    }

    /*
     * Iterate through all VMAs (virtual memory areas)
     * This includes: heap, stack, mmap regions, shared libraries, etc.
     * Use VMA iterator for kernel 6.1+ (maple tree instead of linked list)
     */
    VMA_ITERATOR(vmi, mm, 0);
    for_each_vma(vmi, vma) {
        unsigned long vma_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;

        /* Skip non-readable regions */
        if (!(vma->vm_flags & VM_READ))
            continue;

        /*
         * Optional filtering - uncomment to skip certain types:
         * Skip read-only file mappings (shared libraries):
         *   if (vma->vm_file && !(vma->vm_flags & VM_WRITE))
         *       continue;
         * Skip special mappings:
         *   if (vma->vm_flags & (VM_IO | VM_PFNMAP))
         *       continue;
         */

        DBG("VMA: %lx-%lx pages=%lu flags=%lx",
            vma->vm_start, vma->vm_end, vma_pages, vma->vm_flags);

        nr_pages += vma_pages;

        /* Write LiME header for this VMA if in LIME mode */
        if (mode == LIME_MODE_LIME) {
            lime_mem_range_header header;
            header.magic = LIME_MAGIC;
            header.version = 1;
            header.s_addr = vma->vm_start;  /* Virtual address */
            header.e_addr = vma->vm_end - 1;
            memset(header.reserved, 0, sizeof(header.reserved));

            if (try_write(&header, sizeof(header)) < 0) {
                LIME_ERR("failed to write VMA header");
                ret = -EIO;
                goto out;
            }
        }

        /* Dump each page in this VMA */
        for (vaddr = vma->vm_start; vaddr < vma->vm_end; vaddr += PAGE_SIZE) {
            struct page *page = NULL;
            void *kaddr;
            int gup_ret;
            size_t bytes_to_copy = PAGE_SIZE;

            /* Handle partial page at end of VMA */
            if (vaddr + PAGE_SIZE > vma->vm_end)
                bytes_to_copy = vma->vm_end - vaddr;

            /*
             * Use get_user_pages_remote() - same function as /proc/<pid>/mem
             * FOLL_FORCE allows access even if page is not accessible normally
             */
            gup_ret = get_user_pages_remote(mm, vaddr, 1, FOLL_FORCE,
                                           &page, NULL, NULL);

            if (gup_ret <= 0) {
                /* Page not present - write zeros or skip */
                if (mode != LIME_MODE_RAW) {
                    memset(page_buf, 0, bytes_to_copy);
                    if (write_vaddr(page_buf, bytes_to_copy) < 0) {
                        ret = -EIO;
                        goto out;
                    }
                }
                continue;
            }

            /* Map the physical page to kernel virtual address */
            kaddr = kmap(page);

            /* Copy page content to our buffer */
            memcpy(page_buf, kaddr, bytes_to_copy);

            /* Unmap and release the page */
            kunmap(page);
            put_page(page);

            /* Write the page content */
            if (write_vaddr(page_buf, bytes_to_copy) < 0) {
                LIME_ERR("write failed at vaddr %lx", vaddr);
                ret = -EIO;
                goto out;
            }

            nr_dumped++;
        }
    }

    LIME_INFO("process %d dump complete: %lu pages (%lu dumped)",
              pid, nr_pages, nr_dumped);

out:
    mmap_read_unlock(mm);
    mmput(mm);

    /* Flush any remaining compressed data */
#ifdef LIME_SUPPORTS_DEFLATE
    if (compress) {
        err = deflate_end_stream();
        if (err < 0)
            ret = err;
    }
#endif

    /* Write digest if computed */
    if (compute_digest == LIME_DIGEST_COMPLETE) {
        if (method == LIME_METHOD_TCP)
            ldigest_write_tcp();
        else
            ldigest_write_disk();
        ldigest_clean();
    }

    write_flush();
    free_page((unsigned long)page_buf);
    cleanup();

    return ret;
}

/*
 * Perform memory acquisition
 * This is called either from module init (traditional mode) or from sysfs trigger
 */
int lime_do_acquisition(void)
{
    int ret;
    const char *fmt;

    /*
     * Use sysfs format if set, otherwise use module parameter format
     */
    fmt = lime_format ? lime_format : format;

    if (!path || !path[0]) {
        DBG("No path parameter specified");
        return -EINVAL;
    }

    if (!fmt || !fmt[0]) {
        DBG("No format parameter specified");
        return -EINVAL;
    }

    DBG("Parameters");
    DBG("  PATH: %s", path);
    DBG("  DIO: %u", dio);
    DBG("  FORMAT: %s", fmt);
    DBG("  LOCALHOSTONLY: %u", localhostonly);
    DBG("  DIGEST: %s", digest);
    DBG("  TARGET_PID: %d", lime_target_pid);

#ifdef LIME_SUPPORTS_TIMING
    DBG("  TIMEOUT: %lu", timeout);
#endif

#ifdef LIME_SUPPORTS_DEFLATE
    DBG("  COMPRESS: %u", compress);
#endif

    ret = parse_format(fmt);
    if (ret) {
        DBG("Invalid format parameter specified.");
        return ret;
    }

    method = (sscanf(path, "tcp:%d", &port) == 1) ? LIME_METHOD_TCP : LIME_METHOD_DISK;
    if (digest)
        compute_digest = LIME_DIGEST_COMPUTE;
    else
        compute_digest = 0;

    /* Check if we're doing process-specific or full memory dump */
    if (lime_target_pid > 0) {
        LIME_INFO("process-specific dump: pid=%d", lime_target_pid);
        return lime_dump_process(lime_target_pid);
    } else {
        LIME_INFO("full physical memory dump");
        return init();
    }
}

static int __init lime_init_module(void)
{
    int ret;

    LIME_INFO("Linux Memory Extractor v1.9.1 loading");
    DBG("LiME module loading...");

    /* Always initialize sysfs interface */
    ret = lime_sysfs_init();
    if (ret) {
        LIME_ERR("failed to initialize sysfs interface: %d", ret);
        return ret;
    }

    /*
     * When built as built-in (CONFIG_LIME_MEM=y), default to sysfs-only mode
     * When built as module with sysfs_only=1, also use sysfs-only mode
     * Otherwise, if path and format are provided, auto-start acquisition
     */
#ifdef MODULE
    if (sysfs_only) {
        LIME_INFO("sysfs-only mode: use /sys/kernel/lime/trigger to start");
        DBG("Sysfs-only mode enabled, use /sys/kernel/lime/trigger to start acquisition");
        return 0;
    }

    /* Traditional module behavior: if params provided, auto-start */
    if (path && format) {
        LIME_INFO("auto-start mode: acquiring memory to %s", path);
        ret = lime_do_acquisition();
        if (ret) {
            lime_set_state(LIME_STATE_ERROR);
            LIME_ERR("acquisition failed: %d", ret);
        } else {
            lime_set_state(LIME_STATE_COMPLETE);
            LIME_INFO("acquisition complete");
        }
        /*
         * For traditional usage, return error to unload module after acquisition
         * This maintains backward compatibility
         */
        lime_sysfs_cleanup();
        return ret;
    }

    /* No params and not sysfs_only - wait for sysfs trigger */
    LIME_INFO("waiting for trigger via /sys/kernel/lime/");
    DBG("No path/format specified, use /sys/kernel/lime/trigger to start acquisition");
#else
    /* Built-in: always use sysfs interface */
    LIME_INFO("built-in mode: use /sys/kernel/lime/trigger to start");
    DBG("Built-in mode: use /sys/kernel/lime/trigger to start acquisition");
#endif

    return 0;
}

static int init(void) {
    struct resource *p;
    int err = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
    resource_size_t p_last = -1;
#else
    __PTRDIFF_TYPE__ p_last = -1;
#endif

    DBG("Initializing Dump...");

    if ((err = setup())) {
        DBG("Setup Error");
        cleanup();
        return err;
    }

    if (digest) {
        compute_digest = ldigest_init();
        no_overlap = 1;
    }

    vpage = (void *) __get_free_page(GFP_NOIO);

#ifdef LIME_SUPPORTS_DEFLATE
    if (compress) {
        deflate_page_buf = kmalloc(PAGE_SIZE, GFP_NOIO);
        err = deflate_begin_stream(deflate_page_buf, PAGE_SIZE);
        if (err < 0) {
            DBG("ZLIB begin stream failed");
            return err;
        }
        no_overlap = 1;
    }
#endif

    for (p = iomem_resource.child; p ; p = p->sibling) {

        if (!p->name || strcmp(p->name, LIME_RAMSTR))
            continue;

        if (mode == LIME_MODE_LIME && write_lime_header(p) < 0) {
            DBG("Error writing header 0x%lx - 0x%lx", (long) p->start, (long) p->end);
            break;
        } else if (mode == LIME_MODE_PADDED && write_padding((size_t) ((p->start - 1) - p_last)) < 0) {
            DBG("Error writing padding 0x%lx - 0x%lx", (long) p_last, (long) p->start - 1);
            break;
        }

        write_range(p);

        p_last = p->end;
    }

    write_flush();

    DBG("Memory Dump Complete...");

    cleanup();

    if (compute_digest == LIME_DIGEST_COMPUTE) {
        DBG("Writing Out Digest.");

        compute_digest = ldigest_final();

        if (compute_digest == LIME_DIGEST_COMPLETE) {
            if (method == LIME_METHOD_TCP)
                err = ldigest_write_tcp();
            else
                err = ldigest_write_disk();

            DBG("Digest Write %s.", (err == 0) ? "Complete" : "Failed");
        }
    }

    if (digest)
        ldigest_clean();

#ifdef LIME_SUPPORTS_DEFLATE
    if (compress) {
        deflate_end_stream();
        kfree(deflate_page_buf);
    }
#endif

    free_page((unsigned long) vpage);

    return 0;
}

static ssize_t write_lime_header(struct resource * res) {
    lime_mem_range_header header;

    memset(&header, 0, sizeof(lime_mem_range_header));
    header.magic = LIME_MAGIC;
    header.version = 1;
    header.s_addr = res->start;
    header.e_addr = res->end;

    return write_vaddr(&header, sizeof(lime_mem_range_header));
}

static ssize_t write_padding(size_t s) {
    size_t i = 0;
    ssize_t r;

    memset(vpage, 0, PAGE_SIZE);

    while(s -= i) {

        i = min((size_t) PAGE_SIZE, s);
        r = write_vaddr(vpage, i);

        if (r != i) {
            DBG("Error sending zero page: %zd", r);
            return r;
        }
    }

    return 0;
}

static void write_range(struct resource * res) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
    resource_size_t i, is;
#else
    __PTRDIFF_TYPE__ i, is;
#endif
    struct page * p;
    void * v;

    ssize_t s;

#ifdef LIME_SUPPORTS_TIMING
    ktime_t start,end;
#endif

    DBG("Writing range %llx - %llx.", res->start, res->end);

    for (i = res->start; i <= res->end; i += is) {
#ifdef LIME_SUPPORTS_TIMING
        start = ktime_get_real();
#endif
        p = pfn_to_page((i) >> PAGE_SHIFT);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
        is = min((resource_size_t) PAGE_SIZE, (resource_size_t) (res->end - i + 1));
#else
        is = min((size_t) PAGE_SIZE, (size_t) (res->end - i + 1));
#endif

        if (is < PAGE_SIZE) {
            // We can't map partial pages and
            // the linux kernel doesn't use them anyway
            DBG("Padding partial page: vaddr %p size: %lu", (void *) i, (unsigned long) is);
            write_padding(is);
        } else {
#ifdef LIME_USE_KMAP_ATOMIC
            v = kmap_atomic(p);
#else
            v = kmap(p);
#endif
            /*
             * If we need to compute the digest or compress the output
             * take a snapshot of the page. Otherwise save some cycles.
             */
#ifdef LIME_USE_KMAP_ATOMIC
            preempt_enable();
#endif
            if (no_overlap) {
                copy_page(vpage, v);
                s = write_vaddr(vpage, is);
            } else {
                s = write_vaddr(v, is);
            }
#ifdef LIME_USE_KMAP_ATOMIC
            preempt_disable();
            kunmap_atomic(v);
#else
            kunmap(p);
#endif
            if (s < 0) {
                DBG("Failed to write page: vaddr %p. Skipping Range...", v);
                break;
            }
        }

#ifdef LIME_SUPPORTS_TIMING
        end = ktime_get_real();

        if (timeout > 0 && ktime_to_ms(ktime_sub(end, start)) > timeout) {
            DBG("Reading is too slow.  Skipping Range...");
            write_padding(res->end - i + 1 - is);
            break;
        }
#endif

    }
}

static ssize_t write_vaddr(void * v, size_t is) {
    ssize_t ret;

    if (compute_digest == LIME_DIGEST_COMPUTE)
        compute_digest = ldigest_update(v, is);

#ifdef LIME_SUPPORTS_DEFLATE
    if (compress) {
        /* Run deflate() on input until output buffer is not full. */
        do {
            ret = try_write(deflate_page_buf, deflate(v, is));
            if (ret < 0)
                return ret;
        } while (ret == PAGE_SIZE);
        return is;
    }
#endif

    ret = try_write(v, is);
    return ret;
}

static ssize_t write_flush(void) {
#ifdef LIME_SUPPORTS_DEFLATE
    if (compress) {
        try_write(deflate_page_buf, deflate(NULL, 0));
    }
#endif
    return 0;
}

static ssize_t try_write(void * v, ssize_t is) {
    ssize_t ret;

    if (is <= 0)
        return is;

    ret = RETRY_IF_INTERRUPTED(
        (method == LIME_METHOD_TCP) ? write_vaddr_tcp(v, is) : write_vaddr_disk(v, is)
    );

    if (ret < 0) {
        DBG("Write error: %zd", ret);
    } else if (ret != is) {
        DBG("Short write %zu instead of %zu.", ret, is);
        ret = -1;
    }

    return ret;
}

static int setup(void) {
    return (method == LIME_METHOD_TCP) ? setup_tcp() : setup_disk(path, dio);
}

static void cleanup(void) {
    return (method == LIME_METHOD_TCP) ? cleanup_tcp() : cleanup_disk();
}

static void __exit lime_cleanup_module(void)
{
    DBG("LiME module unloading...");
    lime_sysfs_cleanup();
}

module_init(lime_init_module);
module_exit(lime_cleanup_module);

MODULE_LICENSE("GPL");
