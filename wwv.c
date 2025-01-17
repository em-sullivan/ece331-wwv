// A. Sheaff 3/15/2019 - wwv driver
// Framework code for creating a kernel driver
// that creates the digital data from WWV station
// Pass in time/date data through ioctl.

/*
 * Framework modified by Eric Sullivan
 * 03/21/2020
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/time.h>

#include "wwv.h"

// Data to be "passed" around to various functions
struct wwv_data_t {
    struct gpio_desc *gpio_wwv;		// Enable pin
    struct gpio_desc *gpio_unused17;		// Clock pin
    struct gpio_desc *gpio_unused18;		// Bit 0 pin
    struct gpio_desc *gpio_unused22;		// Bit 1 pin
    struct gpio_desc *gpio_shutdown;	// Shutdown input
    int major;			// Device major number
    struct class *wwv_class;	// Class for auto /dev population
    struct device *wwv_dev;	// Device for auto /dev population
    // ADD YOUR LOCKING VARIABLE BELOW THIS LINE
    struct mutex lock;
};

// ADD ANY WWV DEFINE BELOW THIS LINE

// Macros for delays
#define ZBIT 18
#define OBIT 48
#define PINDEX 78
#define ZDELAY 830000
#define ODELAY 530000
#define PDELAY 230000

// Struct that holds date
struct wwv_date {
    int year;
    int min_ones;
    int min_tens;
    int hour_ones;
    int hour_tens;
    int day_ones;
    int day_tens;
    int day_hund;
};

// WWV data structure access between functions
static struct wwv_data_t *wwv_data_fops;

//************************************
// WWV Data format
//         0        1       2          3             4          5           6          7           8       9
// +----+-------+-------+-------+---------------+-----------------------------------------------+-------+-------+
// |P0	|		|		|		|				|		YEAR Units Value BCD LSb First 			|		|		|
// |	|		|		|		|				+-----------+-----------+-----------+-----------+		|		|
// |	|Blank	|Zero	|DST	|Leap Sec Warn	|1's Year	|2's Year	|4's Year	|8's Year	|Zero	|POS ID	|
// +----+-------+-------+-------+---------------+-----------+-----------+-----------+-----------+-------+-------+
//          10          11          12          13        14        15                16            17             18      19
// +----+-----------------------------------------------+-------+-----------------------------------------------+-------+-------+
// |P1	|	 Minute Units Value BCD LSb First			|		|			Minute Tens Value BCD LSb First 	|		|		|
// |	+-----------+-----------+-----------+-----------+		+---------------+---------------+---------------+		|		|
// |	|1's Minute	|2's Minute	|4's Minute	|8's Minute	|Zero	|10's Minute	|20's Minute	|40's Minute	|Zero	|POS ID	|
// +----+-----------+-----------+-----------+-----------+-------+---------------+---------------+---------------+-------+-------+
//          20           21          22         23         24          25            26               27           28      29
// +----+-----------------------------------------------+-------+-----------------------------------------------+-------+-------+
// |P2	|		 Hour Units Value BCD LSb First			|		|			Hour Tens Value BCD LSb First	 	|		|		|
// |	+-----------+-----------+-----------+-----------+		+---------------+---------------+---------------+		|		|
// |	|1's Hour	|2's Hour	|4's Hour	|8's Hour	|Zero	|10's Hour		|20's Hour		|40's Hour		|Zero	|POS ID	|
// +----+-----------+-----------+-----------+-----------+-------+---------------+---------------+---------------+-------+-------+
//             30                   31                 32                  33              34          35                  36                  37                38                39
// +----+-------------------------------------------------------------------------------+-------+-------------------------------------------------------------------------------+-------+
// |P3	|				 Day of Year Units Value BCD LSb First							|		|					Day of Year Tens Value BCD LSb First						|		|
// +    +-------------------+-------------------+-------------------+-------------------+       +-------------------+-------------------+-------------------+-------------------+       +
// |	|1's Day of Year	|2's Day of Year	|4's Day of Year	|8's Day of Year	|Zero	|10's Day of Year	|20's Day of Year	|40's Day of Year	|80's Day of Year	|POS ID	|
// +----+-------------------+-------------------+-------------------+-------------------+-------+-------------------+-------------------+-------------------+-------------------+-------+
//              40                     41                  42      43       44     45      46       47     48      49
// +----+-----------------------------------------------+-------+-------+-------+-------+-------+-------+-------+-------+
// |P4	|	 Day of Year Hundreds Value BCD LSb First	|		|		|		|		|		|		|		|		|
// |	+-----------+-----------+-----------+-----------+		|		|		|		|		|		|		|		|
// |	|100's Day of Year		|200's Day of Year		|Zero	|Zero	|Zero	|Blank	|Blank	|Blank	|Blank	|Blank	|
// +----+-----------------------+-----------------------+-------+-------+-------+-------+-------+-------+-------+-------+
//        50       51      52     53       54      55      56     57       58      59
// +----+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+
// |P5	|Blank	|Blank	|Blank	|Blank	|Blank	|Blank	|Blank	|Blank	|Blank  |Blank  |
// +----+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+

// ADD YOUR WWV ENCODING/TRANSMITING/MANAGEMENT FUNCTIONS BELOW THIS LINE

/* 
 * Seperates year, minutes, hours and days into
 * a wwv_date struct, which stores each place in its
 * own varaible for encoding. Retruns 0 if its a valid date,
 * returns 1 otherwise.
 */
