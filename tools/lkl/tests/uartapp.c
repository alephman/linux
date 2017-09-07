#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <lkl.h>
#include <lkl_host.h>
#ifndef __MINGW32__
#include <sys/stat.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#else
#include <windows.h>
#endif
#include "test.h"
#include <sys/stat.h>
#include <fcntl.h>

 #include <linux/fb.h>
 #include <sys/mman.h>
 #include <errno.h>
#include <sys/types.h>
#include <termios.h>


#define MAX_SIZE 256


static struct cl_args {
	int printk;
	const char *disk_filename;
	const char *tap_ifname;
	const char *fstype;
	int part;
} cla;

static struct cl_option {
	const char *long_name;
	char short_name;
	const char *help;
	int has_arg;
} options[] = {
	{"enable-printk", 'p', "show Linux printks", 0},
	{"disk-file", 'd', "disk file to use", 1},
	{"partition", 'P', "partition to mount", 1},
	{"net-tap", 'n', "tap interface to use", 1},
	{"type", 't', "filesystem type", 1},
	{0},
};

static int parse_opt(int key, char *arg)
{
	switch (key) {
	case 'p':
		cla.printk = 1;
		break;
	case 'P':
		cla.part = atoi(arg);
		break;
	case 'd':
		cla.disk_filename = arg;
		break;
	case 'n':
		cla.tap_ifname = arg;
		break;
	case 't':
		cla.fstype = arg;
		break;
	default:
		return -1;
	}

	return 0;
}

void printk(const char *str, int len)
{
	int ret __attribute__((unused));

	if (cla.printk)
		ret = write(STDOUT_FILENO, str, len);
}




static struct cl_option *find_short_opt(char name)
{
	struct cl_option *opt;

	for (opt = options; opt->short_name != 0; opt++) {
		if (opt->short_name == name)
			return opt;
	}

	return NULL;
}

static struct cl_option *find_long_opt(const char *name)
{
	struct cl_option *opt;

	for (opt = options; opt->long_name; opt++) {
		if (strcmp(opt->long_name, name) == 0)
			return opt;
	}

	return NULL;
}

static void print_help(void)
{
	struct cl_option *opt;

	printf("usage:\n");
	for (opt = options; opt->long_name; opt++)
		printf("-%c, --%-20s %s\n", opt->short_name, opt->long_name,
		       opt->help);
}

static int parse_opts(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		struct cl_option *opt = NULL;

		if (argv[i][0] == '-') {
			if (argv[i][1] != '-')
				opt = find_short_opt(argv[i][1]);
			else
				opt = find_long_opt(&argv[i][2]);
		}

		if (!opt) {
			print_help();
			return -1;
		}

		if (parse_opt(opt->short_name, argv[i + 1]) < 0) {
			print_help();
			return -1;
		}

		if (opt->has_arg)
			i++;
	}

	return 0;
}



struct uart_dev {
	int fd;
	int uart_fd;  //lkl's uart fd
	int irq;
	void *base;
	void *iomem_base;
	int mmio_size;
	lkl_thread_t irq_tid;
	lkl_thread_t uart_recv_tid;
};


static  void uart_deliver_irq(struct uart_dev *dev)
{
	/* Make sure all memory writes before are visible to the driver. */
	__sync_synchronize();
	lkl_trigger_irq(dev->irq);
}



static void uartio_irq_thread(void *arg)
{
	struct uart_dev  *dev = arg;
	int irq_count=0;


	do {
		   irq_count=1;
                write(dev->fd, &irq_count, 4);
                if (read(dev->fd, &irq_count, 4) == 4) {
				uart_deliver_irq(dev);
		
                }else{
                	printf("bad irq in thread \n");
                }

	} while (1);
}

static void uart_recv_thread(void *arg)
{
	struct uart_dev  *dev = arg;
	int count;
	char buff_recv[MAX_SIZE];  

	do {
		count = lkl_sys_read(dev->uart_fd, buff_recv , MAX_SIZE-1);
		buff_recv[count]='\0';
        	printf("%s" , buff_recv);

	} while (1);
}



