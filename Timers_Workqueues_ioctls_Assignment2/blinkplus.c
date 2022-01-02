

#include <linux/init.h>
#include <linux/module.h>
#include <linux/tty.h>		/* For fg_console, MAX_NR_CONSOLES */
#include <linux/kd.h>		/* For KDSETLED */
#include <linux/vt_kern.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include<linux/kernel.h>
#include <linux/sched.h>

MODULE_DESCRIPTION("Example module illustrating the use of Keyboard LEDs.");
MODULE_AUTHOR("Daniele Paolo Scarpazza");
MODULE_LICENSE("GPL");

struct timer_list my_timer;
struct tty_driver *my_driver;
static dev_t   dev;
static struct cdev   *my_cdev;
static char my_devname[]= "blinkplus";
char kbledstatus = 0;

#define BLINK_DELAY   HZ/5
#define ALL_LEDS_ON   0x07
#define RESTORE_LEDS  0xFF

#define DEV_MAJOR	249 // for static  major and minor device node 
#define DEV_MINOR	5

#define MAGIC 'z'
#define GETINV _IOR(MAGIC, 1, int32_t *)
#define SETINV _IOW(MAGIC, 2, int32_t *)
#define SETLED _IOW(MAGIC, 3, int32_t *)

int interval = BLINK_DELAY;

typedef struct blink_s {
	unsigned int               status;
        unsigned long              blink_delay;
        struct work_struct         *wk;    // work
        struct workqueue_struct  * wq;     // work queue
} blink_info_t;	 

static void do_work (struct work_struct * work)
{
        blink_info_t * info = container_of(work, blink_info_t, wk);
        unsigned int *pstatus = (unsigned int *)&info->status;

        if (*pstatus == kbledstatus)
              *pstatus = RESTORE_LEDS;
        else
            *pstatus =  kbledstatus;

        printk (KERN_INFO "\n *pstatus:%d  kbledstatus:%d", *pstatus,kbledstatus);

         (my_driver->ops->ioctl) (vc_cons[fg_console].d->port.tty, KDSETLED, *pstatus);
}

static void my_timer_func(unsigned long ptr)
{
        blink_info_t * info = (blink_info_t *) ptr;


        printk(KERN_INFO "\nin my_timer_func - %lu, info: %d\n", jiffies, info->status);

        // schedule_work (&info->wk);   // system wide work queue

        // For dedicated work queue use queue_work
        //  queue_work(info->wq, &info->wk);
		queue_work(info->wq, info->wk);

        my_timer.expires = jiffies + info->blink_delay;
        add_timer(&my_timer);
        printk(KERN_INFO "\nblinkplus: timer %lu %lu\n",jiffies,my_timer.expires);
}

static long int my_ioctl(struct file *file, unsigned cmd, unsigned long arg){
	switch(cmd){
		case SETINV:
			if(copy_from_user(&interval, (int32_t *)arg , sizeof(interval)))
				printk ("SETINV : There was an error in copy_from_user\n");
			else
				printk("SETINV : Interval SET\n");
			break;
		case SETLED:
			if(copy_from_user(&interval, (int32_t *)arg , sizeof(interval)))
				printk ("SETLED : There was an error in copy_from_user\n");
			else
				printk("SETLED : Interval SET\n");
			break;
		case GETINV:
			if(copy_to_user((int32_t *)arg, &interval, sizeof(interval)))
				printk("GETINV : There was an error in copy_to_user\n");
			else
				printk("GETLED : Interval retrieved\n");
			break;
		default:
			printk("No matching IOCTL");
			
	}
	return 0;
}

static int blinkplus_open(struct inode *inode, struct file *filp)
{

  printk(" %s blinkplus : open  \n", my_devname);

  return 0;
}

static int blinkplus_release(struct inode *inode, struct file *filp)
{

  printk(" %s blinkplus : close - count  \r\n", my_devname);

  return 0;
} 


static struct file_operations blinkplus_fops = {
	open: 	 blinkplus_open,
	release: blinkplus_release,
	unlocked_ioctl:   my_ioctl,
	owner:	 THIS_MODULE
};

static        dev_t   dev;
static struct cdev   *my_cdev;

unsigned int wait_major;
unsigned int wait_minor;

static int __init kbleds_init(void)
{
	int i;
	int res=0;int major;
	printk(KERN_INFO "kbleds: loading\n");
	printk(KERN_INFO "kbleds: fgconsole is %x\n", fg_console);
	for (i = 0; i < MAX_NR_CONSOLES; i++) {
		if (!vc_cons[i].d)
			break;
		printk(KERN_INFO " console[%i/%i] #%i, tty %lx\n", i,
		       MAX_NR_CONSOLES, vc_cons[i].d->vc_num, (unsigned long)vc_cons[i].d->port.tty);
		}
	printk(KERN_INFO "kbleds: finished scanning consoles\n");

	my_driver = vc_cons[fg_console].d->port.tty->driver;
	printk(KERN_INFO "kbleds: tty driver magic %x\n", my_driver->magic);
 /* initialize usage count as 1, acts like mutex */

/* Dynamic major number */
 res = alloc_chrdev_region(&dev, 0, 1, my_devname);
 if (res<0) { return res; }

  major = MAJOR(dev); /* device major number */

 /* allocate cdev structure and point to our device fops*/
  my_cdev = cdev_alloc();
  cdev_init(my_cdev, &blinkplus_fops);
  my_cdev->owner = THIS_MODULE;

/* connect major to cdev struct */
  res = cdev_add(my_cdev, dev, 1);
  if (res<0){
    unregister_chrdev_region(dev, 1);
    return res;
  }

  printk(" %s init - major: %d  \r\n", my_devname, major);

init_timer(&my_timer);
my_timer.function = my_timer_func;
my_timer.data = (unsigned long)&kbledstatus;
my_timer.expires = jiffies + BLINK_DELAY;
// add_timer(&my_timer);

blink_info_t *p;
struct workqueue_struct *wq;
struct work_struct *mywork;
wq = create_singlethread_workqueue("blinkplus");
mywork = kmalloc(sizeof(struct work_struct), GFP_KERNEL);


p -> wq =  wq;
p->wk = mywork;
p -> blink_delay = BLINK_DELAY;
p -> status = kbledstatus;

// info-> wq = create_singlethread_workqueue("blinkplus");
// info-> wk = kmalloc(sizeof(struct work_struct), GPF_KERNEL);
INIT_WORK(mywork, do_work);

  return res;
}

static void __exit kbleds_cleanup(void)
{
	printk(KERN_INFO "kbleds: unloading...\n");
	del_timer(&my_timer);
	(my_driver->ops->ioctl) (vc_cons[fg_console].d->port.tty, KDSETLED, RESTORE_LEDS);
  	cdev_del(my_cdev);
  	unregister_chrdev_region(dev, 1);

 // For static device nodes, use unregister_chrdev(wait_major,my_devname);
 
  	printk(" %s remove \r\n", my_devname);
}

module_init(kbleds_init);
module_exit(kbleds_cleanup);

MODULE_AUTHOR("Your Name");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Description");