static int wwv_conv_date(struct tm *utc, struct wwv_date *dtime)
{
    // Checks if passed date values are valid
    if (utc->tm_min > 59 || utc->tm_min < 0) return 1;
    if (utc->tm_hour > 23 || utc->tm_hour < 0) return 1;
    if (utc->tm_yday > 366 || utc->tm_yday < 0) return 1;

    dtime->year = (utc->tm_year + 1900) % 10;
    dtime->min_ones = (utc->tm_min) % 10;
    dtime->min_tens = (utc->tm_min) / 10;
    dtime->hour_ones = (utc->tm_hour) % 10;
    dtime->hour_tens = (utc->tm_hour) / 10;
    dtime->day_ones = (utc->tm_yday) % 10;
    dtime->day_tens = ((utc->tm_yday) % 100) / 10;
    dtime->day_hund = (utc->tm_yday) / 100;
    
    return 0;
}

/*
 * Drives a GPIO pin for 100 Hz a specified number
 * number of times.
 */
static int wwv_drivepin(struct gpio_desc *wwv_pin, int times)
{
    volatile int i;
    
    for (i = 0; i < times; i++) {
        gpiod_set_value(wwv_pin, 1);
        usleep_range(4995, 5005);
        gpiod_set_value(wwv_pin, 0);
        usleep_range(4995, 5005);
    }

    return 0;
}

/*
 * Shift through a value and encodes in bcd format.
 */
static int wwv_enc_bcd(struct wwv_data_t *wwv_dat, int val, int places)
{
    int i;

    for (i = 0; i < places; i++) {
        // One bit drives pin for 470ms and rests for rest of sec
        if (val & (1<<i)) {
            wwv_drivepin(wwv_dat->gpio_wwv, OBIT);
            usleep_range(ODELAY, ODELAY + 1);
        // Zero bit drives pin for 170ms and rests for rest of sec
        } else {
            wwv_drivepin(wwv_dat->gpio_wwv, ZBIT);
            usleep_range(ZDELAY, ZDELAY + 1);
        }
    }

    return 0;

}

/*
 * Segment 1 of wwv encoding. Gets the ones place of the year.
 */
static int seg_p1(struct wwv_data_t *wwv_dat, struct wwv_date *dtime)
{
    // First a blank
    msleep(1000);

    // Encodes three zero bits
    wwv_enc_bcd(wwv_dat, 0, 3);

    // encodes years one place
    wwv_enc_bcd(wwv_dat, dtime->year, 4);

    // Zero bit
    wwv_enc_bcd(wwv_dat, 0, 1);

    // Position indicator for Segment 1
    wwv_drivepin(wwv_dat->gpio_wwv, PINDEX);
    usleep_range(PDELAY, PDELAY + 1);

    return 0;
}

/*
 * Segment 2 of wwv encoding. This gets the ones and tens of the 
 * minutes.
 */
static int seg_p2(struct wwv_data_t *wwv_dat, struct wwv_date *dtime)
{
    // Encodes min ones place
    wwv_enc_bcd(wwv_dat, dtime->min_ones, 4);

    // Encodes a zero bit
    wwv_enc_bcd(wwv_dat, 0, 1);

    // Encodes min tens place
    wwv_enc_bcd(wwv_dat, dtime->min_tens, 3);

    // Encodes a zero bit
    wwv_enc_bcd(wwv_dat, 0, 1);

    // Position indicator for segment 2
    wwv_drivepin(wwv_dat->gpio_wwv, PINDEX);
    usleep_range(PDELAY, PDELAY + 1);

    return 0;
}

