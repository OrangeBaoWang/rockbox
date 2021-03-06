/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2016 by Roman Stolyarov
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
/*#define LOGF_ENABLE*/
#include "logf.h"
#include "system.h"
#include "usb_ch9.h"
#include "usb_drv.h"
#include "usb_core.h"
#include "cpu.h"
#include "thread.h"

#define PIN_USB_DET      (32*4+19)
#define IRQ_USB_DET      GPIO_IRQ(PIN_USB_DET)
#define GPIO_USB_DET     GPIO147

#define PIN_USB_DRVVBUS  (32*4+10)
#define PIN_USB_OTG_ID   (32*3+7)

#define EP_BUF_LEFT(ep)  ((ep)->length - (ep)->sent)
#define EP_PTR(ep)       ((void*)((unsigned int)(ep)->buf + (ep)->sent))
#define EP_NUMBER(ep)    (((int)(ep) - (int)&endpoints[0])/sizeof(struct usb_endpoint))
#define EP_NUMBER2(ep)   (EP_NUMBER((ep))/2)
#define TOTAL_EP()       (sizeof(endpoints)/sizeof(struct usb_endpoint))
#define EP_IS_IN(ep)     (EP_NUMBER((ep))%2)

enum ep_type
{
    ep_control,
    ep_bulk,
    ep_interrupt,
    ep_isochronous
};

struct usb_endpoint
{
    volatile void *buf;
    volatile size_t length;
    union
    {
        volatile size_t sent;
        volatile size_t received;
    };
    volatile bool busy;

    const enum ep_type type;
    const bool use_dma;

    const long fifo_addr;
    unsigned short fifo_size;

    volatile bool wait;
    struct semaphore complete;

    volatile int rc;
    bool allocated;
};

#define EP_INIT(_type, _fifo_addr, _fifo_size, _buf, _use_dma) \
    { .type = (_type), .fifo_addr = (_fifo_addr), .fifo_size = (_fifo_size), \
      .buf = (_buf), .use_dma = (_use_dma), \
      .length = 0, .busy = false, .wait = false, .allocated = false }

static union
{
    int buf[64 / sizeof(int)];
    struct usb_ctrlrequest request;
} ep0_rx;

static volatile bool ep0_data_supplied = false;
static volatile bool ep0_data_requested = false;

static struct usb_endpoint endpoints[] =
{
    EP_INIT(ep_control,   USB_FIFO_EP(0),  64, NULL,        false),
    EP_INIT(ep_control,   USB_FIFO_EP(0),  64, &ep0_rx.buf, false),
    EP_INIT(ep_bulk,      USB_FIFO_EP(1), 512, NULL,        false),
    EP_INIT(ep_bulk,      USB_FIFO_EP(1), 512, NULL,        false),
    EP_INIT(ep_interrupt, USB_FIFO_EP(2), 512, NULL,        false),
    EP_INIT(ep_interrupt, USB_FIFO_EP(2), 512, NULL,        false),
};

static inline void select_endpoint(int ep)
{
    REG_USB_INDEX = ep;
}

static void readFIFO(struct usb_endpoint *ep, unsigned int size)
{
    logf("%s(EP%d, %d)", __func__, EP_NUMBER2(ep), size);

    register unsigned char *ptr = (unsigned char*)EP_PTR(ep);
    register unsigned int *ptr32 = (unsigned int*)ptr;
    register unsigned int s = size >> 2;
    register unsigned int x;

    if(size > 0)
    {
        if( ((unsigned int)ptr & 3) == 0 )
        {
            while(s--)
                *ptr32++ = REG32(ep->fifo_addr);

            ptr = (unsigned char*)ptr32;
        }
        else
        {
            while(s--)
            {
                x = REG32(ep->fifo_addr);
                *ptr++ = x & 0xFF; x >>= 8;
                *ptr++ = x & 0xFF; x >>= 8;
                *ptr++ = x & 0xFF; x >>= 8;
                *ptr++ = x;
            }
        }

        s = size & 3;
        while(s--)
            *ptr++ = REG8(ep->fifo_addr);
    }
}

