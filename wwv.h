// A. Sheaff 3/5/2020
// Header for the WWV time/date transmitter kernel driver

/*
 * Modified by Eric Sullivan
 * 4/17/2020
 */
#ifndef WWV_H
#define WWV_H

#include <linux/types.h>
#include <asm/ioctl.h>

// Magic number
#define WWV_MAGIC 0xC1

// IOCTL Write to pass in date/time data
#define WWV_TRANSMIT _IOW(WWV_MAGIC,1,struct tm *)

#endif	// WWV_H