/*
 * Segment 3 of wwv encoding. This gets the ones and tens place of
 * the hours.
 */
static int seg_p3(struct wwv_data_t *wwv_dat, struct wwv_date *dtime)
{
    // Encodes hour ones place
    wwv_enc_bcd(wwv_dat, dtime->hour_ones, 4);

    // Encodes a zero bit
    wwv_enc_bcd(wwv_dat, 0, 1);

    // Encodes hour tens place
    wwv_enc_bcd(wwv_dat, dtime->hour_tens, 3);

    // Encodes a zero bit
    wwv_enc_bcd(wwv_dat, 0, 1);

    // Position indicator for segment 3
    wwv_drivepin(wwv_dat->gpio_wwv, PINDEX);
    usleep_range(PDELAY, PDELAY + 1);

    return 0;
}

/*
 * Segment 4 of wwv encoding. This gets the ones and tens place of
 * the DoY.
 */
static int seg_p4(struct wwv_data_t *wwv_dat, struct wwv_date *dtime)
{
    // Encodes day ones place
    wwv_enc_bcd(wwv_dat, dtime->day_ones, 4);

    // Encodes a zero bit
    wwv_enc_bcd(wwv_dat, 0, 1);

    // Encodes day tens place
    wwv_enc_bcd(wwv_dat, dtime->day_tens, 4);

    // Position indicator for segment 4
    wwv_drivepin(wwv_dat->gpio_wwv, PINDEX);
    usleep_range(PDELAY, PDELAY + 1);

    return 0;
}

/*
 * Segment 5 of wwv encoding. This gets the hundreds place of DoY.
 */
static int seg_p5(struct wwv_data_t *wwv_dat, struct wwv_date *dtime)
{
    // Encodes day hundreds place
    wwv_enc_bcd(wwv_dat, dtime->day_hund, 2);

    // Encodes 3 zero pits
    wwv_enc_bcd(wwv_dat, 0, 3);

    // Waits for the last 5 seconds
    ssleep(5);

    return 0;
}

/*
 * Encodes wwv for all segments. This is what is called in ioctl)
 */
static int wwv_enc_date(struct wwv_data_t *wwv_dat, struct wwv_date *dtime)
{
    // Runs each segment for encoding
    seg_p1(wwv_dat, dtime);
    seg_p2(wwv_dat, dtime);
    seg_p3(wwv_dat, dtime);
    seg_p4(wwv_dat, dtime);
    seg_p5(wwv_dat, dtime);

    // Final segment is all zeros
    ssleep(10);

    return 0;
}

// ioctl system call
// If another process is using the pins and the device was opened O_NONBLOCK
//   then return with the appropriate error
// Otherwise
//   If another process is using the pins
//     then block/wait for the pin to be free.  Clean up and return an error if a signal is received.
//   Otherwise
//     Copy the user space data using copy_from_user() to a local kernel space buffer that you allocate
//     Encode to the copied data using your encoding system from homework 5 (your "wwv" code) to another kernel buffer that you allocate
//     Toggle pins as in homework 04 gpio code.  While delaying, do not consume CPU resources. *** SEE TIMERS-HOWTO.TXT IN THE KERNEL DOCUMENTATION ***
//       Use a variable for the clock high and low pulse widths that is shared with the ioctl class.  The
//  CLEAN UP (free all allocated memory and any other resouces you allocated) AND RETURN APPROPRAITE VALUE