static void writeFIFO(struct usb_endpoint *ep, size_t size)
{
    logf("%s(EP%d, %d)", __func__, EP_NUMBER2(ep), size);

    register unsigned int *d32 = (unsigned int *)EP_PTR(ep);
    register unsigned char *d8 = (unsigned char *)d32;
    register size_t s = size >> 2;
    register unsigned int x;

    if(size > 0)
    {
        if( ((unsigned int)d8 & 3) == 0 )
        {
            while (s--)
                REG32(ep->fifo_addr) = *d32++;
            d8 = (unsigned char *)d32;
        }
        else
        {
            while (s--)
            {
                x = (unsigned int)(*d8++) & 0xff;
                x |= ((unsigned int)(*d8++) & 0xff) << 8;
                x |= ((unsigned int)(*d8++) & 0xff) << 16;
                x |= ((unsigned int)(*d8++) & 0xff) << 24;
                REG32(ep->fifo_addr) = x;
            }
        }

        if( (s = size & 3) )
        {
            while (s--)
                REG8(ep->fifo_addr) = *d8++;
        }
    }
}

static void flushFIFO(struct usb_endpoint *ep)
{
    logf("%s(%d)", __func__, EP_NUMBER(ep));

    switch (ep->type)
    {
        case ep_control:
        break;

        case ep_bulk:
        case ep_interrupt:
        case ep_isochronous:
            if(EP_IS_IN(ep))
                REG_USB_INCSR |= (USB_INCSR_FF | USB_INCSR_CDT);
            else
                REG_USB_OUTCSR |= (USB_OUTCSR_FF | USB_OUTCSR_CDT);
        break;
    }
}

static inline void ep_transfer_completed(struct usb_endpoint* ep)
{
    ep->sent   = 0;
    ep->length = 0;
    ep->buf    = NULL;
    ep->busy   = false;
    if(ep->wait)
        semaphore_release(&ep->complete);
}

static void EP0_send(void)
{
    struct usb_endpoint* ep = &endpoints[0];
    unsigned int length;
    unsigned short csr0;

    select_endpoint(0);
    csr0 = REG_USB_CSR0;

    if(ep->sent == 0)
    {
        length = MIN(ep->length, ep->fifo_size);
        REG_USB_CSR0 = (csr0 | USB_CSR0_FLUSHFIFO);
    }
    else
        length = MIN(EP_BUF_LEFT(ep), ep->fifo_size);

    writeFIFO(ep, length);
    ep->sent += length;

    if(ep->sent >= ep->length)
    {
        REG_USB_CSR0 = (csr0 | USB_CSR0_INPKTRDY | USB_CSR0_DATAEND); /* Set data end! */
        if (!ep->wait)
            usb_core_transfer_complete(0, USB_DIR_IN, 0, ep->sent);
        ep->rc = 0;
        ep_transfer_completed(ep);
    }
    else
        REG_USB_CSR0 = (csr0 | USB_CSR0_INPKTRDY);
}

