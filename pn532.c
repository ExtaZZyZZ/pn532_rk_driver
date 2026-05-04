// SPDX-License-Identifier: GPL-2.0
/*
 * PN532 NFC UART chardev driver for RK3568 / Diasom SOM
 *
 * Захватывает /dev/ttyS7 через tty_kopen_exclusive(),
 * устанавливает собственный line discipline N_DEVELOPMENT (26),
 * регистрирует /dev/pn532 как chardev.
 *
 * Userspace пишет HSU-фреймы через write(), читает ответы через read().
 * ioctl(PN532_IOC_WAKEUP) отправляет wake-up последовательность.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/termios.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "pn532_ioctl.h"

#define DRIVER_NAME     "pn532"
#define RX_FIFO_SIZE    1024

/*
 * N_DEVELOPMENT (26) — зарезервирован для разработки/отладки.
 * Не конфликтует со стандартными line disciplines.
 */
#define N_PN532         26

static char *uart_port = "/dev/ttyS7";
module_param(uart_port, charp, 0444);
MODULE_PARM_DESC(uart_port, "TTY device path (default /dev/ttyS7)");

struct pn532_dev {
    struct tty_struct       *tty;
    int                      old_ldisc_num;
    DECLARE_KFIFO(rx_fifo, u8, RX_FIFO_SIZE);
    wait_queue_head_t        rx_wq;
    spinlock_t               lock;
    struct mutex             write_lock;
    struct cdev              cdev;
    dev_t                    devno;
    struct class            *class;
    atomic_t                 open_count;
};

static struct pn532_dev *pn532_global;

/* ─── Line discipline callbacks ──────────────────────────────────────── */

static void pn532_ldisc_receive_buf(struct tty_struct *tty,
                                    const u8 *cp, const u8 *fp,
                                    size_t count)
{
    struct pn532_dev *dev = tty->disc_data;
    unsigned int pushed;

    if (!dev)
        return;

    spin_lock(&dev->lock);
    pushed = kfifo_in(&dev->rx_fifo, cp, count);
    spin_unlock(&dev->lock);

    if (pushed)
        wake_up_interruptible(&dev->rx_wq);
}

static ssize_t pn532_ldisc_read(struct tty_struct *tty, struct file *file,
                                u8 *buf, size_t nr, void **cookie,
                                unsigned long offset)
{
    /* Чтение идёт через /dev/pn532, не напрямую из ldisc */
    return 0;
}

static int pn532_ldisc_open(struct tty_struct *tty)
{
    tty->disc_data = pn532_global;
    tty->receive_room = 65536;
    return 0;
}

static void pn532_ldisc_close(struct tty_struct *tty)
{
    tty->disc_data = NULL;
}

static struct tty_ldisc_ops pn532_ldisc_ops = {
    .owner        = THIS_MODULE,
    .num          = N_PN532,
    .name         = "pn532",
    .open         = pn532_ldisc_open,
    .close        = pn532_ldisc_close,
    .receive_buf  = pn532_ldisc_receive_buf,
    .read         = pn532_ldisc_read,
};

/* ─── /dev/pn532 file operations ─────────────────────────────────────── */

static int pn532_fop_open(struct inode *inode, struct file *file)
{
    struct pn532_dev *dev = container_of(inode->i_cdev,
                                         struct pn532_dev, cdev);
    if (atomic_inc_return(&dev->open_count) > 1) {
        atomic_dec(&dev->open_count);
        return -EBUSY;
    }
    file->private_data = dev;
    return 0;
}

static int pn532_fop_release(struct inode *inode, struct file *file)
{
    struct pn532_dev *dev = file->private_data;
    atomic_dec(&dev->open_count);
    return 0;
}

