#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>   // for task_struct, send_sig
#include <linux/mm.h>             // for get_mm_rss
#include <linux/kthread.h>        // for kernel thread
#include <linux/delay.h>          // for msleep

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

/* Device and driver structures */
static dev_t dev_number;
static struct cdev monitor_cdev;
static struct class *monitor_class;

/* Kernel thread for periodic monitoring */
static struct task_struct *monitor_thread;

/* -------- Container Metadata Structure --------
 * Stores information about each registered container
 */
struct container_entry {
    pid_t pid;                    // Host PID of container
    char id[32];                 // Container name/ID
    unsigned long soft_limit;    // Soft memory limit (bytes)
    unsigned long hard_limit;    // Hard memory limit (bytes)
    struct container_entry *next;
};

/* Head of linked list storing all containers */
static struct container_entry *head = NULL;

/* Mutex to protect list access */
static DEFINE_MUTEX(list_lock);


/* -------- KERNEL MONITOR THREAD --------
 * Runs periodically to:
 * 1. Check memory usage (RSS) of each container
 * 2. Compare with soft/hard limits
 * 3. Log warnings or kill process if needed
 */
static int monitor_fn(void *data)
{
    while (!kthread_should_stop()) {

        struct container_entry *cur;

        /* Lock list before traversal */
        mutex_lock(&list_lock);

        cur = head;
        while (cur) {

            struct task_struct *task;
            unsigned long rss_bytes = 0;

            /* Get task_struct from PID */
            task = pid_task(find_vpid(cur->pid), PIDTYPE_PID);

            /* Ensure process exists and has memory descriptor */
            if (task && task->mm) {

                /* get_mm_rss returns number of pages
                 * convert to bytes using PAGE_SHIFT
                 */
                rss_bytes = get_mm_rss(task->mm) << PAGE_SHIFT;

                /* HARD LIMIT: kill process */
                if (rss_bytes > cur->hard_limit) {
                    printk(KERN_WARNING
                        "[monitor] HARD limit exceeded for %s (pid=%d, rss=%lu bytes)\n",
                        cur->id, cur->pid, rss_bytes);

                    /* Send SIGKILL to terminate process */
                    send_sig(SIGKILL, task, 0);
                }

                /* SOFT LIMIT: just warning */
                else if (rss_bytes > cur->soft_limit) {
                    printk(KERN_WARNING
                        "[monitor] SOFT limit exceeded for %s (pid=%d, rss=%lu bytes)\n",
                        cur->id, cur->pid, rss_bytes);
                }
            }

            cur = cur->next;
        }

        mutex_unlock(&list_lock);

        /* Sleep before next check (1 second interval) */
        msleep(1000);
    }

    return 0;
}


/* -------- IOCTL HANDLER --------
 * Handles communication from user-space runtime
 */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    /* Copy request from user space */
    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    /* -------- REGISTER CONTAINER -------- */
    if (cmd == MONITOR_REGISTER) {

        struct container_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;

        /* Copy container ID safely */
        strncpy(entry->id, req.container_id, sizeof(entry->id) - 1);
        entry->id[sizeof(entry->id) - 1] = '\0'; // ensure null termination

        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;

        /* Insert into linked list */
        mutex_lock(&list_lock);
        entry->next = head;
        head = entry;
        mutex_unlock(&list_lock);

        printk(KERN_INFO
            "[monitor] Registered container %s (pid=%d)\n",
            entry->id, entry->pid);
    }

    /* -------- UNREGISTER CONTAINER -------- */
    else if (cmd == MONITOR_UNREGISTER) {

        struct container_entry **cur;

        mutex_lock(&list_lock);

        cur = &head;
        while (*cur) {
            if ((*cur)->pid == req.pid) {

                struct container_entry *tmp = *cur;
                *cur = (*cur)->next;
                kfree(tmp);

                printk(KERN_INFO
                    "[monitor] Unregistered pid=%d\n",
                    req.pid);
                break;
            }
            cur = &(*cur)->next;
        }

        mutex_unlock(&list_lock);
    }

    return 0;
}


/* -------- FILE OPERATIONS -------- */
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};


/* -------- MODULE INIT --------
 * 1. Allocate device number
 * 2. Create char device
 * 3. Create /dev entry
 * 4. Start monitoring thread
 */
static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);

    cdev_init(&monitor_cdev, &fops);
    cdev_add(&monitor_cdev, dev_number, 1);

    monitor_class = class_create(DEVICE_NAME);
    device_create(monitor_class, NULL, dev_number, NULL, DEVICE_NAME);

    /* Start kernel monitoring thread */
    monitor_thread = kthread_run(monitor_fn, NULL, "container_monitor_thread");

    printk(KERN_INFO "[monitor] Module loaded\n");
    return 0;
}


/* -------- MODULE EXIT --------
 * 1. Stop monitoring thread
 * 2. Free linked list
 * 3. Remove device
 */
static void __exit monitor_exit(void)
{
    struct container_entry *cur = head;

    /* Stop monitoring thread safely */
    if (monitor_thread)
        kthread_stop(monitor_thread);

    /* Free all container entries */
    while (cur) {
        struct container_entry *tmp = cur;
        cur = cur->next;
        kfree(tmp);
    }

    device_destroy(monitor_class, dev_number);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(dev_number, 1);

    printk(KERN_INFO "[monitor] Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