// You will need to choose the type of locking yourself.  It may be atmonic variables, spinlocks, mutex, or semaphore.
static long wwv_ioctl(struct file * filp, unsigned int cmd, unsigned long arg)
{
    long ret = 0;					// Return value
    struct wwv_data_t *wwv_dat;	// Driver data - has gpio pins
    struct tm *udtime = NULL; // Date info passed from user space
    struct wwv_date *kdtime = NULL; //Seperated date info for wwv functions
	
    // Get our driver data
    wwv_dat=(struct wwv_data_t *)filp->private_data;

    // Returns error is device is opened with NONBLOCK
    // while the pins are already being used
    if (filp->f_flags & O_NONBLOCK) {
        if(mutex_is_locked(&(wwv_data_fops->lock))) {
            printk(KERN_INFO"WWV Error! Can't open NONBLOCK!\n");
            return -EAGAIN;
        }
    }
 
    // IOCTL cmds
    switch (cmd) {
	    case WWV_TRANSMIT:
            printk(KERN_INFO "WWV_TRANSMIT\n");

            // Locking
            ret = mutex_lock_interruptible(&(wwv_data_fops->lock));
            if (ret != 0) {
                printk(KERN_INFO "Error! Could not acquire lock!");
                return -EINTR;
            }

            // Allocate memory for userspace data
            udtime = kmalloc(sizeof(struct tm), GFP_ATOMIC);
            if (udtime == NULL) {
                printk(KERN_INFO "Error! Could not allocate memory for userspace buffer!\n");
                ret = -ENOMEM;
                goto fail;
            }   

            // Allocates memory for the wwv_date struct    
            kdtime = kmalloc(sizeof(struct wwv_date), GFP_ATOMIC);
            if (kdtime == NULL) {
                printk(KERN_INFO "Error! Could not allocate memory for wwv_date buffer!\n");
                ret = -ENOMEM;
                goto fail;
            } 
    
            // Copies date from userspace
            ret = copy_from_user(udtime, (struct tm *)arg, sizeof(struct tm));
            if (ret != 0) {
                printk(KERN_INFO "Sturct could not be passed\n");
                ret = -EFAULT;
                goto fail;
            }

            // Debuging for userspace
	        printk(KERN_INFO "Struct was passed\n");	
            
            // Stores date into wwv_date struct
            ret = wwv_conv_date(udtime, kdtime);
            if (ret != 0) {
                printk(KERN_INFO "Date values passed are not valid!\n");
                ret = -EINVAL;
                goto fail;
            }

            // Prints out Date data for debugging purposes
            printk(KERN_INFO "Min: %d %d\n", kdtime->min_tens, kdtime->min_ones);
            printk(KERN_INFO "Hour: %d %d\n", kdtime->hour_tens, kdtime->hour_ones);
            printk(KERN_INFO "Day: %d %d %d\n", kdtime->day_hund, kdtime->day_tens, kdtime->day_ones);
           
            // Performs encoding
            wwv_enc_date(wwv_dat, kdtime);
            break;
		
        default:
            printk(KERN_INFO "Invalid command for wwv\n");
            ret = -EINVAL;
            return ret;
    }

    // Clean up
    printk(KERN_INFO "Clean up\n");
    mutex_unlock(&(wwv_data_fops->lock));
    kfree(udtime);
    udtime = NULL;
    kfree(kdtime);
    kdtime = NULL;
    gpiod_set_value(wwv_dat->gpio_wwv,0);
    return 0;

fail:
    // Unlocks lock
    mutex_unlock(&(wwv_data_fops->lock));
    
    // Frees the memory of the buffers (if it needs to)
    if (udtime != NULL) kfree(udtime);
    if (kdtime != NULL) kfree(kdtime);
	
    return ret;
}

// Write system call
//   Just return 0
static ssize_t wwv_write(struct file *filp, const char __user * buf, size_t count, loff_t * offp)
{
    return 0;
}

// Open system call
// Open only if the file access flags (NOT permissions) are appropiate as discussed in class
// Return an appropraite error otherwise
static int wwv_open(struct inode *inode, struct file *filp)
{
    // SUCESSFULLY THE FILE IF AND ONLY IF THE FILE FLAGS FOR ACCESS ARE APPROPRIATE
    // RETURN WITH APPROPRIATE ERROR OTHERWISE
    if ((filp->f_flags&O_ACCMODE)==O_RDONLY) return -EOPNOTSUPP;
    if ((filp->f_flags&O_ACCMODE)==O_RDWR) return -EOPNOTSUPP;

    filp->private_data=wwv_data_fops;  // My driver data (afsk_dat)

    return 0;
}

// Close system call
// What is there to do?
static int wwv_release(struct inode *inode, struct file *filp)
{
    return 0;
}

// File operations for the wwv device
static const struct file_operations wwv_fops = {
    .owner = THIS_MODULE,	// Us
    .open = wwv_open,		// Open
    .release = wwv_release,// Close
    .write = wwv_write,	// Write
    .unlocked_ioctl=wwv_ioctl,	// ioctl
};