static void EP0_handler(void)
{
    unsigned short csr0;
    struct usb_endpoint *ep_send = &endpoints[0];
    struct usb_endpoint *ep_recv = &endpoints[1];

    /* Read CSR0 */
    select_endpoint(0);
    csr0 = REG_USB_CSR0;

    logf("%s(): 0x%x", __func__, csr0);

    /* Check for SentStall:
        This bit is set when a STALL handshake is transmitted. The CPU should clear this bit.
     */
    if(csr0 & USB_CSR0_SENTSTALL)
    {
        REG_USB_CSR0 = csr0 & ~USB_CSR0_SENTSTALL;
        return;
    }

    /* Check for SetupEnd:
        This bit will be set when a control transaction ends before the DataEnd bit has been set.
        An interrupt will be generated and the FIFO flushed at this time.
        The bit is cleared by the CPU writing a 1 to the ServicedSetupEnd bit.
     */
    if(csr0 & USB_CSR0_SETUPEND)
    {
        csr0 |= USB_CSR0_SVDSETUPEND;
        REG_USB_CSR0 = csr0;
        ep0_data_supplied = false;
        ep0_data_requested = false;
        if (ep_send->busy)
        {
            if (!ep_send->wait)
                usb_core_transfer_complete(0, USB_DIR_IN, -1, 0);
            ep_transfer_completed(ep_send);
        }
        if (ep_recv->busy)
        {
            usb_core_transfer_complete(0, USB_DIR_OUT, -1, 0);
            ep_transfer_completed(ep_recv);
        }
    }

    /* Call relevant routines for endpoint 0 state */
    if(csr0 & USB_CSR0_OUTPKTRDY)   /* There is a packet in the fifo */
    {
        if (ep_send->busy)
        {
            if (!ep_send->wait)
                usb_core_transfer_complete(0, USB_DIR_IN, -1, 0);
            ep_transfer_completed(ep_send);
        }
        if (ep_recv->busy && ep_recv->buf && ep_recv->length)
        {
            unsigned int size = REG_USB_COUNT0;
            readFIFO(ep_recv, size);
            ep_recv->received += size;
            if (size < ep_recv->fifo_size || ep_recv->received >= ep_recv->length)
            {
                REG_USB_CSR0 = csr0 | USB_CSR0_SVDOUTPKTRDY | USB_CSR0_DATAEND; /* Set data end! */
                usb_core_transfer_complete(0, USB_DIR_OUT, 0, ep_recv->received);
                ep_transfer_completed(ep_recv);
            }
            else REG_USB_CSR0 = csr0 | USB_CSR0_SVDOUTPKTRDY; /* clear OUTPKTRDY bit */
        }
        else if (!ep0_data_supplied)
        {
            ep_recv->buf = ep0_rx.buf;
            readFIFO(ep_recv, REG_USB_COUNT0);
            csr0 |= USB_CSR0_SVDOUTPKTRDY;
            if (!ep0_rx.request.wLength)
            {
                csr0 |= USB_CSR0_DATAEND; /* Set data end! */
                ep0_data_requested = false;
                ep0_data_supplied = false;
            }
            else if (ep0_rx.request.bRequestType & USB_DIR_IN)
                ep0_data_requested = true;
            else ep0_data_supplied = true;
            REG_USB_CSR0 = csr0;
            usb_core_control_request(&ep0_rx.request);
            ep_transfer_completed(ep_recv);
        }
    }
    else if (ep_send->busy)
        EP0_send();
}

static void EPIN_handler(unsigned int endpoint)
{
    struct usb_endpoint* ep = &endpoints[endpoint*2];
    unsigned int length, csr;

    select_endpoint(endpoint);
    csr = REG_USB_INCSR;
    logf("%s(%d): 0x%x", __func__, endpoint, csr);

    if(!ep->busy)
    {
        logf("Entered EPIN handler without work!");
        return;
    }

    if(csr & USB_INCSR_SENTSTALL)
    {
        REG_USB_INCSR = csr & ~USB_INCSR_SENTSTALL;
        return;
    }

    if(ep->use_dma)
        return;

    if(csr & USB_INCSR_FFNOTEMPT)
    {
        logf("FIFO is not empty! 0x%x", csr);
        return;
    }

    logf("EP%d: %d -> %d", endpoint, ep->sent, ep->length);
    
    if(ep->sent == 0)
        length = MIN(ep->length, ep->fifo_size);
    else
        length = MIN(EP_BUF_LEFT(ep), ep->fifo_size);

    writeFIFO(ep, length);
    REG_USB_INCSR = csr | USB_INCSR_INPKTRDY;
    ep->sent += length;

    if(ep->sent >= ep->length)
    {
        if (!ep->wait)
            usb_core_transfer_complete(endpoint, USB_DIR_IN, 0, ep->sent);
        ep->rc = 0;
        ep_transfer_completed(ep);
        logf("sent complete");
    }
}

