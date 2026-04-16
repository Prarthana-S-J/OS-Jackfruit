#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

static dev_t dev_number;
static struct cdev monitor_cdev;
static struct class *monitor_class;

struct container_entry {
    pid_t pid;
    char id[32];
    unsigned long soft_limit;
    unsigned long hard_limit;
    struct container_entry *next;
};

static struct container_entry *head = NULL;
static DEFINE_MUTEX(list_lock);

/* -------- IOCTL HANDLER -------- */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {

        struct container_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        strncpy(entry->id, req.container_id, sizeof(entry->id)-1);
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;

        mutex_lock(&list_lock);
        entry->next = head;
        head = entry;
        mutex_unlock(&list_lock);

        printk(KERN_INFO "[monitor] Registered container %s (pid=%d)\n",
               entry->id, entry->pid);
    }

    else if (cmd == MONITOR_UNREGISTER) {

        struct container_entry **cur;

        mutex_lock(&list_lock);

        cur = &head;
        while (*cur) {
            if ((*cur)->pid == req.pid) {
                struct container_entry *tmp = *cur;
                *cur = (*cur)->next;
                kfree(tmp);

                printk(KERN_INFO "[monitor] Unregistered pid=%d\n", req.pid);
                break;
            }
            cur = &(*cur)->next;
        }

        mutex_unlock(&list_lock);
    }

    return 0;
}

/* -------- FILE OPS -------- */
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* -------- INIT -------- */
static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);

    cdev_init(&monitor_cdev, &fops);
    cdev_add(&monitor_cdev, dev_number, 1);

    monitor_class = class_create(DEVICE_NAME);
    device_create(monitor_class, NULL, dev_number, NULL, DEVICE_NAME);

    printk(KERN_INFO "[monitor] Module loaded\n");
    return 0;
}

/* -------- EXIT -------- */
static void __exit monitor_exit(void)
{
    struct container_entry *cur = head;

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
