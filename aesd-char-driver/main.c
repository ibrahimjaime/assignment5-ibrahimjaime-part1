/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Ibrahim");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                   loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = &aesd_device;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_available;
    size_t bytes_to_copy;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    // Step A: find which entry contains the byte at position *f_pos,
    // and how far into that entry we should start reading
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer,
                                                             *f_pos, &entry_offset);
    if (entry == NULL) {
        // f_pos is past all stored data — nothing left to read (EOF)
        retval = 0;
        goto out;
    }

    // Step B: figure out how many bytes we can actually copy this call
    bytes_available = entry->size - entry_offset;
    bytes_to_copy = min(count, bytes_available);

    // Step C: copy those bytes out to userspace
    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    // Step D: advance the file offset so the next read() continues correctly
    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = &aesd_device;
    char *new_buffer;
    char *newline_pos;
    size_t total_size;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    // Step A: get exclusive access — no other read/write can run at the same time
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    // Step B: grow pending_buffer to fit the new data
    total_size = dev->pending_size + count;
    new_buffer = krealloc(dev->pending_buffer, total_size, GFP_KERNEL);
    if (!new_buffer) {
        retval = -ENOMEM;
        goto out;
    }
    dev->pending_buffer = new_buffer;

    // Step C: copy the new data from userspace, appended after existing pending data
    if (copy_from_user(dev->pending_buffer + dev->pending_size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    dev->pending_size = total_size;

    // Step D: check if we now have a complete command (i.e. contains '\n')
    newline_pos = memchr(dev->pending_buffer, '\n', dev->pending_size);

    if (newline_pos == NULL) {
        // No newline yet — just keep accumulating, nothing to commit
        retval = count;
        goto out;
    }

    {
        size_t command_len = (newline_pos - dev->pending_buffer) + 1; // include the '\n' itself
        size_t leftover_len = dev->pending_size - command_len;
        struct aesd_buffer_entry new_entry;
        struct aesd_buffer_entry *old_entry;

        // Step E: if the circular buffer is full, the oldest entry is about
        // to get silently overwritten by add_entry(). We must free its memory
        // ourselves first, or it leaks.
        if (dev->circular_buffer.full) {
            old_entry = &dev->circular_buffer.entry[dev->circular_buffer.out_offs];
            kfree(old_entry->buffptr);
        }

        // Step F: package the completed command (pending_buffer up through the \n)
        new_entry.buffptr = dev->pending_buffer;
        new_entry.size = command_len;
        aesd_circular_buffer_add_entry(&dev->circular_buffer, &new_entry);

        // Step G: handle leftover bytes after the \n (e.g. user wrote two
        // commands in a single write() call: "cmd1\ncmd2")
        if (leftover_len > 0) {
            char *leftover = kmalloc(leftover_len, GFP_KERNEL);
            if (!leftover) {
                retval = -ENOMEM;
                goto out;
            }
            memcpy(leftover, newline_pos + 1, leftover_len);
            dev->pending_buffer = leftover;
            dev->pending_size = leftover_len;
        } else {
            // No leftover — pending_buffer's ownership was handed to the
            // circular buffer entry (new_entry.buffptr), so just reset our pointer
            dev->pending_buffer = NULL;
            dev->pending_size = 0;
        }

        retval = count;
    }

out:
    mutex_unlock(&dev->lock);
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    uint8_t index;
    struct aesd_buffer_entry *entry;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    // free every stored command still sitting in the circular buffer
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
        }
    }

    // free any incomplete write that never got a trailing \n
    if (aesd_device.pending_buffer) {
        kfree(aesd_device.pending_buffer);
    }

    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}


module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