static ssize_t pn532_fop_write(struct file *file,
                               const char __user *ubuf,
                               size_t count, loff_t *ppos)
{
    struct pn532_dev *dev = file->private_data;
    u8 *kbuf;
    int ret;

    if (count == 0 || count > 265)  /* максимальный PN532 extended frame */
        return -EINVAL;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, ubuf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    mutex_lock(&dev->write_lock);

    /* Сбрасываем RX FIFO перед отправкой новой команды */
    spin_lock_irq(&dev->lock);
    kfifo_reset(&dev->rx_fifo);
    spin_unlock_irq(&dev->lock);

    ret = dev->tty->ops->write(dev->tty, kbuf, count);

    mutex_unlock(&dev->write_lock);
    kfree(kbuf);

    return (ret < 0) ? ret : (ssize_t)count;
}

static ssize_t pn532_fop_read(struct file *file,
                              char __user *ubuf,
                              size_t count, loff_t *ppos)
{
    struct pn532_dev *dev = file->private_data;
    u8 byte;
    size_t i = 0;
    int ret;

    if (count == 0)
        return 0;

    /* Ждём хотя бы первый байт (таймаут 1с) */
    ret = wait_event_interruptible_timeout(dev->rx_wq,
              !kfifo_is_empty(&dev->rx_fifo),
              HZ);
    if (ret == 0)
        return -ETIMEDOUT;
    if (ret < 0)
        return ret;

    /* Небольшая пауза чтобы накопился весь фрейм ответа */
    msleep(20);

    /* Вычитываем сколько есть, но не больше count */
    while (i < count) {
        spin_lock_irq(&dev->lock);
        ret = kfifo_out(&dev->rx_fifo, &byte, 1);
        spin_unlock_irq(&dev->lock);

        if (!ret)
            break;

        if (put_user(byte, ubuf + i))
            return -EFAULT;
        i++;
    }

    return (ssize_t)i;
}

