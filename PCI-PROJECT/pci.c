
/* GOOD TO SUBMIT */

#define DRV_NAME "LDD_PCI"
#define DRV_VERSION	"0.9.28"


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/completion.h>
#include <linux/crc32.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/gfp.h>
#include <linux/if_vlan.h>
#include <asm/irq.h>



#if defined(CONFIG_SH_DREAMCAST)
#define RX_BUF_IDX 0	/* 8K ring */
#else
#define RX_BUF_IDX	2	/* 32K ring */
#endif
#define RX_BUF_LEN	(8192 << RX_BUF_IDX)
#define RX_BUF_PAD	16
#define RX_BUF_WRAP_PAD 2048 /* spare padding to handle lack of packet wrap */

#if RX_BUF_LEN == 65536
#define RX_BUF_TOT_LEN	RX_BUF_LEN
#else
#define RX_BUF_TOT_LEN	(RX_BUF_LEN + RX_BUF_PAD + RX_BUF_WRAP_PAD)
#endif

#define CP_REGS_SIZE		(0xff + 1)

/* Number of Tx descriptor registers. */
#define NUM_TX_DESC	4

/* max supported ethernet frame size -- must be at least (dev->mtu+18+4).*/
#define MAX_ETH_FRAME_SIZE	1792

/* max supported payload size */
#define MAX_ETH_DATA_SIZE (MAX_ETH_FRAME_SIZE - VLAN_ETH_HLEN - ETH_FCS_LEN)

/* Size of the Tx bounce buffers -- must be at least (dev->mtu+18+4). */
#define TX_BUF_SIZE	MAX_ETH_FRAME_SIZE
#define TX_BUF_TOT_LEN	(TX_BUF_SIZE * NUM_TX_DESC)


typedef enum {
	RTL8139 = 0,
} board_t;

static const struct pci_device_id rtl8139_pci_tbl[] = {
	{0x10ec, 0x8139, PCI_ANY_ID, PCI_ANY_ID, 0, 0, RTL8139 },
	{0,}
};
MODULE_DEVICE_TABLE (pci, rtl8139_pci_tbl);

/* Symbolic offsets to registers. */
enum RTL8139_registers {
	TxStatus0	= 0x10,	 /* Transmit status (Four 32bit registers). */
	TxAddr0		= 0x20,	 /* Tx descriptors (also four 32bit). */
	RxBuf		= 0x30,
	ChipCmd		= 0x37,
	RxBufPtr	= 0x38,
	IntrMask	= 0x3C,
	IntrStatus	= 0x3E,
	RxConfig	= 0x44,
	RxMissed	= 0x4C,  /* 24 bits valid, write clears. */
	MultiIntr	= 0x5C,
};

enum ClearBitMasks {
	MultiIntrClear	= 0xF000,
	ChipCmdClear	= 0xE2,
};

enum ChipCmdBits {
	CmdReset	= 0x10,
	CmdRxEnb	= 0x08,
	CmdTxEnb	= 0x04,
	RxBufEmpty	= 0x01,
};

/* Interrupt register bits, using my own meaningful names. */
enum IntrStatusBits {
	PCIErr		= 0x8000,
	PCSTimeout	= 0x4000,
	RxFIFOOver	= 0x40,
	RxUnderrun	= 0x20,
	RxOverflow	= 0x10,
	TxErr		= 0x08,
	TxOK		= 0x04,
	RxErr		= 0x02,
	RxOK		= 0x01,
	RxAckBits	= RxFIFOOver | RxOverflow | RxOK,
};

enum TxStatusBits {
	TxHostOwns	= 0x2000,
	TxUnderrun	= 0x4000,
	TxStatOK	= 0x8000,
	TxOutOfWindow	= 0x20000000,
	TxAborted	= 0x40000000,
	TxCarrierLost	= 0x80000000,
};



struct rtl8139_stats {
	u64	packets;
	u64	bytes;
	struct u64_stats_sync	syncp;
};