static void EPOUT_handler(unsigned int endpoint)
{
    struct usb_endpoint* ep = &endpoints[endpoint*2+1];
    unsigned int size, csr;

    if(!ep->busy)
    {
        logf("Entered EPOUT handler without work!");
        return;
    }

    select_endpoint(endpoint);
    while((csr = REG_USB_OUTCSR) & (USB_OUTCSR_SENTSTALL|USB_OUTCSR_OUTPKTRDY))
    {
        logf("%s(%d): 0x%x", __func__, endpoint, csr);
        if(csr & USB_OUTCSR_SENTSTALL)
        {
            logf("stall sent, flushing fifo..");
            flushFIFO(ep);
            REG_USB_OUTCSR = csr & ~USB_OUTCSR_SENTSTALL;
            return;
        }

        if(ep->use_dma)
            return;

        if(csr & USB_OUTCSR_OUTPKTRDY) /* There is a packet in the fifo */
        {
            size = REG_USB_OUTCOUNT;

            readFIFO(ep, size);
            ep->received += size;

            /*if(csr & USB_OUTCSR_FFFULL)
                csr &= ~USB_OUTCSR_FFFULL;*/

            REG_USB_OUTCSR = csr & ~USB_OUTCSR_OUTPKTRDY;

            logf("received: %d max length: %d", ep->received, ep->length);

            if(size < ep->fifo_size || ep->received >= ep->length)
            {
                usb_core_transfer_complete(endpoint, USB_DIR_OUT, 0, ep->received);
                ep_transfer_completed(ep);
                logf("receive transfer_complete");
            }
        }
    }
}

static void EPDMA_handler(int number)
{
    int endpoint = -1;
    unsigned int size = 0;
    struct usb_endpoint* ep = NULL;

    if(number == USB_INTR_DMA_BULKIN)
    {
        endpoint = (REG_USB_CNTL(0) >> 4) & 0xF;
        ep = &endpoints[endpoint*2];
        size = (unsigned int)ep->buf - REG_USB_ADDR(0);
    }
    else if(number == USB_INTR_DMA_BULKOUT)
    {
        endpoint = (REG_USB_CNTL(1) >> 4) & 0xF;
        ep = &endpoints[endpoint*2+1];
        size = (unsigned int)ep->buf - REG_USB_ADDR(1);
    }

    logf("DMA_BULK%d %d", number, endpoint);

    if(number == USB_INTR_DMA_BULKOUT)
    {
        /* Disable DMA */
        REG_USB_CNTL(1) = 0;

        __dcache_invalidate_all();

        select_endpoint(endpoint);
        /* Read out last packet manually */
        unsigned int lpack_size = REG_USB_OUTCOUNT;
        if(lpack_size > 0)
        {
            ep->buf += ep->length - lpack_size;
            readFIFO(ep, lpack_size);
            REG_USB_OUTCSR &= ~USB_OUTCSR_OUTPKTRDY;
        }
    }
    else if(number == USB_INTR_DMA_BULKIN && size % ep->fifo_size)
    {
        /* If the last packet is less than MAXP, set INPKTRDY manually */
        REG_USB_INCSR |= USB_INCSR_INPKTRDY;
    }

    if (ep)
    {
        int dir = EP_IS_IN(ep) ? USB_DIR_IN : USB_DIR_OUT;
        if ((dir ==  USB_DIR_OUT) || !ep->wait)
            usb_core_transfer_complete(endpoint, dir, 0, ep->length);
        ep->rc = 0;
        ep_transfer_completed(ep);
    }
}

static void setup_endpoint(struct usb_endpoint *ep)
{
    int endpoint = EP_NUMBER2(ep);
    unsigned char csr, csrh;

    select_endpoint(endpoint);

    if (ep->busy)
    {
        if(EP_IS_IN(ep))
        {
            if (ep->wait)
                semaphore_release(&ep->complete);
            else usb_core_transfer_complete(endpoint, USB_DIR_IN, -1, 0);
        }
        else usb_core_transfer_complete(endpoint, USB_DIR_OUT, -1, 0);
    }

    ep->busy = false;
    ep->wait = false;
    ep->sent = 0;
    ep->length = 0;

    if(ep->type != ep_control)
        ep->fifo_size = usb_drv_port_speed() ? 512 : 64;

    if(EP_IS_IN(ep))
    {
        csr = (USB_INCSR_FF | USB_INCSR_CDT);
        csrh = USB_INCSRH_MODE;

        if(ep->use_dma)
            csrh |= (USB_INCSRH_DMAREQENAB | USB_INCSRH_AUTOSET | USB_INCSRH_DMAREQMODE);

        if(ep->type == ep_interrupt)
            csrh |= USB_INCSRH_FRCDATATOG;

        REG_USB_INMAXP   = ep->fifo_size;
        REG_USB_INCSR    = csr;
        REG_USB_INCSRH   = csrh;

        if (ep->allocated)
            REG_USB_INTRINE |= USB_INTR_EP(EP_NUMBER2(ep));
    }
    else
    {
        csr = (USB_OUTCSR_FF | USB_OUTCSR_CDT);
        csrh = 0;

        if(ep->type == ep_interrupt)
            csrh |= USB_OUTCSRH_DNYT;

        if(ep->use_dma)
            csrh |= (USB_OUTCSRH_DMAREQENAB | USB_OUTCSRH_AUTOCLR | USB_OUTCSRH_DMAREQMODE);

        REG_USB_OUTMAXP   = ep->fifo_size;
        REG_USB_OUTCSR    = csr;
        REG_USB_OUTCSRH   = csrh;

        if (ep->allocated)
            REG_USB_INTROUTE |= USB_INTR_EP(EP_NUMBER2(ep));
    }
}

