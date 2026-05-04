/* SPDX-License-Identifier: GPL-2.0 */
/* Shared between kernel module and userspace */
#ifndef PN532_IOCTL_H
#define PN532_IOCTL_H

#include <linux/ioctl.h>

#define PN532_IOC_MAGIC  'N'

/* Send 16-byte wake-up sequence (0x55) and wait 50ms */
#define PN532_IOC_WAKEUP  _IO(PN532_IOC_MAGIC, 1)

#endif /* PN532_IOCTL_H */