struct rtl8139 {
	void __iomem		*mmio_addr;
	int			drv_flags;
	struct pci_dev		*pci_dev;
	u32			msg_enable;
	struct napi_struct	napi;
	struct net_device	*dev;

	unsigned char		*rx_ring;
	unsigned int		cur_rx;	/* RX buf index of next pkt */
	struct rtl8139_stats	rx_stats;
	dma_addr_t		rx_ring_dma;

	unsigned int		tx_flag;
	unsigned long		cur_tx;
	unsigned long		dirty_tx;
	struct rtl8139_stats	tx_stats;
	unsigned char		*tx_buf[NUM_TX_DESC];	/* Tx bounce buffers */
	unsigned char		*tx_bufs;	/* Tx bounce buffer region. */
	dma_addr_t		tx_bufs_dma;
	spinlock_t		lock;
	// unsigned int		regs_len;
};


static int rtl8139_open (struct net_device *dev);
static void rtl8139_init_ring (struct net_device *dev);
static netdev_tx_t rtl8139_start_xmit (struct sk_buff *skb,
				       struct net_device *dev);
static irqreturn_t rtl8139_interrupt (int irq, void *dev_instance);
static int rtl8139_close (struct net_device *dev);
static void rtl8139_hw_start (struct net_device *dev);


/* write MMIO register, with flush */
/* Flush avoids rtl8139 bug w/ posted MMIO writes */
#define RTL_W8_F(reg, val8)	do { iowrite8 ((val8), ioaddr + (reg)); ioread8 (ioaddr + (reg)); } while (0)
#define RTL_W16_F(reg, val16)	do { iowrite16 ((val16), ioaddr + (reg)); ioread16 (ioaddr + (reg)); } while (0)
#define RTL_W32_F(reg, val32)	do { iowrite32 ((val32), ioaddr + (reg)); ioread32 (ioaddr + (reg)); } while (0)

/* write MMIO register */
#define RTL_W8(reg, val8)	iowrite8 ((val8), ioaddr + (reg))
#define RTL_W16(reg, val16)	iowrite16 ((val16), ioaddr + (reg))
#define RTL_W32(reg, val32)	iowrite32 ((val32), ioaddr + (reg))

/* read MMIO register */
#define RTL_R8(reg)		ioread8 (ioaddr + (reg))
#define RTL_R16(reg)		ioread16 (ioaddr + (reg))
#define RTL_R32(reg)		ioread32 (ioaddr + (reg))

#define WRITEB_F(val8 ,  regaddr)   do { iowrite8 ( (val8) ,  (regaddr) ); ioread8  (regaddr); } while (0)
#define WRITEW_F(val16 , regaddr)   do { iowrite16 ( (val16), (regaddr) ); ioread16 (regaddr); } while (0)
#define WRITEL_F(val32 , regaddr)   do { iowrite32 ( (val32), (regaddr) ); ioread32 (regaddr); } while (0)



static const u16 rtl8139_intr_mask =
	PCIErr | PCSTimeout | RxUnderrun | RxOverflow | RxFIFOOver |
	TxErr | TxOK | RxErr | RxOK;

static const u16 rtl8139_norx_intr_mask =
	PCIErr | PCSTimeout | RxUnderrun |
	TxErr | TxOK | RxErr ;


static void __rtl8139_cleanup_dev (struct net_device *dev)
{
	struct rtl8139 *tp = netdev_priv(dev);
	struct pci_dev *pdev;

	pdev = tp->pci_dev;

	if (tp->mmio_addr){
		pci_iounmap (pdev, tp->mmio_addr);
		printk("IF PASSED\n");
	}

	/* it's ok to call this even if we have no regions to free */
	pci_release_regions (pdev);
	free_netdev(dev);
}


static void rtl8139_chip_reset (void __iomem *ioaddr)
{
	int i;

	/* Soft reset the chip. */
	RTL_W8 (ChipCmd, CmdReset);

	/* Check that the chip has finished the reset. */
	for (i = 1000; i > 0; i--) {
		barrier();
		if ((RTL_R8 (ChipCmd) & CmdReset) == 0)
			break;
		udelay (10);
	}
}