static struct gpio_desc *wwv_dt_obtain_pin(struct device *dev, struct device_node *parent, char *name, int init_val)
{
    struct device_node *dn_child=NULL;	// DT child
    struct gpio_desc *gpiod_pin=NULL;	// GPIO Descriptor for setting value
    int ret=-1;	// Return value
    int pin=-1;	// Pin number
    char *label=NULL;	// DT Pin label

    // Find the child - release with of_node_put()
    dn_child=of_get_child_by_name(parent,name);
    if (dn_child==NULL) {
        printk(KERN_INFO "No child %s\n",name);
        gpiod_pin=NULL;
        goto fail;
    }

    // Get the child pin number - does not appear to need to be released
    pin=of_get_named_gpio(dn_child,"gpios",0);
    if (pin<0) {
        printk(KERN_INFO "no %s GPIOs\n",name);
        gpiod_pin=NULL;
        goto fail;
    }
    // Verify pin is OK
    if (!gpio_is_valid(pin)) {
        gpiod_pin=NULL;
        goto fail;
    }
    printk(KERN_INFO "Found %s pin %d\n",name,pin);

    // Get the of string tied to pin - Does not appear to need to be released
    ret=of_property_read_string(dn_child,"label",(const char **)&label);
    if (ret<0) {
        printk(KERN_INFO "Cannot find label\n");
        gpiod_pin=NULL;
        goto fail;
    }
    // Request the pin - release with devm_gpio_free() by pin number
    if (init_val>=0) {
        ret=devm_gpio_request_one(dev,pin,GPIOF_OUT_INIT_LOW,label);
        if (ret<0) {
            dev_err(dev,"Cannot get %s gpio pin\n",name);
            gpiod_pin=NULL;
            goto fail;
        }
    } else {
        ret=devm_gpio_request_one(dev,pin,GPIOF_IN,label);
        if (ret<0) {
            dev_err(dev,"Cannot get %s gpio pin\n",name);
            gpiod_pin=NULL;
            goto fail;
        }
    }

    // Get the gpiod pin struct
    gpiod_pin=gpio_to_desc(pin);
    if (gpiod_pin==NULL) {
        printk(KERN_INFO "Failed to acquire wwv gpio\n");
        gpiod_pin=NULL;
        goto fail;
    }

    // Make sure the pin is set correctly
    if (init_val>=0) gpiod_set_value(gpiod_pin,init_val);


    // Release the device node
    of_node_put(dn_child);
	
    return gpiod_pin;

fail:
    if (pin>=0) devm_gpio_free(dev,pin);
    if (dn_child) of_node_put(dn_child);

    return gpiod_pin;
}


// Sets device node permission on the /dev device special file
static char *wwv_devnode(struct device *dev, umode_t *mode)
{
    if (mode) *mode = 0666;
    return NULL;
}

// My data is going to go in either platform_data or driver_data
//  within &pdev->dev. (dev_set/get_drvdata)
// Called when the device is "found" - for us
// This is called on module load based on ".of_match_table" member
static int wwv_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;	// Device associcated with platform

    struct wwv_data_t *wwv_dat;		// Data to be passed around the calls
    struct device_node *dn=NULL;

    int ret=-1;	// Return value


    // Allocate device driver data and save
    wwv_dat=kmalloc(sizeof(struct wwv_data_t),GFP_ATOMIC);
    if (wwv_dat==NULL) {
        printk(KERN_INFO "Memory allocation failed\n");
        return -ENOMEM;
    }

    memset(wwv_dat,0,sizeof(struct wwv_data_t));
	
    dev_set_drvdata(dev,wwv_dat);

    // Find my device node
    dn=of_find_node_by_name(NULL,"wwv");
    if (dn==NULL) {
        printk(KERN_INFO "Cannot find device\n");
        ret=-ENODEV;
        goto fail;
    }
    wwv_dat->gpio_wwv=wwv_dt_obtain_pin(dev,dn,"WWV",0);
    if (wwv_dat->gpio_wwv==NULL) goto fail;
    wwv_dat->gpio_unused17=wwv_dt_obtain_pin(dev,dn,"Unused17",0);
    if (wwv_dat->gpio_unused17==NULL) goto fail;
    wwv_dat->gpio_unused18=wwv_dt_obtain_pin(dev,dn,"Unused18",0);
    if (wwv_dat->gpio_unused18==NULL) goto fail;
    wwv_dat->gpio_unused22=wwv_dt_obtain_pin(dev,dn,"Unused22",0);
    if (wwv_dat->gpio_unused22==NULL) goto fail;
    wwv_dat->gpio_shutdown=wwv_dt_obtain_pin(dev,dn,"Shutdown",-1);
    if (wwv_dat->gpio_shutdown==NULL) goto fail;


    // Create the device - automagically assign a major number
    wwv_dat->major=register_chrdev(0,"wwv",&wwv_fops);
    if (wwv_dat->major<0) {
	    printk(KERN_INFO "Failed to register character device\n");
	    ret=wwv_dat->major;
	    goto fail;
    }

    // Create a class instance
    wwv_dat->wwv_class=class_create(THIS_MODULE, "wwv_class");
    if (IS_ERR(wwv_dat->wwv_class)) {
        printk(KERN_INFO "Failed to create class\n");
        ret=PTR_ERR(wwv_dat->wwv_class);
        goto fail;
    }

    // Setup the device so the device special file is created with 0666 perms
    wwv_dat->wwv_class->devnode=wwv_devnode;
    wwv_dat->wwv_dev=device_create(wwv_dat->wwv_class,NULL,MKDEV(wwv_dat->major,0),(void *)wwv_dat,"wwv");
    if (IS_ERR(wwv_dat->wwv_dev)) {
        printk(KERN_INFO "Failed to create device file\n");
        ret=PTR_ERR(wwv_dat->wwv_dev);
        goto fail;
    }

    wwv_data_fops=wwv_dat;

    // Init mutex lock
    mutex_init(&(wwv_dat->lock));
	
    printk(KERN_INFO "Registered\n");
    dev_info(dev, "Initialized");
    return 0;