static void udc_reset(void)
{
    /* From the datasheet:

       When a reset condition is detected on the USB, the controller performs the following actions:
           * Sets FAddr to 0.
           * Sets Index to 0.
           * Flushes all endpoint FIFOs.
           * Clears all control/status registers.
           * Enables all endpoint interrupts.
           * Generates a Reset interrupt.
    */

    logf("%s()", __func__);

    unsigned int i;

    REG_USB_FADDR = 0;
    REG_USB_INDEX = 0;

    /* Disable interrupts */
    REG_USB_INTRINE  = 0;
    REG_USB_INTROUTE = 0;
    REG_USB_INTRUSBE = 0;

    /* Disable DMA */
    REG_USB_CNTL(0) = 0;
    REG_USB_CNTL(1) = 0;

    /* High speed, softconnect */
    REG_USB_POWER = (USB_POWER_SOFTCONN | USB_POWER_HSENAB);

    /* Reset EP0 */
    select_endpoint(0);
    REG_USB_CSR0 = (USB_CSR0_SVDOUTPKTRDY | USB_CSR0_SVDSETUPEND | USB_CSR0_FLUSHFIFO);

    if (endpoints[0].busy)
    {
        if (endpoints[0].wait)
            semaphore_release(&endpoints[0].complete);
        else usb_core_transfer_complete(0, USB_DIR_IN, -1, 0);
    }

    endpoints[0].busy = false;
    endpoints[0].wait = false;
    endpoints[0].sent = 0;
    endpoints[0].length = 0;
    endpoints[0].allocated = true;

    if (endpoints[1].busy)
        usb_core_transfer_complete(0, USB_DIR_OUT, -1, 0);

    endpoints[1].busy = false;
    endpoints[1].wait = false;
    endpoints[1].received = 0;
    endpoints[1].length = 0;
    endpoints[1].allocated = true;

    /* Reset other endpoints */
    for(i=2; i<TOTAL_EP(); i++)
        setup_endpoint(&endpoints[i]);

    ep0_data_supplied = false;
    ep0_data_requested = false;

    /* Enable interrupts */
    REG_USB_INTRINE  |= USB_INTR_EP(0);
    REG_USB_INTRUSBE |= USB_INTR_RESET;

    usb_core_bus_reset();
}

/* Interrupt handler */
void OTG(void)
{
    /* Read interrupt registers */
    unsigned char    intrUSB = REG_USB_INTRUSB;
    unsigned short   intrIn  = REG_USB_INTRIN;
    unsigned short   intrOut = REG_USB_INTROUT;
    unsigned char    intrDMA = REG_USB_INTR;

    logf("%x %x %x %x", intrUSB, intrIn, intrOut, intrDMA);

    /* EPIN & EPOUT are all handled in DMA */
    if(intrIn & USB_INTR_EP(0))
        EP0_handler();
    if(intrIn & USB_INTR_EP(1))
        EPIN_handler(1);
    if(intrIn & USB_INTR_EP(2))
        EPIN_handler(2);
    if(intrOut & USB_INTR_EP(1))
        EPOUT_handler(1);
    if(intrOut & USB_INTR_EP(2))
        EPOUT_handler(2);
    if(intrUSB & USB_INTR_RESET)
        udc_reset();
    if(intrUSB & USB_INTR_SUSPEND)
        logf("USB suspend");
    if(intrUSB & USB_INTR_RESUME)
        logf("USB resume");
    if(intrDMA & USB_INTR_DMA_BULKIN)
        EPDMA_handler(USB_INTR_DMA_BULKIN);
    if(intrDMA & USB_INTR_DMA_BULKOUT)
        EPDMA_handler(USB_INTR_DMA_BULKOUT);
}