static const struct net_device_ops rtl8139_netdev_ops = {
	.ndo_open		= rtl8139_open,
	.ndo_stop		= rtl8139_close,
	.ndo_start_xmit		= rtl8139_start_xmit,
};


static void rtl8139_remove_one(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata (pdev);
	unregister_netdev (dev);

	__rtl8139_cleanup_dev (dev);
	pci_disable_device (pdev);

}

static u64 pci_regs_test(void __iomem *regs)
{
	u32 mac1, mac2;
	u64 mac_whole;

	mac1 = ioread32(regs);
	mac2 = ioread32(regs + 4);

	mac_whole = ((u64) mac2 << 32) | mac1;
	printk(KERN_INFO "Complete MAC address read from io regs is %pM - translated\n", \
			&mac_whole);

	return mac_whole;
}

static int rtl8139_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct net_device *dev;
	struct rtl8139 *tp;
	int rc;
	void __iomem *regs;
	resource_size_t pciaddr;
	unsigned int addr_len, i, pci_using_dac;
        u64 dev_mac;

	dev = alloc_etherdev(sizeof(struct rtl8139));
	tp = netdev_priv(dev);
	tp->pci_dev = pdev;
	tp->dev = dev;
	spin_lock_init (&tp->lock);
	rc = pci_enable_device(pdev);
	rc = pci_set_mwi(pdev);
	rc = pci_request_regions(pdev, DRV_NAME);
	pciaddr = pci_resource_start(pdev, 1);
		if (pci_resource_len(pdev, 1) < CP_REGS_SIZE) {
		rc = -EIO;
		dev_err(&pdev->dev, "MMIO resource (%llx) too small\n",
		       (unsigned long long)pci_resource_len(pdev, 1));
		
	}
		if ((sizeof(dma_addr_t) > 4) &&
	    !pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64)) &&
	    !pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) {
		pci_using_dac = 1;
	} else {
		pci_using_dac = 0;

		rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (rc) {
			dev_err(&pdev->dev,
				"No usable DMA configuration, aborting\n");
		}
		rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		if (rc) {
			dev_err(&pdev->dev,
				"No usable consistent DMA configuration, aborting\n");
	
		}
	}
	regs = ioremap(pciaddr, CP_REGS_SIZE);
	tp->mmio_addr = regs;

    dev_mac = pci_regs_test(tp->mmio_addr);
    memcpy(dev->dev_addr, &dev_mac, 6);

	printk("Device name is : %s\n", dev -> name);
	memcpy(dev -> name, DRV_NAME, strlen(DRV_NAME)*sizeof(char));
	printk("Device name is : %s\n", dev -> name);

	dev->netdev_ops = &rtl8139_netdev_ops;
	rc = register_netdev(dev);
	pci_set_drvdata(pdev, dev);
	pci_set_master(pdev);

	return 0;

}

static int rtl8139_open (struct net_device *dev)
{
	struct rtl8139 *tp = netdev_priv(dev);
	void __iomem *ioaddr = tp->mmio_addr;
	const int irq = tp->pci_dev->irq;
	int retval;

	retval = request_irq(irq, rtl8139_interrupt, IRQF_SHARED, dev->name, dev);
	if (retval)
		return retval;


	tp->tx_bufs = dma_alloc_coherent(&tp->pci_dev->dev, TX_BUF_TOT_LEN,
					   &tp->tx_bufs_dma, GFP_KERNEL);

	tp->rx_ring = dma_alloc_coherent(&tp->pci_dev->dev, RX_BUF_TOT_LEN,
					   &tp->rx_ring_dma, GFP_KERNEL);
	if (tp->tx_bufs == NULL || tp->rx_ring == NULL) {
		free_irq(irq, dev);

		if (tp->tx_bufs)
			dma_free_coherent(&tp->pci_dev->dev, TX_BUF_TOT_LEN,
					    tp->tx_bufs, tp->tx_bufs_dma);
		if (tp->rx_ring)
			dma_free_coherent(&tp->pci_dev->dev, RX_BUF_TOT_LEN,
					    tp->rx_ring, tp->rx_ring_dma);
	
		return -ENOMEM;
	}

	// tp->tx_flag = (TX_FIFO_THRESH << 11) & 0x003f0000;
	rtl8139_init_ring (dev);
	rtl8139_hw_start (dev);
	netif_start_queue (dev);

	return 0;
}

