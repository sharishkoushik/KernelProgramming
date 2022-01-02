/* Start of RealTek rtl8139 specs*/

/* min and max supported ethernet frame size. Maximum  must be at least (dev->mtu+14+4).*/
#define MAX_ETH_FRAME_SIZE      1536
#define ETH_MIN_LEN 60  

#define TX_BUF_SIZE  MAX_ETH_FRAME_SIZE 

/* Number of transmit Tx descriptor registers. */
#define NUM_TX_DESC 4

/* Total transmit buffer size is 4 x 1536 */
#define TOTAL_TX_BUF_SIZE  (TX_BUF_SIZE * NUM_TX_DESC)

/* Size of the in-memory receive ring. */
#define RX_BUF_LEN_IDX 2        /* 0==8K, 1==16K, 2==32K, 3==64K */
#define RX_BUF_LEN     (8192 << RX_BUF_LEN_IDX) 
#define RX_BUF_PAD     16      /* see 11th and 12th bit of RCR: 0x44 */
#define RX_BUF_WRAP_PAD 2048   /* spare padding to handle pkt wrap */
#define TOTAL_RX_BUF_SIZE  (RX_BUF_LEN + RX_BUF_PAD + RX_BUF_WRAP_PAD)

/* RealTek 8139 register offsets */

#define TSD0          0x10    /* First Transmit descriptor contains status of transmit packet */ 
#define TSAD0         0x20    /* Contains the  physical address of the packet in memory*/
#define RBSTART       0x30    /* Recieve buffer start address, Start of data , packet 1 */ 
#define CR            0x37    /* See CR register commands below */
#define CAPR          0x38    /* Buffer read pointer. Address of buffer that driver has read */
#define IMR           0x3c    /* Set this to enable interrupts */
#define ISR           0x3e    /* Interrupt status register */
#define TCR           0x40    /* Initialize to support DMA burst size (1024), before tranmsit */
#define RCR           0x44    /* Needs to initialize before recieve */
#define MPC           0x4c    /* Needs to initialize this missed packet counter */
#define MULINT        0x5c    /* Initialize to avoid any early recieve interrupts */    

/* RealTek TSD register commands. work with TSD0-3 registers */

#define TxHostOwns    0x2000
#define TxUnderrun    0x4000
#define TxStatOK      0x8000
#define TxOutOfWindow 0x20000000
#define TxAborted     0x40000000
#define TxCarrierLost 0x80000000

/* RealTek rtl8139 CR register commands. Work with CR register */

#define RxBufEmpty 0x01
#define CmdTxEnb   0x04
#define CmdRxEnb   0x08
#define CmdReset   0x10
#define ChipCmdClear 0xe2


/* RealTek ISR Bits */

#define RxOK       0x01
#define RxErr      0x02
#define TxOK       0x04
#define TxErr      0x08
#define RxOverFlow 0x10
#define RxUnderrun 0x20
#define RxFIFOOver 0x40
#define CableLen   0x2000
#define TimeOut    0x4000
#define SysErr     0x8000

/* This mask is used to enable all interrupts by writing to IMR register*/
#define INT_MASK (RxOK | RxErr | TxOK | TxErr | \
               RxOverFlow | RxUnderrun | RxFIFOOver | \
               CableLen | TimeOut | SysErr)

/* RealTek TCR register shifts/mask */
#define TCR_DMA_BURST_SHIFT  8
#define TCR_DMA_BURST_MASK   0x7

/* RealTek RCR register shifts/mask */
#define RCR_AAP_SHIFT              0   //Accept Physical Address packets
#define RCR_APM_SHIFT              1   //Accept Physical Match packets
#define RCR_AM_SHIFT               2   //Accept MCast packets
#define RCR_AB_SHIFT               3   //Accept Bcast packets
#define RCR_AR_SHIFT               4   //Accept Runt packets
#define RCR_AER_SHIFT              5   //Accept Error Packets
#define RCR_9356_SEL_SHIFT         6   //EEPROM Select: This bit reflects what type of EEPROM is used.
#define RCR_WRAP_SHIFT             7   // 0 - Wraps around the received data in the Rx buffer 1 - Disables Wrap around
#define RCR_MXDMA_SHIFT            8   //Max DMA Burst Size per Rx DMA Burst - 7 is unlimited , means the received packet is completely burst xfered
#define RCR_RBLEN_SHIFT           12   //Rx Ring Buffer Length - 1 is 16k + 16 bytes
#define RCR_RXFTH_SHIFT           13   //Rx FIFO Threshold - high water-mark for starting transfer from Rx FIFO to host
#define RCR_RER8_SHIFT            16   //Accept error packets if matching provided criteria.
#define RCR_MulERINT_SHIFT        17   //Multiple early interrupt select
#define RCR_RESVD0_SHIFT          18
#define RCR_ERTH_SHIFT            24   //Early Rx threshold bits
#define RCR_RESVD1_SHIFT          28

/* write MMIO register, with flush */
/* Flush avoids rtl8139 bug w/ posted MMIO writes */
#define WRITEB_F(val8 ,  regaddr)   do { iowrite8 ( (val8) ,  (regaddr) ); ioread8  (regaddr); } while (0)
#define WRITEW_F(val16 , regaddr)   do { iowrite16 ( (val16), (regaddr) ); ioread16 (regaddr); } while (0)
#define WRITEL_F(val32 , regaddr)   do { iowrite32 ( (val32), (regaddr) ); ioread32 (regaddr); } while (0)

// Supported VendorID and DeviceID

#define VENDOR_ID  0x10ec  // Realtek Semiconductor Company, LTD
#define DEVICE_ID  0x8139  // RTL8139 10/100 Ethernet Controller

#define DRV_NAME    "LDDA_PCI"  // Use to change name of interface from eth

// Device private structure

struct rtl8139
{
    struct pci_dev *pci_dev;  // PCI device 
    
    void * __iomem  mmio_addr; 	//memory mapped IO addr
    unsigned long regs_len; 	//length of MMIO region
    
    unsigned int cur_tx;
    unsigned int dirty_tx;
    unsigned char *tx_buf[NUM_TX_DESC];  
    unsigned char *tx_bufs;   
    dma_addr_t tx_bufs_dma;
    
    void *rx_ring;
    unsigned int cur_rx;
    dma_addr_t rx_ring_dma;
    
    spinlock_t lock;
    struct net_device_stats stats; 
};

typedef struct rtl8139 rtl8139_t;

/* PCI callback and ISR routines */
static int __devinit 
rtl8139_probe(struct pci_dev *pdev, const struct pci_device_id *id);
static void __devexit rtl8139_remove (struct pci_dev *pdev);
static irqreturn_t rtl8139_interrupt (int irq, void *dev_instance);
// static int rtl8139_suspend (struct pci_dev *pdev, pm_message_t state);
// static int rtl8139_resume (struct pci_dev *pdev);

// Net Device specific routines 
static int rtl8139_open(struct net_device *netdev);
static int rtl8139_stop(struct net_device *netdev);
static int rtl8139_start_xmit(struct sk_buff *skb, struct net_device *netdev);
static void rtl8139_hardware_start(struct net_device *netdev);
static struct net_device_stats* rtl8139_get_stats(struct net_device *netdev);

#define assert(expr) \
        if(unlikely(!(expr))) {                                     \
            printk(KERN_ERR "Assertion failed! %s,%s,%s,line=%d\n", \
            #expr, __FILE__, __func__, __LINE__);                   \
        }