typedef unsigned long uintptr_t;
typedef unsigned int uint32_t;
typedef uintptr_t vaddr_t;

static inline void write32(uint32_t val, vaddr_t addr)
{
        *(volatile uint32_t *)addr = val;
}


static int wapper_tcsetattr (
     int fd,
     int optional_actions,
     const struct termios *termios_p)
{
  unsigned long int cmd;

  switch (optional_actions)
    {
    case TCSANOW:
      cmd = TCSETS;
      break;
    case TCSADRAIN:
      cmd = TCSETSW;
      break;
    case TCSAFLUSH:
      cmd = TCSETSF;
      break;
    default:
      __set_errno (EINVAL);
      return -1;
    }


  return lkl_sys_ioctl (fd, cmd, termios_p);
}

	 
int
wapper_tcgetattr (fd, termios_p)
      int fd;
      struct termios *termios_p;
{
  int retval;

  retval = lkl_sys_ioctl ( fd, TCGETS, termios_p);

 
  return retval;
}


#define IBAUD0	020000000000

static speed_t
wapper_cfgetospeed (     const struct termios *termios_p)
{
  return termios_p->c_cflag & (CBAUD | CBAUDEX);
}


static speed_t
wapper_cfgetispeed (const struct termios *termios_p)
{
  return ((termios_p->c_iflag & IBAUD0)
	  ? 0 : termios_p->c_cflag & (CBAUD | CBAUDEX));
}

/* Set the output baud rate stored in *TERMIOS_P to SPEED.  */
static int wapper_cfsetospeed  (struct termios *termios_p,
     speed_t speed)
{
  if ((speed & ~CBAUD) != 0
      && (speed < B57600 || speed > __MAX_BAUD))
    {
      __set_errno (EINVAL);
      return -1;
    }

#ifdef _HAVE_STRUCT_TERMIOS_C_OSPEED
  termios_p->c_ospeed = speed;
#endif
  termios_p->c_cflag &= ~(CBAUD | CBAUDEX);
  termios_p->c_cflag |= speed;

  return 0;
}



static int wapper_cfsetispeed (    struct termios *termios_p,   speed_t speed)
{
  if ((speed & ~CBAUD) != 0
      && (speed < B57600 || speed > __MAX_BAUD))
    {
      __set_errno (EINVAL);
      return -1;
    }

#ifdef _HAVE_STRUCT_TERMIOS_C_ISPEED
  termios_p->c_ispeed = speed;
#endif
  if (speed == 0)
    termios_p->c_iflag |= IBAUD0;
  else
    {
      termios_p->c_iflag &= ~IBAUD0;
      termios_p->c_cflag &= ~(CBAUD | CBAUDEX);
      termios_p->c_cflag |= speed;
    }

  return 0;
}


static int
wapper_tcflush (  int fd,  int queue_selector)
{
   return  lkl_sys_ioctl(fd, TCFLSH, (long)queue_selector);
}




static int  tty_config_uart(struct uart_dev *dev)
{

    struct termios t ;
    wapper_tcgetattr( dev->uart_fd , &t) ;
    t.c_cflag &=~CSIZE ;
    t.c_cflag &=~CSTOPB;
    t.c_cflag |= CREAD ;
    t.c_cflag |= CLOCAL ;
    t.c_cflag &= ~CRTSCTS;
    t.c_cflag |= CS8;
    wapper_cfsetispeed( &t , B115200);
    wapper_cfsetospeed( &t , B115200);
    wapper_tcflush( dev->uart_fd, TCIOFLUSH);
    wapper_tcsetattr( dev->uart_fd , TCSANOW ,&t);

	return 0;


}

static int  tty_open_uart(struct uart_dev *dev)
{
       int ret;

	//check if the /dev directory exist 
       ret = lkl_sys_access("/dev", LKL_S_IRWXO);
	if (ret < 0) {
		if (ret == -LKL_ENOENT)
			ret = lkl_sys_mkdir("/dev", 0700);
		if (ret < 0)
			return ret;
	}


	//create the uart device node
	ret = lkl_sys_mknod("/dev/ttyAMA0", LKL_S_IFCHR | 666 , LKL_MKDEV(204, 64));
	if (ret)
		return ret;
	
	dev->uart_fd= lkl_sys_open("/dev/ttyAMA0", O_RDWR|O_NOCTTY,0);
	
	if (dev->uart_fd<0)
		return dev->uart_fd;
	
	return 0;

}