/* Start the hardware at open or resume. */
static void rtl8139_hw_start (struct net_device *dev)
{
	struct rtl8139 *tp = netdev_priv(dev);
	void __iomem *ioaddr = tp->mmio_addr;
	u32 i;
	u8 tmp;
	printk("hw_start: Hello\n");
	/* Bring old chips out of low-power mode. */

	rtl8139_chip_reset (ioaddr);

	tp->cur_rx = 0;

	/* init Rx ring buffer DMA address */
	RTL_W32_F (RxBuf, tp->rx_ring_dma);

	/* Must enable Tx/Rx before setting transfer thresholds! */
	RTL_W8 (ChipCmd, CmdRxEnb | CmdTxEnb);


	/* init Tx buffer DMA addresses */
	for (i = 0; i < NUM_TX_DESC; i++)
		RTL_W32_F (TxAddr0 + (i * 4), tp->tx_bufs_dma + (tp->tx_buf[i] - tp->tx_bufs));

	RTL_W32 (RxMissed, 0);

	/* no early-rx interrupts */
	RTL_W16 (MultiIntr, RTL_R16 (MultiIntr) & MultiIntrClear);

	/* make sure RxTx has started */
	tmp = RTL_R8 (ChipCmd);
	if ((!(tmp & CmdRxEnb)) || (!(tmp & CmdTxEnb)))
		RTL_W8 (ChipCmd, CmdRxEnb | CmdTxEnb);

	/* Enable all known interrupts by setting the interrupt mask. */
	RTL_W16 (IntrMask, rtl8139_intr_mask);
	printk("hw_start: Bye\n");
}

/* Initialize the Rx and Tx rings, along with various 'dev' bits. */
static void rtl8139_init_ring (struct net_device *dev)
{
	struct rtl8139 *tp = netdev_priv(dev);
	int i;
	printk("init_ring: Hello\n");
	tp->cur_rx = 0;
	tp->cur_tx = 0;
	tp->dirty_tx = 0;

	for (i = 0; i < NUM_TX_DESC; i++)
		tp->tx_buf[i] = &tp->tx_bufs[i * TX_BUF_SIZE];
	printk("init_ring: Bye\n");
}

static inline void rtl8139_tx_clear (struct rtl8139 *tp)
{
	tp->cur_tx = 0;
	tp->dirty_tx = 0;

	/* XXX account for unsent Tx packets in tp->stats.tx_dropped */
}

static netdev_tx_t rtl8139_start_xmit (struct sk_buff *skb,
					     struct net_device *dev)
{
	struct rtl8139 *tp = netdev_priv(dev);
	void __iomem *ioaddr = tp->mmio_addr;
	unsigned int entry;
	unsigned int len = skb->len;
	unsigned long flags;

	/* Calculate the next Tx descriptor entry. */
	entry = tp->cur_tx % NUM_TX_DESC;

	/* Note: the chip doesn't have auto-pad! */
	if (likely(len < TX_BUF_SIZE)) {
		if (len < ETH_ZLEN)
			memset(tp->tx_buf[entry], 0, ETH_ZLEN);
		skb_copy_and_csum_dev(skb, tp->tx_buf[entry]);
		dev_kfree_skb_any(skb);
	} else {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&tp->lock, flags);

	wmb();
	RTL_W32_F (TxStatus0 + (entry * sizeof (u32)), tp->tx_flag | max(len, (unsigned int)ETH_ZLEN));

	tp->cur_tx++;

	if ((tp->cur_tx - NUM_TX_DESC) == tp->dirty_tx)
		netif_stop_queue (dev);
	spin_unlock_irqrestore(&tp->lock, flags);