fail:
    // Device cleanup
    if (wwv_dat->wwv_dev) device_destroy(wwv_dat->wwv_class,MKDEV(wwv_dat->major,0));
    // Class cleanup
    if (wwv_dat->wwv_class) class_destroy(wwv_dat->wwv_class);
    // char dev clean up
    if (wwv_dat->major) unregister_chrdev(wwv_dat->major,"wwv");

    if (wwv_dat->gpio_shutdown) devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_shutdown));
    if (wwv_dat->gpio_unused22) devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_unused22));
    if (wwv_dat->gpio_unused18) devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_unused18));
    if (wwv_dat->gpio_unused17) devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_unused17));
    if (wwv_dat->gpio_wwv) devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_wwv));


    dev_set_drvdata(dev,NULL);
    kfree(wwv_dat);
    printk(KERN_INFO "WWV Failed\n");
    return ret;
}

// Called when the device is removed or the module is removed
static int wwv_remove(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct wwv_data_t *wwv_dat;	// Data to be passed around the calls

    // Obtain the device driver data
    wwv_dat=dev_get_drvdata(dev);

    // Device cleanup
    device_destroy(wwv_dat->wwv_class,MKDEV(wwv_dat->major,0));
    // Class cleanup
    class_destroy(wwv_dat->wwv_class);
    // Remove char dev
    unregister_chrdev(wwv_dat->major,"wwv");

    // Free the gpio pins with devm_gpio_free() & gpiod_put()
    devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_shutdown));
    devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_unused22));
    devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_unused18));
    devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_unused17));
    devm_gpio_free(dev,desc_to_gpio(wwv_dat->gpio_wwv));
#if 0
    // not clear if these are allocated and need to be freed
    gpiod_put(wwv_dat->gpio_shutdown);
    gpiod_put(wwv_dat->gpio_unused22);
    gpiod_put(wwv_dat->gpio_unused18);
    gpiod_put(wwv_dat->gpio_unused17);
    gpiod_put(wwv_dat->gpio_wwv);
#endif
	
    // Free the device driver data
    dev_set_drvdata(dev,NULL);
    kfree(wwv_dat);

    printk(KERN_INFO "Removed\n");
    dev_info(dev, "GPIO mem driver removed - OK");

    return 0;
}

static const struct of_device_id wwv_of_match[] = {
    {.compatible = "brcm,bcm2835-wwv",},
    { /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, wwv_of_match);

static struct platform_driver wwv_driver = {
    .probe = wwv_probe,
    .remove = wwv_remove,
    .driver = {
            .name = "bcm2835-wwv",
            .owner = THIS_MODULE,
            .of_match_table = wwv_of_match,
            },
};

module_platform_driver(wwv_driver);

MODULE_DESCRIPTION("WWV pin modulator");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("WWV");
//MODULE_ALIAS("platform:wwv-bcm2835");
