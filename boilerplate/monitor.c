#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>   // copy_from_user
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>      // kmalloc
#include <linux/mutex.h>
#include <linux/sched/signal.h> // task_struct, send_sig
#include <linux/mm.h>        // get_mm_rss
#include <linux/kthread.h>   // kernel thread
#include <linux/delay.h>     // msleep

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

/* ---------------- DEVICE SETUP ---------------- */
// These help create /dev/container_monitor
static dev_t dev_number;
static struct cdev monitor_cdev;
static struct class *monitor_class;

/* ---------------- KERNEL THREAD ---------------- */
// This thread will run continuously in background
static struct task_struct *monitor_thread;

/* ---------------- CONTAINER STRUCT ---------------- */
// Stores info about each container
struct container_entry {
    pid_t pid;                 // PID of container process
    char id[32];               // container name (alpha, beta)
    unsigned long soft_limit;  // warning limit (bytes)
    unsigned long hard_limit;  // kill limit (bytes)
    struct container_entry *next; // linked list
};

/* Head of linked list */
static struct container_entry *head = NULL;

/* Mutex to protect list (important for concurrency) */
static DEFINE_MUTEX(list_lock);


/* ---------------- MONITOR THREAD ---------------- */
// Runs every second and checks memory usage
static int monitor_fn(void *data)
{
    while (!kthread_should_stop()) {

        struct container_entry *cur;

        // Lock list before accessing it
        mutex_lock(&list_lock);

        cur = head;
        while (cur) {

            struct task_struct *task;
            unsigned long rss_bytes = 0;

            // Get process from PID
            task = pid_task(find_vpid(cur->pid), PIDTYPE_PID);

            // Check if process exists and has memory
            if (task && task->mm) {

                // Get RSS (in pages) → convert to bytes
                rss_bytes = get_mm_rss(task->mm) << PAGE_SHIFT;

                // HARD LIMIT → kill process
                if (rss_bytes > cur->hard_limit) {
                    printk(KERN_WARNING,
                        "[monitor] HARD limit exceeded for %s (pid=%d)\n",
                        cur->id, cur->pid);

                    // Kill process
                    send_sig(SIGKILL, task, 0);
                }

                // SOFT LIMIT → just warning
                else if (rss_bytes > cur->soft_limit) {
                    printk(KERN_WARNING,
                        "[monitor] SOFT limit exceeded for %s (pid=%d)\n",
                        cur->id, cur->pid);
                }
            }

            cur = cur->next;
        }

        mutex_unlock(&list_lock);

        // Wait 1 second before next check
        msleep(1000);
    }

    return 0;
}


/* ---------------- IOCTL HANDLER ---------------- */
// Handles communication from engine (user space)
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    // Copy data from user space → kernel space
    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    /* -------- REGISTER -------- */
    if (cmd == MONITOR_REGISTER) {

        // Allocate new container entry
        struct container_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;

        // Copy container name safely
        strncpy(entry->id, req.container_id, sizeof(entry->id) - 1);
        entry->id[31] = '\0';

        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;

        // Add to linked list
        mutex_lock(&list_lock);
        entry->next = head;
        head = entry;
        mutex_unlock(&list_lock);

        printk(KERN_INFO,
            "[monitor] Registered container %s (pid=%d)\n",
            entry->id, entry->pid);
    }

    /* -------- UNREGISTER -------- */
    else if (cmd == MONITOR_UNREGISTER) {

        struct container_entry **cur;

        mutex_lock(&list_lock);

        cur = &head;
        while (*cur) {
            if ((*cur)->pid == req.pid) {

                struct container_entry *tmp = *cur;

                // Remove from list
                *cur = (*cur)->next;
                kfree(tmp);

                printk(KERN_INFO,
                    "[monitor] Unregistered pid=%d\n", req.pid);
                break;
            }
            cur = &(*cur)->next;
        }

        mutex_unlock(&list_lock);
    }

    return 0;
}


/* ---------------- FILE OPERATIONS ---------------- */
// Connect ioctl to device
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};


/* ---------------- MODULE INIT ---------------- */
// Runs when module is loaded
static int __init monitor_init(void)
{
    // Allocate device number
    alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);

    // Register device
    cdev_init(&monitor_cdev, &fops);
    cdev_add(&monitor_cdev, dev_number, 1);

    // Create /dev/container_monitor
    monitor_class = class_create(DEVICE_NAME);
    device_create(monitor_class, NULL, dev_number, NULL, DEVICE_NAME);

    // Start monitoring thread
    monitor_thread = kthread_run(monitor_fn, NULL, "monitor_thread");

    printk(KERN_INFO "[monitor] Module loaded\n");
    return 0;
}


/* ---------------- MODULE EXIT ---------------- */
// Runs when module is removed
static void __exit monitor_exit(void)
{
    struct container_entry *cur = head;

    // Stop monitoring thread
    if (monitor_thread)
        kthread_stop(monitor_thread);

    // Free all containers
    while (cur) {
        struct container_entry *tmp = cur;
        cur = cur->next;
        kfree(tmp);
    }

    // Cleanup device
    device_destroy(monitor_class, dev_number);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(dev_number, 1);

    printk(KERN_INFO "[monitor] Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