	netif_dbg(tp, tx_queued, dev, "Queued Tx packet size %u to slot %d\n",
		  len, entry);

	return NETDEV_TX_OK;
}


static void rtl8139_tx_interrupt (struct net_device *dev,struct rtl8139 *tp,void __iomem *ioaddr)
{
	unsigned long dirty_tx, tx_left;

	// assert (dev != NULL);
	// assert (ioaddr != NULL);

	dirty_tx = tp->dirty_tx;
	tx_left = tp->cur_tx - dirty_tx;
	while (tx_left > 0) {
		int entry = dirty_tx % NUM_TX_DESC;
		int txstatus;

		txstatus = RTL_R32 (TxStatus0 + (entry * sizeof (u32)));
		if (!(txstatus & (TxStatOK | TxUnderrun | TxAborted)))
			break;	/* It still hasn't been Txed */

		if(txstatus & TxStatOK) { /* Successfully transmitted */
            printk("Packet is transmitted, TxStatOK bit is set\n");
            u64_stats_update_begin(&tp->tx_stats.syncp);
			tp->tx_stats.packets++;
			tp->tx_stats.bytes += txstatus & 0x7ff;
			u64_stats_update_end(&tp->tx_stats.syncp);

		}
        else {
            dev->stats.tx_errors++;
		    if ( txstatus & TxAborted )
                dev->stats.tx_aborted_errors++;
            if ( txstatus & TxUnderrun )
                dev->stats.tx_fifo_errors++;
            if ( txstatus * TxOutOfWindow )
            	dev->stats.tx_window_errors++;
        	if ( txstatus * TxCarrierLost )
            	dev->stats.tx_carrier_errors++;
        }

		dirty_tx++;
		tx_left--;
	}

	if (tp->cur_tx - dirty_tx > NUM_TX_DESC) {
		netdev_err(dev, "Out-of-sync dirty pointer, %ld vs. %ld\n",
			   dirty_tx, tp->cur_tx);
		dirty_tx += NUM_TX_DESC;
	}


	/* only wake the queue if we did work, and the queue is stopped */
	if (tp->dirty_tx != dirty_tx) {
		tp->dirty_tx = dirty_tx;
		mb();
		netif_wake_queue (dev);
	}
}

static void rtl8139_rx_interrupt (struct net_device *dev,struct rtl8139 *tp,void __iomem *ioaddr)
{
	while((RTL_R8 (ChipCmd) & RxBufEmpty) == 0){
		unsigned int rx_status;
    	unsigned short rx_size;
        unsigned short pkt_size;
        struct sk_buff *skb;

		if(tp->cur_rx > RX_BUF_LEN)
            tp->cur_rx = tp->cur_rx % RX_BUF_LEN;
		rx_status = *(u32 *)(tp->rx_ring + tp->cur_rx);
		rx_size = rx_status >> 16;

		pkt_size = rx_size - 4;
		skb = napi_alloc_skb(&tp->napi, pkt_size);
		if (skb) {
            skb->dev = dev;
			memcpy(skb, tp->rx_ring + tp->cur_rx + 4, pkt_size);
			skb_put (skb, pkt_size);
            skb->protocol = eth_type_trans (skb, dev);
			netif_receive_skb (skb);

			u64_stats_update_begin(&tp->rx_stats.syncp);
			tp->rx_stats.packets++;
			tp->rx_stats.bytes += pkt_size;
			u64_stats_update_end(&tp->rx_stats.syncp);
		}

        else {
		    printk (KERN_WARNING "%s: dropping packet.\n", dev->name);
          	/* CODE HERE */
            dev->stats.rx_errors++;
			/* Update detailed RX error-counters */
            if ( rx_status & (1 << 15) )
                dev->stats.multicast++;
            if ( rx_status & ((1 << 4)|(1 << 3)) )
                dev->stats.rx_length_errors++;
            if ( rx_status & (1 << 2) )
                dev->stats.rx_crc_errors++;
            if ( rx_status & (1 << 1) )
                dev->stats.rx_frame_errors++;
        }

		tp->cur_rx = (tp->cur_rx + rx_size + 4 + 3) & ~3;
		RTL_W16 (RxBufPtr, (u16) (tp->cur_rx - 16));
	}
}
/* The interrupt handler does all of the Rx thread work and cleans up
   after the Tx thread. */