bool usb_drv_stalled(int endpoint, bool in)
{
    endpoint &= 0x7F;

    logf("%s(%d, %s)", __func__, endpoint, in?"IN":"OUT");

    select_endpoint(endpoint);

    if(endpoint == EP_CONTROL)
        return (REG_USB_CSR0 & USB_CSR0_SENDSTALL) != 0;
    else
    {
        if(in)
            return (REG_USB_INCSR & USB_INCSR_SENDSTALL) != 0;
        else
            return (REG_USB_OUTCSR & USB_OUTCSR_SENDSTALL) != 0;
    }
}

void usb_drv_stall(int endpoint, bool stall, bool in)
{
    endpoint &= 0x7F;

    logf("%s(%d,%s,%s)", __func__, endpoint, stall?"Y":"N", in?"IN":"OUT");

    select_endpoint(endpoint);

    if(endpoint == EP_CONTROL)
    {
        if(stall)
            REG_USB_CSR0 |= USB_CSR0_SENDSTALL;
        else
            REG_USB_CSR0 &= ~USB_CSR0_SENDSTALL;
    }
    else
    {
        if(in)
        {
            if(stall)
                REG_USB_INCSR |= USB_INCSR_SENDSTALL;
            else
                REG_USB_INCSR = (REG_USB_INCSR & ~USB_INCSR_SENDSTALL) | USB_INCSR_CDT;
        }
        else
        {
            if(stall)
                REG_USB_OUTCSR |= USB_OUTCSR_SENDSTALL;
            else
                REG_USB_OUTCSR = (REG_USB_OUTCSR & ~USB_OUTCSR_SENDSTALL) | USB_OUTCSR_CDT;
        }
    }
}

int usb_detect(void)
{
    return (__gpio_get_pin(PIN_USB_DET) == 1)
        ? USB_INSERTED : USB_EXTRACTED;
}

void usb_init_device(void)
{
    __gpio_clear_pin(PIN_USB_DRVVBUS);
    __gpio_as_output(PIN_USB_DRVVBUS);

    __gpio_as_input(PIN_USB_OTG_ID);
    __gpio_as_input(PIN_USB_DET);

    __gpio_disable_pull(PIN_USB_OTG_ID);
    __gpio_disable_pull(PIN_USB_DET);

#ifdef USB_STATUS_BY_EVENT
    __gpio_as_irq_rise_edge(PIN_USB_DET);
    system_enable_irq(IRQ_USB_DET);
#endif

    system_enable_irq(IRQ_OTG);

    for(unsigned i=0; i<TOTAL_EP(); i++)
        semaphore_init(&endpoints[i].complete, 1, 0);
}

#ifdef USB_STATUS_BY_EVENT
static int usb_oneshot_callback(struct timeout *tmo)
{
    (void)tmo;
    int state = usb_detect();

    /* This is called only if the state was stable for HZ/16 - check state
     * and post appropriate event. */
    usb_status_event(state);

    if(state == USB_EXTRACTED)
        __gpio_as_irq_rise_edge(PIN_USB_DET);
    else
        __gpio_as_irq_fall_edge(PIN_USB_DET);

    return 0;
}

void GPIO_USB_DET(void)
{
    static struct timeout usb_oneshot;
    timeout_register(&usb_oneshot, usb_oneshot_callback, (HZ/16), 0);
}
#endif

void usb_enable(bool on)
{
    if(on)
        usb_core_init();
    else
        usb_core_exit();
}

void usb_attach(void)
{
    usb_enable(true);
}

