#include <linux/miscdevice.h>
#include <linux/fs.h>
#include "scth_internal.h"

/* Device-Driver del modulo.
    Rende il modulo raggiungibile da user-space, crea il device file del modulo dove gestisce aspetti di allocaizone major minor number,
        esposizione /dev/scthrottle ecc */

static long scth_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
// Ponte tra device driver e dispatcher ioctl
{
    (void)filp;
    return scth_ioctl_dispatch(cmd, arg);
}

static const struct file_operations scth_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = scth_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = NULL,
#endif
};

static struct miscdevice scth_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = SCTH_DEV_NAME,
    .fops  = &scth_fops,
    .mode  = 0666,
};

int scth_dev_init(void)
// Crea il miscdevice ovvero crea /dev/scthrottle, da qui in poi sarà possibile l'interazione da user-space
{
    return misc_register(&scth_miscdev);
}

void scth_dev_exit(void)
// Deregistra il miscdevice, usato in cleanup
{
    misc_deregister(&scth_miscdev);
}