static int  tty_write_uart(struct uart_dev *dev)
{
	int ret;
	char buff_send[] = "this is a tty write test\n";  

	ret=lkl_sys_write(dev->uart_fd, buff_send, sizeof(buff_send));

	return ret;

}


static int  tty_read_uart(struct uart_dev *dev)
{

	dev->uart_recv_tid= lkl_host_ops.thread_create(uart_recv_thread, dev);
	return 0;

}



static int amba_read(void *data, int offset, void *res, int size)
{
	void * addr = data +offset;
	
	if (size == 1)
		*(uint8_t *)res = *(uint8_t *)addr;

	if (size == 2)
		*(uint16_t *)res = *(uint16_t *)addr;
	
	if (size == 4)
		*(uint32_t *)res = *(uint32_t *)addr;

	if (size == 8)
		*(uint64_t *)res = * (uint64_t *) addr;

	return 0;
}



static int amba_write(void *data, int offset, void *res, int size)
{

	void * addr = data +offset;
	
	if (size == 1)
		*(uint8_t *)addr = *(uint8_t *)res;

	if (size == 2)
		*(uint16_t *)addr = *(uint16_t *)res;
	
	if (size == 4)
		*(uint32_t *)addr = *(uint32_t *)res;

	if (size == 8)
		*(uint64_t *)addr = * (uint64_t *) res;

	return 0;
}


struct lkl_iomem_ops {
	int (*read)(void *data, int offset, void *res, int size);
	int (*write)(void *data, int offset, void *value, int size);
};

static const struct lkl_iomem_ops amba_ops = {
	.read = amba_read,
	.write = amba_write,
};



static int setup_dev_uart(struct uart_dev *dev){

	int ret;

	dev->irq = lkl_get_free_irq("uart");
	dev->mmio_size = 0x1000;

	dev->iomem_base = register_iomem(dev->base, dev->mmio_size, &amba_ops);
	if (!dev->iomem_base) {
		return -LKL_ENOMEM;
	}
	
	
	 ret =lkl_sys_amba_device_add((long) dev->iomem_base, dev->mmio_size,
						   dev->irq,0x241011);
	if (ret < 0) {
		lkl_printf("can't register uart device\n");
		return -1;
	}

	return 0;

}




static  int init_uio_uart(struct uart_dev *dev){

	 int fd;
        int ret;
	 
        int irq_count;

         unsigned long base;

	 
        fd = open("/dev/uio0",O_RDWR | O_SYNC);
		
	if (fd < 0) {
                printf("open dev failed: %s\n", strerror(errno));
                exit(-1);
        }
  
        base = (unsigned char  *) mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,fd, 0);


     if(base == MAP_FAILED)
     {
         printf("mmap fail\n");
         exit(-1);
     }

	 dev->irq_tid= lkl_host_ops.thread_create(uartio_irq_thread, dev);
	 dev->fd=fd;
	 dev->base=(void *) base;

     return 0;

}



static void test_uart_dev(struct uart_dev *dev){

	init_uio_uart(dev);

	setup_dev_uart(dev);

	tty_open_uart(dev);
	tty_config_uart( dev);

	tty_write_uart(dev);

	tty_read_uart(dev);

	return 0;
	
}



int main(int argc, char **argv)
{
	struct uart_dev dev;
	
	if (parse_opts(argc, argv) < 0)
		return -1;
	
	 lkl_host_ops.print = printk;
	 lkl_start_kernel(&lkl_host_ops, "  mem=16M loglevel=8");
	 
        test_uart_dev(&dev);

	while( 1)  
		sleep(1000);

	lkl_sys_close(dev.uart_fd);

	close(dev.fd);
	

	return 0;
}