void usb_drv_init(void)
{
    logf("%s()", __func__);

    /* Dis- and reconnect from USB */
    REG_USB_POWER &= ~USB_POWER_SOFTCONN;
    mdelay(20);
    REG_USB_POWER |= USB_POWER_SOFTCONN;
    mdelay(20);

    udc_reset();
}

void usb_drv_exit(void)
{
    logf("%s()", __func__);

    REG_USB_FADDR = 0;
    REG_USB_INDEX = 0;

    /* Disable interrupts */
    REG_USB_INTRINE  = 0;
    REG_USB_INTROUTE = 0;
    REG_USB_INTRUSBE = 0;

    /* Disable DMA */
    REG_USB_CNTL(0) = 0;
    REG_USB_CNTL(1) = 0;

    /* Disconnect from USB */
    REG_USB_POWER &= ~USB_POWER_SOFTCONN;
}

void usb_drv_set_address(int address)
{
    logf("%s(%d)", __func__, address);

    REG_USB_FADDR = address;
}

static void usb_drv_send_internal(struct usb_endpoint* ep, void* ptr, int length, bool blocking)
{
    int flags = disable_irq_save();

    if(ep->type == ep_control)
    {
        if ((ptr == NULL && length == 0) || !ep0_data_requested)
        {
            restore_irq(flags);
            return;
        }
        ep0_data_requested = false;
    }

    ep->buf = ptr;
    ep->sent = 0;
    ep->length = length;
    ep->busy = true;
    if(blocking)
    {
        ep->rc = -1;
        ep->wait = true;
    }
    else ep->rc = 0;

    if(ep->type == ep_control)
    {
        EP0_send();
    }
    else
    {
        if(ep->use_dma)
        {
            //dma_cache_wback_inv((unsigned long)ptr, length);
            __dcache_writeback_all();
            REG_USB_ADDR(0) = PHYSADDR((unsigned long)ptr);
            REG_USB_COUNT(0) = length;
            REG_USB_CNTL(0) = (USB_CNTL_INTR_EN | USB_CNTL_MODE_1 |
                                 USB_CNTL_DIR_IN  | USB_CNTL_ENA |
                                 USB_CNTL_EP(EP_NUMBER2(ep)) | USB_CNTL_BURST_16);
        }
        else
            EPIN_handler(EP_NUMBER2(ep));
    }

    restore_irq(flags);

    if(blocking)
    {
        semaphore_wait(&ep->complete, HZ);
        ep->wait = false;
    }
}

int usb_drv_send_nonblocking(int endpoint, void* ptr, int length)
{
    struct usb_endpoint *ep = &endpoints[(endpoint & 0x7F)*2];

    logf("%s(%d, 0x%x, %d)", __func__, endpoint, (int)ptr, length);

    if (ep->allocated)
    {
        usb_drv_send_internal(ep, ptr, length, false);
        return 0;
    }

    return -1;
}

int usb_drv_send(int endpoint, void* ptr, int length)
{
    struct usb_endpoint *ep = &endpoints[(endpoint & 0x7F)*2];

    logf("%s(%d, 0x%x, %d)", __func__, endpoint, (int)ptr, length);

    if (ep->allocated)
    {
        usb_drv_send_internal(ep, ptr, length, true);
        return ep->rc;
    }

    return -1;
}

int usb_drv_recv(int endpoint, void* ptr, int length)
{
    int flags;
    struct usb_endpoint *ep;
    endpoint &= 0x7F;

    logf("%s(%d, 0x%x, %d)", __func__, endpoint, (int)ptr, length);

    if (ptr == NULL || length == 0)
        return 0;

    ep = &endpoints[endpoint*2+1];

    if (!ep->allocated)
        return -1;

    flags = disable_irq_save();

    ep->buf = ptr;
    ep->received = 0;
    ep->length = length;
    ep->busy = true;
    if(ep->use_dma)
    {
        //dma_cache_wback_inv((unsigned long)ptr, length);
        __dcache_writeback_all();
        REG_USB_ADDR(1) = PHYSADDR((unsigned long)ptr);
        REG_USB_COUNT(1) = length;
        REG_USB_CNTL(1) = (USB_CNTL_INTR_EN | USB_CNTL_MODE_1 |
                             USB_CNTL_ENA | USB_CNTL_EP(endpoint) |
                             USB_CNTL_BURST_16);
    }
    else
    {
        if (endpoint == EP_CONTROL)
        {
            ep0_data_supplied = false;
            EP0_handler();
        }
        else EPOUT_handler(endpoint);
    }

    restore_irq(flags);
    return 0;
}

