#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include "protocol.h"

#define VENDOR_ID  0xdead
#define PRODUCT_ID 0xbeef

/* table of devices that work with this driver */
static const struct usb_device_id plug162_table[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    { }                 /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, plug162_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_SKEL_MINOR_BASE 192
#define WRITES_IN_FLIGHT 4

/* Structure to hold all of our device specific stuff */
struct usb_plug162 {
    struct usb_device   *udev;          /* the usb device for this device */
    struct usb_interface    *interface;     /* the interface for this device */
    struct semaphore    limit_sem;      /* limiting the number of writes in progress */
    struct mutex        read_mutex;     /* limit to only one read in progress */
    struct usb_anchor   submitted;      /* in case we need to retract our submissions */
    struct urb      *int_in_urb;       /* the urb to read data with */
    unsigned char   *int_in_buf;
    size_t          int_in_filled;
    size_t          int_in_size;
    size_t          int_out_size;
    __u8            int_in_ep_addr;   
    __u8            int_out_ep_addr;  
    __u8            int_out_ep_interval;
    __u8            int_in_ep_interval;
    int         errors;         /* the last request tanked */
    int         open_count;     /* count the number of openers */
    bool            ongoing_read;       /* a read is going on */
    bool            processed_urb;      /* indicates we haven't processed the urb */
    spinlock_t      err_lock;       /* lock for errors */
    struct kref     kref;
    struct mutex        io_mutex;       /* synchronize I/O with disconnect */
    struct completion   int_in_completion; /* to wait for an ongoing read */
};

#define to_usb_dev(d) container_of(d, struct usb_plug162, kref)

/* static struct usb_driver driver; */
static void plug162_draw_down(struct usb_plug162 *dev);
static struct usb_driver plug162_driver;

static void plug162_delete(struct kref *kref)
{
    struct usb_plug162 *dev = to_usb_dev(kref);

    usb_free_urb(dev->int_in_urb);
    kfree(dev->int_in_buf);
    usb_put_dev(dev->udev);
    kfree(dev);
}

static int plug162_open(struct inode *inode, struct file *file)
{
    struct usb_plug162 *dev;
    struct usb_interface *intf;
    int subminor;
    int ret = 0;
    
    subminor = iminor(inode);
    intf = usb_find_interface(&plug162_driver, subminor);
    
    if (intf == NULL) {
        printk(KERN_ERR "%s - error, can't find device for minor %d\n",
            __func__, subminor);
        ret = -ENODEV;
        goto exit;
    }

    dev = usb_get_intfdata(intf);
    if (dev == NULL) {
        ret = -ENODEV;
        goto exit;
    }

    kref_get(&dev->kref);

    mutex_lock(&dev->io_mutex);

    if (!dev->open_count++) {
        ret = usb_autopm_get_interface(intf);
        if (ret) {
            dev->open_count--;
            mutex_unlock(&dev->io_mutex);
            kref_put(&dev->kref, plug162_delete);
            goto exit;
        }
    }

    file->private_data = dev;
    mutex_unlock(&dev->io_mutex);

exit:
    return ret;
}

static int plug162_release(struct inode *inode, struct file *file)
{
    struct usb_plug162 *dev;

    dev = file->private_data;
    if (dev == NULL)
        return -ENODEV;
    
    /* allow the device to be autosuspended */
    mutex_lock(&dev->io_mutex);
    if (!--dev->open_count && dev->interface)
        usb_autopm_put_interface(dev->interface);
    mutex_unlock(&dev->io_mutex);

    /* decrement the count on our device */
    kref_put(&dev->kref, plug162_delete);
    
    return 0;
}

static int plug162_flush(struct file *file, fl_owner_t id)
{
    return 0;
}

static void plug162_read_int_callback(struct urb *urb)
{
    struct usb_plug162 *dev;
    
    dev = urb->context;
    spin_lock(&dev->err_lock);
    /* sync/async unlink faults aren't errors */
    if (urb->status) {
        if (!(urb->status == -ENOENT ||
            urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN)) {
            printk(KERN_DEBUG "%s - nonzero write int status received: %d",
                __func__, urb->status);
            spin_lock(&dev->err_lock);
            dev->errors = urb->status;
            spin_unlock(&dev->err_lock);
        }
        dev->int_in_filled = 0;
    } else {
        dev->int_in_filled = urb->actual_length;
    }
    spin_unlock(&dev->err_lock);
    complete(&dev->int_in_completion);
}

static int plug162_do_read_io(struct usb_plug162 *dev, size_t count)
{
    int rv;

    rv = mutex_lock_interruptible(&dev->io_mutex);
    if (rv < 0)
        return rv;

    if (dev->interface == NULL) {
        rv = -ENODEV;
        goto exit;
    }

    usb_fill_int_urb(dev->int_in_urb,
            dev->udev,
            usb_rcvintpipe(dev->udev,
                dev->int_in_ep_addr),
            dev->int_in_buf,
            count,
            plug162_read_int_callback,
            dev, dev->int_in_ep_interval);
    
    rv = usb_submit_urb(dev->int_in_urb, GFP_KERNEL);
    if (rv < 0) {
        printk(KERN_ERR "%s - failed submitting read urb, error %d",
            __func__, rv);
        dev->int_in_filled = 0;
        rv = (rv == -ENOMEM) ? rv : -EIO;
    }

exit:
    mutex_unlock(&dev->io_mutex);

    return rv;
}


static ssize_t plug162_read(struct file *file, char *buf, size_t count,
                loff_t *ppos)
{
    
    struct usb_plug162 *dev;
    size_t copy_len;
    int rv = 0;

    dev = file->private_data;
    if (!dev->int_in_urb)
        return 0;
    if (file->f_flags & O_NONBLOCK)
        return -EPERM;
    
    rv = mutex_lock_interruptible(&dev->read_mutex);
    if (rv < 0)
        return rv;
    
    rv = plug162_do_read_io(dev, dev->int_in_size);
    if (rv < 0) 
        goto exit;

    rv = wait_for_completion_interruptible(&dev->int_in_completion);
    if (rv < 0) {
        printk(KERN_DEBUG "Wait for completion interrupted");
        mutex_lock(&dev->io_mutex);
        if (dev->interface != NULL)
            usb_kill_urb(dev->int_in_urb);
        mutex_unlock(&dev->io_mutex);
        init_completion(&dev->int_in_completion);
        goto exit;
    }

    copy_len = min(count, dev->int_in_filled);
    if (copy_to_user(buf, dev->int_in_buf, copy_len))
        rv = -EFAULT;
    else
        rv = copy_len;

exit:
    mutex_unlock(&dev->read_mutex);

    return rv;
}

static void plug162_write_int_callback(struct urb *urb)
{
    struct usb_plug162 *dev;

    dev = urb->context;

    if (urb->status) {
        if (!(urb->status == -ENOENT ||
            urb->status == -ECONNRESET || 
            urb->status == -ESHUTDOWN)) {
            printk(KERN_DEBUG "%s - nonzero write int status received: %d",
                    __func__, urb->status);
            spin_lock(&dev->err_lock);
            dev->errors = urb->status;
            spin_unlock(&dev->err_lock);
        }

    }
    
    kfree(urb->transfer_buffer);
    up(&dev->limit_sem);
}


static ssize_t plug162_write(struct file *file, const char *user_buf, 
                size_t count, loff_t *ppos)
{
    struct usb_plug162 *dev;
    struct urb *urb = NULL;
    unsigned char *buf = NULL;
    size_t write_size;
    int rv = 0;

    dev = file->private_data;
    write_size = min(count, dev->int_out_size);
    
    if (count == 0)
        goto exit;

    if (!(file->f_flags & O_NONBLOCK)) {
        if (down_interruptible(&dev->limit_sem)) {
            rv = -ERESTARTSYS;
            goto exit;
        }
    } else {
        if (down_trylock(&dev->limit_sem)) {
            rv = -EAGAIN;
            goto exit;
        }
    }

    spin_lock_irq(&dev->err_lock);
    rv = dev->errors;
    if (rv < 0) {
        dev->errors = 0;
        rv = (rv == -EPIPE) ? rv : -EIO;
    }
    spin_unlock_irq(&dev->err_lock);
    if (rv < 0)
        goto error;

    urb = usb_alloc_urb(0, GFP_KERNEL);
    if (urb == NULL) {
        rv = -ENOMEM;
        goto error;
    }
    
    buf = kmalloc(write_size, GFP_KERNEL);
    if (urb == NULL) {
        rv = -ENOMEM;
        goto error;
    }

    if (copy_from_user(buf, user_buf, write_size)) {
        rv = -EFAULT;
        goto error;
    }

    mutex_lock(&dev->io_mutex);
    if (dev->interface == NULL) {
        mutex_unlock(&dev->io_mutex);
        rv = -ENODEV;
        goto error;
    }
    
    usb_fill_int_urb(urb, dev->udev,
                usb_sndintpipe(dev->udev, dev->int_out_ep_addr),
                buf, write_size, plug162_write_int_callback, dev,
                dev->int_out_ep_interval);
    usb_anchor_urb(urb, &dev->submitted);

    rv = usb_submit_urb(urb, GFP_KERNEL);
    mutex_unlock(&dev->io_mutex);
    if (rv < 0) {
        printk(KERN_ERR "%s - failed submitting write urb, error %d",
                __func__, rv);
        goto error_unanchor;
    }

    usb_free_urb(urb);

    return write_size;

error_unanchor:
    usb_unanchor_urb(urb);

error:
    printk(KERN_DEBUG "ERROR");
    if (urb) {
        kfree(buf);
        usb_free_urb(urb);
    }
    
    up(&dev->limit_sem);
exit:
    return rv;
}


static const struct file_operations plug162_fops = {
    .owner =    THIS_MODULE,
    .read =     plug162_read,
    .write =    plug162_write,
    .open =     plug162_open,
    .release =  plug162_release,
    .flush =    plug162_flush,
    .llseek =   noop_llseek,
};

static struct usb_class_driver plug162_class = {
    .name =     "plug162%d",
    .fops =     &plug162_fops,
    .minor_base =   USB_SKEL_MINOR_BASE,
};

static int plug162_probe(struct usb_interface *interface, 
                const struct usb_device_id *id)
{
    struct usb_plug162 *dev;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *ep;
    int i;
    int ret = -ENOMEM;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (dev == NULL) {
        printk(KERN_WARNING "Out of memory\n");
        goto error;
    }
    kref_init(&dev->kref);
    sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
    mutex_init(&dev->read_mutex);
    mutex_init(&dev->io_mutex);
    spin_lock_init(&dev->err_lock);
    init_usb_anchor(&dev->submitted);
    init_completion(&dev->int_in_completion);

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = interface;

    iface_desc = interface->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
        ep = &iface_desc->endpoint[i].desc;

        if (!dev->int_in_ep_addr && usb_endpoint_is_int_in(ep)) {
            dev->int_in_ep_addr = ep->bEndpointAddress;
            dev->int_in_ep_interval = ep->bInterval;
            dev->int_in_size = usb_endpoint_maxp(ep);
            dev->int_in_buf = kmalloc(dev->int_in_size, GFP_KERNEL);
            if (dev->int_in_buf == NULL) {
                printk(KERN_ERR "Could not allocate int_in_buf");
                goto error;
            }
            dev->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
            if (dev->int_in_urb == NULL) {
                printk(KERN_ERR "Could not allocate int_in_urb\n");
                goto error;
            }
        }

        if (!dev->int_out_ep_addr && 
            usb_endpoint_is_int_out(ep)) {
            dev->int_out_size = usb_endpoint_maxp(ep);
            dev->int_out_ep_addr = ep->bEndpointAddress;
            dev->int_out_ep_interval = ep->bInterval;
        }
    }
    if (!(dev->int_in_ep_addr && dev->int_out_ep_addr)) {
        printk(KERN_DEBUG "Could not find both int-in and int-out endpoints\n");
        goto error;
    }

    usb_set_intfdata(interface, dev);
    ret = usb_register_dev(interface, &plug162_class);
    if (ret == -EINVAL) {
        printk(KERN_DEBUG "Not able to get a minor for this device\n");
        usb_set_intfdata(interface, NULL);
        goto error;
    }

    dev_info(&interface->dev, 
        "USB Plug162 device now attached to USBPlug162-%d",
        interface->minor);

    return 0;

error:
    if (dev)
        kref_put(&dev->kref, plug162_delete);

    return ret;
}