static irqreturn_t rtl8139_interrupt (int irq, void *dev_instance)
{
	struct net_device *dev = (struct net_device *) dev_instance;
	struct rtl8139 *tp = netdev_priv(dev);
	void __iomem *ioaddr = tp->mmio_addr;
	u16 status, ackstat;
	int link_changed = 0; /* avoid bogus "uninit" warning */
	int handled = 0;

	spin_lock (&tp->lock);
	status = RTL_R16 (IntrStatus);

	/* shared irq? */
	if (unlikely((status & rtl8139_intr_mask) == 0))
		goto out;

	handled = 1;

	/* h/w no longer present (hotplug?) or major error, bail */
	if (unlikely(status == 0xFFFF))
		goto out;


	if (status & (TxOK | TxErr)) {
		printk("rtl8139_interrupt : TxOK | TxErr is bening called\n");
		rtl8139_tx_interrupt (dev, tp, ioaddr);
		if (status & TxErr)
			RTL_W16 (IntrStatus, TxErr);
	}
	if (status & RxOK) {
		printk("rtl8139_interrupt : RxOK | RxErr is bening called\n");
		rtl8139_rx_interrupt (dev, tp, ioaddr);
		if (status & TxErr)
			RTL_W16 (IntrStatus, RxErr);
	}

 out:
	spin_unlock (&tp->lock);

	netdev_dbg(dev, "exiting interrupt, intr_status=%#4.4x\n", RTL_R16(IntrStatus));
	return IRQ_RETVAL(handled);
}

static int rtl8139_close (struct net_device *dev)
{
	struct rtl8139 *tp = netdev_priv(dev);
	void __iomem *ioaddr = tp->mmio_addr;
	unsigned long flags;

	netif_stop_queue(dev);

	netif_dbg(tp, ifdown, dev, "Shutting down ethercard, status was 0x%04x\n", RTL_R16(IntrStatus));
	

	spin_lock_irqsave (&tp->lock, flags);

	/* Stop the chip's Tx and Rx DMA processes. */
	RTL_W8 (ChipCmd, 0);
	

	/* Disable interrupts by clearing the interrupt mask. */
	RTL_W16 (IntrMask, 0);
	
	/* Update the error counts. */
	dev->stats.rx_missed_errors += RTL_R32 (RxMissed);
	RTL_W32 (RxMissed, 0);


	spin_unlock_irqrestore (&tp->lock, flags);

	free_irq(tp->pci_dev->irq, dev);

	rtl8139_tx_clear (tp);

	dma_free_coherent(&tp->pci_dev->dev, RX_BUF_TOT_LEN,
			  tp->rx_ring, tp->rx_ring_dma);
	dma_free_coherent(&tp->pci_dev->dev, TX_BUF_TOT_LEN,
			  tp->tx_bufs, tp->tx_bufs_dma);
	tp->rx_ring = NULL;
	tp->tx_bufs = NULL;

	return 0;
}

/* Set or clear the multicast filter for this adaptor.
   This routine is not state sensitive and need not be SMP locked. */

static struct pci_driver rtl8139_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= rtl8139_pci_tbl,
	.probe		= rtl8139_init_one,
	.remove		= rtl8139_remove_one,
};


static int __init rtl8139_init_module (void)
{
	
// #ifdef MODULE
// 	pr_info(RTL8139_DRIVER_NAME "\n");
// #endif

	return pci_register_driver(&rtl8139_pci_driver);
}

static void __exit rtl8139_cleanup_module (void)
{
	pci_unregister_driver (&rtl8139_pci_driver);
}
module_init(rtl8139_init_module);
module_exit(rtl8139_cleanup_module);