void usb_drv_set_test_mode(int mode)
{
    logf("%s(%d)", __func__, mode);

    switch(mode)
    {
        case 0:
            REG_USB_TESTMODE &= ~USB_TEST_ALL;
            break;
        case 1:
            REG_USB_TESTMODE |= USB_TEST_J;
            break;
        case 2:
            REG_USB_TESTMODE |= USB_TEST_K;
            break;
        case 3:
            REG_USB_TESTMODE |= USB_TEST_SE0NAK;
            break;
        case 4:
            REG_USB_TESTMODE |= USB_TEST_PACKET;
            break;
    }
}

int usb_drv_port_speed(void)
{
    return (REG_USB_POWER & USB_POWER_HSMODE) ? 1 : 0;
}

void usb_drv_cancel_all_transfers(void)
{
    logf("%s()", __func__);

    unsigned int i, flags = disable_irq_save();

    for(i=0; i<TOTAL_EP(); i++)
    {
        if (endpoints[i].busy)
        {
            if (i & 1)
                usb_core_transfer_complete(i >> 1, USB_DIR_OUT, -1, 0);
            else if (endpoints[i].wait)
                semaphore_release(&endpoints[i].complete);
            else usb_core_transfer_complete(i >> 1, USB_DIR_IN, -1, 0);
        }

        if(i != 1) /* ep0 out needs special handling */
            endpoints[i].buf = NULL;

        endpoints[i].sent = 0;
        endpoints[i].length = 0;

        select_endpoint(i/2);
        flushFIFO(&endpoints[i]);
    }
    restore_irq(flags);
}

void usb_drv_release_endpoint(int ep)
{
    int n = ep & 0x7f;

    logf("%s(%d, %s)", __func__, (ep & 0x7F), (ep >> 7) ? "IN" : "OUT");

    if (n)
    {
        int dir = ep & USB_ENDPOINT_DIR_MASK;

        if(dir == USB_DIR_IN)
        {
            REG_USB_INTRINE &= ~USB_INTR_EP(n);
            endpoints[n << 1].allocated = false;
        }
        else
        {
            REG_USB_INTROUTE &= ~USB_INTR_EP(n);
            endpoints[(n << 1) + 1].allocated = false;
        }
    }
}

int usb_drv_request_endpoint(int type, int dir)
{
    logf("%s(%d, %s)", __func__, type, (dir == USB_DIR_IN) ? "IN" : "OUT");

    dir  &= USB_ENDPOINT_DIR_MASK;
    type &= USB_ENDPOINT_XFERTYPE_MASK;

    /* There are only 3+2 endpoints, so hardcode this ... */
    switch(type)
    {
        case USB_ENDPOINT_XFER_BULK:
            if(dir == USB_DIR_IN)
            {
                if (endpoints[2].allocated)
                    break;
                endpoints[2].allocated = true;
                REG_USB_INTRINE |= USB_INTR_EP(1);
                return (1 | USB_DIR_IN);
            }
            else
            {
                if (endpoints[3].allocated)
                    break;
                endpoints[3].allocated = true;
                REG_USB_INTROUTE |= USB_INTR_EP(1);
                return (1 | USB_DIR_OUT);
            }

        case USB_ENDPOINT_XFER_INT:
            if(dir == USB_DIR_IN)
            {
                if (endpoints[4].allocated)
                    break;
                endpoints[4].allocated = true;
                REG_USB_INTRINE |= USB_INTR_EP(2);
                return (2 | USB_DIR_IN);
            }
            else
            {
                if (endpoints[5].allocated)
                    break;
                endpoints[5].allocated = true;
                REG_USB_INTROUTE |= USB_INTR_EP(2);
                return (2 | USB_DIR_OUT);
            }

        default:
            break;
    }

    return -1;
}