static void plug162_disconnect(struct usb_interface *interface)
{
    struct usb_plug162 *dev;
    int minor = interface->minor;
    
    dev = usb_get_intfdata(interface);
    usb_set_intfdata(interface, NULL);

    usb_deregister_dev(interface, &plug162_class);
    
    mutex_lock(&dev->io_mutex);
    dev->interface = NULL;
    mutex_unlock(&dev->io_mutex);

    usb_kill_anchored_urbs(&dev->submitted);
    usb_kill_urb(dev->int_in_urb);
    
    kref_put(&dev->kref, plug162_delete);
    dev_info(&interface->dev, "USB Plug162 #%d now disconnected", minor);
}

static void plug162_draw_down(struct usb_plug162 *dev)
{
    int time;

    time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
    if (!time)
        usb_kill_anchored_urbs(&dev->submitted);
    usb_kill_urb(dev->int_in_urb);
}

static int plug162_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct usb_plug162 *dev = usb_get_intfdata(intf);

    if (dev == NULL)
        return 0;
    plug162_draw_down(dev);

    return 0;
}

static int plug162_resume(struct usb_interface *intf)
{
    return 0;
}

static int plug162_pre_reset(struct usb_interface *intf)
{
    struct usb_plug162 *dev = usb_get_intfdata(intf);

    mutex_lock(&dev->io_mutex);
    plug162_draw_down(dev);

    return 0;
}

static int plug162_post_reset(struct usb_interface *intf)
{
    struct usb_plug162 *dev = usb_get_intfdata(intf);

    /* we are sure no URBs are active - no locking needed */
    dev->errors = -EPIPE;
    mutex_unlock(&dev->io_mutex);

    return 0;
}


static struct usb_driver plug162_driver = {
    .name =     "usb-plug162",
    .probe =    plug162_probe,
    .disconnect =   plug162_disconnect,
    .suspend =  plug162_suspend,
    .resume =   plug162_resume,
    .pre_reset =    plug162_pre_reset,
    .post_reset =   plug162_post_reset,
    .id_table = plug162_table,
    .supports_autosuspend = 1,
};

static int __init usb_plug162_init(void)
{
    int result;

    result = usb_register(&plug162_driver);
    if (result)
        printk(KERN_DEBUG "usb_register failed. Error number %d\n", result);

    return result;
}

static void __exit usb_plug162_exit(void)
{
    usb_deregister(&plug162_driver);
}

module_init(usb_plug162_init);
module_exit(usb_plug162_exit);

MODULE_LICENSE("GPL");
