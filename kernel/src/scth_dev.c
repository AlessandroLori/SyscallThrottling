#include <linux/miscdevice.h>
#include <linux/fs.h>

#include "scth_internal.h"

static long scth_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    (void)filp;
    return scth_ioctl_dispatch(cmd, arg);
}

static const struct file_operations scth_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = scth_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    /* per il progetto: userland 64-bit only */
    .compat_ioctl   = NULL,
#endif
};

static struct miscdevice scth_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = SCTH_DEV_NAME,
    .fops  = &scth_fops,
    .mode  = 0666, /* puoi cambiarlo a 0600 se preferisci */
};

int scth_dev_init(void)
{
    return misc_register(&scth_miscdev);
}

void scth_dev_exit(void)
{
    misc_deregister(&scth_miscdev);
}