static long pn532_fop_ioctl(struct file *file,
                            unsigned int cmd, unsigned long arg)
{
    struct pn532_dev *dev = file->private_data;
    static const u8 wakeup[16] = {
        0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
        0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    };

    switch (cmd) {
    case PN532_IOC_WAKEUP:
        mutex_lock(&dev->write_lock);
        spin_lock_irq(&dev->lock);
        kfifo_reset(&dev->rx_fifo);
        spin_unlock_irq(&dev->lock);
        dev->tty->ops->write(dev->tty, wakeup, sizeof(wakeup));
        mutex_unlock(&dev->write_lock);
        msleep(50);
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations pn532_fops = {
    .owner          = THIS_MODULE,
    .open           = pn532_fop_open,
    .release        = pn532_fop_release,
    .write          = pn532_fop_write,
    .read           = pn532_fop_read,
    .unlocked_ioctl = pn532_fop_ioctl,
};

/* ─── TTY helpers ────────────────────────────────────────────────────── */

/*
 * Получаем dev_t из имени ttyS<N>.
 * TTY_MAJOR=4 — стандартный major для /dev/ttyS* на всех платформах.
 * Парсим суффикс из строки вида "/dev/ttyS7" → minor=7.
 */
static dev_t pn532_path_to_devt(const char *path)
{
    const char *p;
    unsigned int minor;
    int ret;

    /* Ищем "ttyS" в пути и берём число после */
    p = strstr(path, "ttyS");
    if (!p)
        return (dev_t)-EINVAL;
    p += 4; /* пропускаем "ttyS" */

    ret = kstrtouint(p, 10, &minor);
    if (ret)
        return (dev_t)(unsigned long)ret;

    return MKDEV(4, minor);  /* TTY_MAJOR=4, ttyS<minor> */
}

/* ─── TTY setup ──────────────────────────────────────────────────────── */

static int pn532_tty_configure(struct tty_struct *tty)
{
    struct ktermios kt;
    int ret;

    down_read(&tty->termios_rwsem);
    kt = tty->termios;
    up_read(&tty->termios_rwsem);

    /* 115200 8N1, no flow control */
    kt.c_iflag = IGNBRK | IGNPAR;
    kt.c_oflag = 0;
    kt.c_cflag = CS8 | CREAD | CLOCAL | B115200;
    kt.c_lflag = 0;
    kt.c_cc[VMIN]  = 1;
    kt.c_cc[VTIME] = 0;
    tty_termios_encode_baud_rate(&kt, 115200, 115200);

    ret = tty_set_termios(tty, &kt);
    if (ret)
        pr_err("pn532: tty_set_termios failed: %d\n", ret);

    return ret;
}

/* ─── Module init / exit ─────────────────────────────────────────────── */

static int __init pn532_init(void)
{
    struct pn532_dev *dev;
    struct tty_struct *tty;
    dev_t devno_base;
    int ret;

    ret = tty_register_ldisc(&pn532_ldisc_ops);
    if (ret) {
        pr_err("pn532: tty_register_ldisc failed: %d\n", ret);
        return ret;
    }

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        ret = -ENOMEM;
        goto err_ldisc;
    }
    pn532_global = dev;

    spin_lock_init(&dev->lock);
    mutex_init(&dev->write_lock);
    init_waitqueue_head(&dev->rx_wq);
    INIT_KFIFO(dev->rx_fifo);
    atomic_set(&dev->open_count, 0);

    /* Получаем dev_t из строки пути и открываем TTY эксклюзивно */
    {
        dev_t devt = pn532_path_to_devt(uart_port);
        if (IS_ERR_VALUE((unsigned long)devt)) {
            pr_err("pn532: cannot stat %s: %d\n", uart_port, (int)devt);
            ret = (int)devt;
            goto err_free;
        }
        tty = tty_kopen_exclusive(devt);
    }
    if (IS_ERR(tty)) {
        pr_err("pn532: cannot open %s: %ld\n", uart_port, PTR_ERR(tty));
        ret = PTR_ERR(tty);
        goto err_free;
    }
    dev->tty = tty;

    /*
     * Устанавливаем CLOCAL напрямую через write lock до tty_set_termios —
     * это разблокирует tty_set_ldisc который мог бы ждать carrier detect.
     */
    down_write(&tty->termios_rwsem);
    tty->termios.c_cflag |= CLOCAL;
    up_write(&tty->termios_rwsem);

    ret = pn532_tty_configure(tty);
    if (ret)
        goto err_tty;

    /* Запоминаем старый ldisc и ставим наш */
    dev->old_ldisc_num = tty->ldisc->ops->num;
    ret = tty_set_ldisc(tty, N_PN532);
    if (ret) {
        pr_err("pn532: tty_set_ldisc failed: %d\n", ret);
        goto err_tty;
    }

    /* Регистрируем /dev/pn532 */
    ret = alloc_chrdev_region(&devno_base, 0, 1, DRIVER_NAME);
    if (ret)
        goto err_ldisc_restore;
    dev->devno = devno_base;

    cdev_init(&dev->cdev, &pn532_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devno, 1);
    if (ret)
        goto err_unreg;

    dev->class = class_create(DRIVER_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        goto err_cdev;
    }

    device_create(dev->class, NULL, dev->devno, NULL, "pn532");
    pr_info("pn532: ready on %s → /dev/pn532\n", uart_port);
    return 0;

err_cdev:
    cdev_del(&dev->cdev);
err_unreg:
    unregister_chrdev_region(dev->devno, 1);
err_ldisc_restore:
    tty_set_ldisc(tty, dev->old_ldisc_num);
err_tty:
    tty_kclose(tty);
err_free:
    kfree(dev);
    pn532_global = NULL;
err_ldisc:
    tty_unregister_ldisc(&pn532_ldisc_ops);
    return ret;
}

static void __exit pn532_exit(void)
{
    struct pn532_dev *dev = pn532_global;

    device_destroy(dev->class, dev->devno);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devno, 1);
    tty_set_ldisc(dev->tty, dev->old_ldisc_num);
    tty_kclose(dev->tty);
    tty_unregister_ldisc(&pn532_ldisc_ops);
    kfree(dev);
    pr_info("pn532: unloaded\n");
}

module_init(pn532_init);
module_exit(pn532_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PN532 NFC UART chardev driver");
MODULE_AUTHOR("Ilya");
