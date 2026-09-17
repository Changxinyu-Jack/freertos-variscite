/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_ns.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
#include "fsl_debug_console.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "fsl_lpuart.h"
#include "fsl_irqsteer.h"
#include "fsl_clock.h"
#include "fsl_flexcan.h"
#include "rsc_table.h"
#include "main/imx8qm_pads.h"
#include "svc/pad/pad_api.h"
#include "svc/rm/rm_api.h"
/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define RPMSG_LITE_SHMEM_BASE         (VDEV1_VRING_BASE)
#define RPMSG_LITE_LINK_ID            (RL_PLATFORM_IMX8QM_M4_A_USER_LINK_ID)
#define RPMSG_LITE_NS_ANNOUNCE_STRING "rpmsg-virtual-tty-channel"
#define APP_TASK_STACK_SIZE (256)
#ifndef LOCAL_EPT_ADDR
#define LOCAL_EPT_ADDR (30)
#endif

/* Globals */
static char app_buf[512]; /* Each RPMSG buffer can carry less than 512 payload */

/*
 * CAN0 bring-up probe.
 *
 * Answers two questions that the device tree alone cannot:
 *   1. Has SCFW's resource manager granted CAN0 (SC_R_CAN_0) to this partition?
 *      The "-m4" device tree only makes Linux let go; it says nothing about
 *      whether the SCU side actually hands the resource over.
 *   2. Do the SCU-side pad mux and clock calls the peripheral needs succeed?
 *
 * A negative result is the answer too: if SCFW has not partitioned CAN0 to the
 * M4, everything below fails and no firmware change can work around it.
 */
typedef struct
{
    int32_t owned;    /* sc_rm_is_resource_owned(SC_R_CAN_0): 1 = granted to us */
    int32_t power_on; /* sc_pm_set_resource_power_mode() : 0 = SC_ERR_NONE      */
    int32_t clock;    /* CLOCK_SetIpFreq()            : Hz, 0 = failed          */
    int32_t pad_rx;   /* sc_pad_set_all(SC_P_FLEXCAN0_RX): 0 = SC_ERR_NONE      */
    int32_t pad_tx;   /* sc_pad_set_all(SC_P_FLEXCAN0_TX): 0 = SC_ERR_NONE      */
} can_probe_t;

static can_probe_t g_can = {-1, -1, -1, -1, -1};

static void can_probe(sc_ipc_t ipc)
{
    sc_err_t err;

    g_can.owned = sc_rm_is_resource_owned(ipc, SC_R_CAN_0) ? 1 : 0;

    err          = sc_pm_set_resource_power_mode(ipc, SC_R_CAN_0, SC_PM_PW_MODE_ON);
    g_can.power_on = (int32_t)err;

    g_can.clock = (int32_t)CLOCK_SetIpFreq(kCLOCK_DMA_Can0, SC_80MHZ);

    /*
     * ctrl = 0x21 comes from the board's device tree (flexcan0grp:
     * `fsl,pins = <0x93 0x00 0x21  0x92 0x00 0x21>`), i.e. the value Variscite
     * ships and that CAN actually worked with under Linux. The MCUXpresso
     * example uses 0x40 here; on this board that electrical setting leaves the
     * controller seeing a permanently recessive bus.
     */
    err        = sc_pad_set_all(ipc, SC_P_FLEXCAN0_RX, 0U, SC_PAD_CONFIG_NORMAL, SC_PAD_ISO_OFF, 0x21,
                                SC_PAD_WAKEUP_OFF);
    g_can.pad_rx = (int32_t)err;

    err        = sc_pad_set_all(ipc, SC_P_FLEXCAN0_TX, 0U, SC_PAD_CONFIG_NORMAL, SC_PAD_ISO_OFF, 0x21,
                                SC_PAD_WAKEUP_OFF);
    g_can.pad_tx = (int32_t)err;
}

/*
 * Tiny hand-rolled formatter for the probe reply.
 *
 * Deliberately avoids snprintf: this example links no other newlib stdio call
 * (the console goes through the SDK's own PRINTF), and a failing snprintf
 * returns -1, which as a uint32_t length blows past the RPMsg buffer and trips
 * the assert in the send path. These helpers cannot fail.
 */
static char *app_put_str(char *p, const char *s)
{
    while (*s != '\0')
    {
        *p++ = *s++;
    }
    return p;
}

static char *app_put_i32(char *p, int32_t v)
{
    char     tmp[12];
    int      i = 0;
    uint32_t u;

    if (v < 0)
    {
        *p++ = '-';
        u    = (uint32_t)(-(int64_t)v);
    }
    else
    {
        u = (uint32_t)v;
    }

    do
    {
        tmp[i++] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0U);

    while (i > 0)
    {
        *p++ = tmp[--i];
    }

    return p;
}

static char *app_put_hex2(char *p, uint8_t v)
{
    static const char d[] = "0123456789abcdef";

    *p++ = d[(v >> 4) & 0x0FU];
    *p++ = d[v & 0x0FU];

    return p;
}

/* Fixed-width, so register dumps line up and are easy to compare by eye. */
static char *app_put_hex32f(char *p, uint32_t v)
{
    int i;

    for (i = 7; i >= 0; i--)
    {
        *p++ = "0123456789abcdef"[((v >> (i * 4)) & 0x0FU)];
    }

    return p;
}

static char *app_put_hex(char *p, uint32_t v)
{
    int  i;
    bool started = false;

    for (i = 7; i >= 0; i--)
    {
        uint8_t n = (uint8_t)((v >> (i * 4)) & 0x0FU);

        if ((n != 0U) || started || (i == 0))
        {
            *p++    = "0123456789abcdef"[n];
            started = true;
        }
    }

    return p;
}

/*
 * CAN0 receive -> RPMsg bridge.
 *
 * can_probe() already got CAN0 granted and powered, muxed the pads and set the
 * 80 MHz clock -- that is the SCU-side bring-up. What was still missing is the
 * controller itself: without FLEXCAN_Init() the module sits in reset and no
 * frame ever reaches a mailbox.
 *
 * Frames are moved out of the ISR through a FreeRTOS queue: rpmsg_lite is not
 * ISR-safe, so the send happens in can_task() instead.
 */
#define CAN_RX_MB          (9U)       /* first free MB after the reserved TX MB (see ERR005641) */
#define CAN_BAUDRATE       (500000U)
#define CAN_RX_QUEUE_LEN   (8U)
#define CAN_TASK_STACK_SIZE (256U)

typedef struct
{
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_rx_frame_t;

static flexcan_handle_t      g_can_handle;
static flexcan_frame_t       g_can_frame;    /* filled by the driver in the ISR */
static flexcan_mb_transfer_t g_can_rx_xfer;  /* kept resident so the MB can be re-armed */
static QueueHandle_t         g_can_queue = NULL;
static volatile uint32_t     g_can_rx_count = 0U;

/* Set by app_task once the RPMsg link is up, so can_task() knows where to send. */
static struct rpmsg_lite_instance *g_rpmsg     = NULL;
static struct rpmsg_lite_endpoint *g_ept       = NULL;
static volatile uint32_t           g_peer_addr = 0U;

static void can_rx_callback(CAN_Type *base, flexcan_handle_t *handle, status_t status, uint32_t result,
                            void *userData)
{
    can_rx_frame_t f;
    BaseType_t     woken = pdFALSE;

    if ((status != kStatus_FLEXCAN_RxIdle) || (result != (uint32_t)CAN_RX_MB))
    {
        return;
    }

    /* The driver already copied the mailbox into g_can_frame before calling us. */
    f.id  = (g_can_frame.id & CAN_ID_STD_MASK) >> CAN_ID_STD_SHIFT;
    f.dlc = (uint8_t)g_can_frame.length;
    if (f.dlc > 8U)
    {
        f.dlc = 8U;
    }

    f.data[0] = (uint8_t)g_can_frame.dataByte0;
    f.data[1] = (uint8_t)g_can_frame.dataByte1;
    f.data[2] = (uint8_t)g_can_frame.dataByte2;
    f.data[3] = (uint8_t)g_can_frame.dataByte3;
    f.data[4] = (uint8_t)g_can_frame.dataByte4;
    f.data[5] = (uint8_t)g_can_frame.dataByte5;
    f.data[6] = (uint8_t)g_can_frame.dataByte6;
    f.data[7] = (uint8_t)g_can_frame.dataByte7;

    g_can_rx_count++;

    if (g_can_queue != NULL)
    {
        (void)xQueueSendFromISR(g_can_queue, &f, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void can_rx_arm(void)
{
    g_can_rx_xfer.mbIdx = (uint8_t)CAN_RX_MB;
    g_can_rx_xfer.frame = &g_can_frame;
    (void)FLEXCAN_TransferReceiveNonBlocking(DMA__CAN0, &g_can_handle, &g_can_rx_xfer);
}

/* Caller must have run can_probe() and IRQSTEER_Init() first. */
static void can_init(void)
{
    flexcan_config_t       flexcanConfig;
    flexcan_rx_mb_config_t mbConfig;
    static const IRQn_Type irqsteer_irqs[] = IRQSTEER_IRQS;
    uint32_t               i;

    /*
     * FreeRTOS requires every ISR that calls a "...FromISR" API to run at a
     * priority numerically >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (2),
     * i.e. a register value >= 0x20. IRQSTEER_Init() only calls EnableIRQ() and
     * leaves the priority at its reset default of 0, so can_rx_callback()'s
     * xQueueSendFromISR() tripped configASSERT -- which is
     * taskDISABLE_INTERRUPTS(); for(;;). The core then sat in a loop with
     * interrupts off, the MU was never serviced again, and Linux reported
     * "imx_rproc_kick: failed ... err:-62" (-ETIME).
     *
     * The RPMsg platform layer sets the MU to APP_MU_IRQ_PRIORITY (3) for the
     * same reason; match it here.
     */
    for (i = 0U; i < (sizeof(irqsteer_irqs) / sizeof(irqsteer_irqs[0])); i++)
    {
        NVIC_SetPriority(irqsteer_irqs[i], 3U);
    }

    /* 80 MHz CAN clock / 500 kbps = 160; 160 / (1+7+7+5 quanta) = prescaler 8 */
    FLEXCAN_GetDefaultConfig(&flexcanConfig);
    flexcanConfig.baudRate               = CAN_BAUDRATE;
    flexcanConfig.timingConfig.phaseSeg1 = 6U;
    flexcanConfig.timingConfig.phaseSeg2 = 4U;
    flexcanConfig.timingConfig.propSeg   = 6U;

    FLEXCAN_Init(DMA__CAN0, &flexcanConfig, CLOCK_GetIpFreq(kCLOCK_DMA_Can0));

    FLEXCAN_TransferCreateHandle(DMA__CAN0, &g_can_handle, can_rx_callback, NULL);

    /* All-zero mask: don't compare any ID bit, i.e. accept every standard frame. */
    FLEXCAN_SetRxMbGlobalMask(DMA__CAN0, FLEXCAN_RX_MB_STD_MASK(0U, 0U, 0U));

    mbConfig.format = kFLEXCAN_FrameFormatStandard;
    mbConfig.type   = kFLEXCAN_FrameTypeData;
    mbConfig.id     = FLEXCAN_ID_STD(0U);
    FLEXCAN_SetRxMbConfig(DMA__CAN0, CAN_RX_MB, &mbConfig, true);

    IRQSTEER_EnableInterrupt(IRQSTEER, DMA_FLEXCAN0_INT_IRQn);

    can_rx_arm();
}

static void can_task(void *param)
{
    can_rx_frame_t f;

    for (;;)
    {
        if (xQueueReceive(g_can_queue, &f, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        /* Re-arm the mailbox before doing anything slow. */
        can_rx_arm();

        if ((g_rpmsg == NULL) || (g_ept == NULL) || (g_peer_addr == 0U))
        {
            continue; /* nothing to send to yet */
        }

        {
            uint32_t       size = 0U;
            uint32_t       len;
            char          *p;
            char          *tx;
            uint32_t       i;
            int32_t        rc;

            tx = (char *)rpmsg_lite_alloc_tx_buffer(g_rpmsg, &size, RL_BLOCK);
            if (tx == NULL)
            {
                continue;
            }

            p = app_put_str(tx, "CAN RX #");
            p = app_put_i32(p, (int32_t)g_can_rx_count);
            p = app_put_str(p, " id=0x");
            p = app_put_hex(p, f.id);
            p = app_put_str(p, " dlc=");
            p = app_put_i32(p, (int32_t)f.dlc);
            p = app_put_str(p, " data=");
            for (i = 0U; i < f.dlc; i++)
            {
                if (i != 0U)
                {
                    *p++ = ' ';
                }
                p = app_put_hex2(p, f.data[i]);
            }
            p   = app_put_str(p, "\r\n");
            len = (uint32_t)(p - tx);
            if (len > size)
            {
                len = size;
            }

            rc = rpmsg_lite_send_nocopy(g_rpmsg, g_ept, g_peer_addr, tx, len);
            if (rc != RL_SUCCESS)
            {
                /* Drop it rather than asserting: CAN traffic must not kill the demo. */
                PRINTF("[can] rpmsg send failed: %d\r\n", (int)rc);
            }
        }
    }
}

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

/*******************************************************************************
 * Code
 ******************************************************************************/
static TaskHandle_t app_task_handle = NULL;

static void app_task(void *param)
{
    volatile uint32_t remote_addr;
    struct rpmsg_lite_endpoint *volatile my_ept;
    volatile rpmsg_queue_handle my_queue;
    struct rpmsg_lite_instance *volatile my_rpmsg;
    void *rx_buf;
    uint32_t len;
    int32_t result;
    void *tx_buf;
    uint32_t size;

    /* Print the initial banner */
    PRINTF("\r\nRPMSG String Echo FreeRTOS RTOS API Demo...\r\n");

#ifdef MCMGR_USED
    uint32_t startupData;

    /* Get the startup data */
    (void)MCMGR_GetStartupData(kMCMGR_Core1, &startupData);

    my_rpmsg = rpmsg_lite_remote_init((void *)startupData, RPMSG_LITE_LINK_ID, RL_NO_FLAGS);

    /* Signal the other core we are ready */
    (void)MCMGR_SignalReady(kMCMGR_Core1);
#else
    my_rpmsg = rpmsg_lite_remote_init((void *)RPMSG_LITE_SHMEM_BASE, RPMSG_LITE_LINK_ID, RL_NO_FLAGS);
#endif /* MCMGR_USED */

    while (0 == rpmsg_lite_is_link_up(my_rpmsg))
        ;

    my_queue = rpmsg_queue_create(my_rpmsg);
    my_ept   = rpmsg_lite_create_ept(my_rpmsg, LOCAL_EPT_ADDR, rpmsg_queue_rx_cb, my_queue);
    (void)rpmsg_ns_announce(my_rpmsg, my_ept, RPMSG_LITE_NS_ANNOUNCE_STRING, RL_NS_CREATE);

    /* Publish the link so can_task() can push frames to the host. */
    g_rpmsg = my_rpmsg;
    g_ept   = my_ept;

    PRINTF("\r\nNameservice sent, ready for incoming messages...\r\n");

    for (;;)
    {
        /* Get RPMsg rx buffer with message */
        result =
            rpmsg_queue_recv_nocopy(my_rpmsg, my_queue, (uint32_t *)&remote_addr, (char **)&rx_buf, &len, RL_BLOCK);
        if (result != 0)
        {
            assert(false);
        }

        /* Remember where the host is so can_task() has a destination. */
        g_peer_addr = remote_addr;

        /* Copy string from RPMsg rx buffer */
        assert(len < sizeof(app_buf));
        memcpy(app_buf, rx_buf, len);
        app_buf[len] = 0; /* End string by '\0' */

        if ((len == 2) && (app_buf[0] == 0xd) && (app_buf[1] == 0xa))
            PRINTF("Get New Line From Master Side\r\n");
        else
            PRINTF("Get Message From Master Side : \"%s\" [len : %d]\r\n", app_buf, len);

        /* Get tx buffer from RPMsg */
        tx_buf = rpmsg_lite_alloc_tx_buffer(my_rpmsg, &size, RL_BLOCK);
        assert(tx_buf);

        /* Host sent "can": answer with the live CAN0 status instead of echoing. */
        if ((len >= 3) && (app_buf[0] == 'c') && (app_buf[1] == 'a') && (app_buf[2] == 'n'))
        {
            char *p = (char *)tx_buf;

            p = app_put_str(p, "CAN0 STATUS\r\n  owned=");
            p = app_put_i32(p, g_can.owned);
            p = app_put_str(p, " power_on=");
            p = app_put_i32(p, g_can.power_on);
            p = app_put_str(p, " clock=");
            p = app_put_i32(p, (int32_t)CLOCK_GetIpFreq(kCLOCK_DMA_Can0));
            p = app_put_str(p, "\r\n  pad_rx=");
            p = app_put_i32(p, g_can.pad_rx);
            p = app_put_str(p, " pad_tx=");
            p = app_put_i32(p, g_can.pad_tx);

            p = app_put_str(p, "\r\n  MCR=0x");
            p = app_put_hex32f(p, (uint32_t)DMA__CAN0->MCR);
            p = app_put_str(p, " CTRL1=0x");
            p = app_put_hex32f(p, (uint32_t)DMA__CAN0->CTRL1);
            p = app_put_str(p, "\r\n  ESR1=0x");
            p = app_put_hex32f(p, (uint32_t)DMA__CAN0->ESR1);
            p = app_put_str(p, " IFLAG1=0x");
            p = app_put_hex32f(p, (uint32_t)DMA__CAN0->IFLAG1);
            p = app_put_str(p, "\r\n  rx_count=");
            p = app_put_i32(p, (int32_t)g_can_rx_count);
            p = app_put_str(p, " pending_in_queue=");
            p = app_put_i32(p, (g_can_queue != NULL) ? (int32_t)uxQueueMessagesWaiting(g_can_queue) : -1);
            p = app_put_str(p, "\r\n");

            len = (uint32_t)(p - (char *)tx_buf);

            /* Never hand the send path more than the buffer can hold. */
            if (len > size)
            {
                len = size;
            }
        }
        else
        {
            /* Copy string to RPMsg tx buffer */
            memcpy(tx_buf, app_buf, len);
        }

        /* Send back with nocopy send */
        result = rpmsg_lite_send_nocopy(my_rpmsg, my_ept, remote_addr, tx_buf, len);
        if (result != 0)
        {
            assert(false);
        }
        /* Release held RPMsg rx buffer */
        result = rpmsg_queue_nocopy_free(my_rpmsg, rx_buf);
        if (result != 0)
        {
            assert(false);
        }
    }
}

/*!
 * @brief Main function
 */
int main(void)
{
    /* Initialize standard SDK demo application pins */
    sc_ipc_t ipc;
    ipc = BOARD_InitRpc();

    BOARD_InitPins(ipc);
    BOARD_BootClockRUN();
    BOARD_InitDebugConsole();
    BOARD_InitMemory();

    /* Power up the MU used for RPMSG */
    if (sc_pm_set_resource_power_mode(ipc, SC_R_MU_5B, SC_PM_PW_MODE_ON) != SC_ERR_NONE)
    {
        PRINTF("Error: Failed to power on MU!\r\n");
    }
    if (sc_pm_set_resource_power_mode(ipc, SC_R_MU_7A, SC_PM_PW_MODE_ON) != SC_ERR_NONE)
    {
        PRINTF("Error: Failed to power on MU!\r\n");
    }

    if (sc_pm_set_resource_power_mode(ipc, SC_R_IRQSTR_M4_0, SC_PM_PW_MODE_ON) != SC_ERR_NONE)
    {
        PRINTF("Error: Failed to power on IRQSTEER!\r\n");
    }

    IRQSTEER_Init(IRQSTEER);

    can_probe(ipc);
    PRINTF("[can_probe] owned=%d power_on=%d clock=%d pad_rx=%d pad_tx=%d\r\n", (int)g_can.owned,
           (int)g_can.power_on, (int)g_can.clock, (int)g_can.pad_rx, (int)g_can.pad_tx);

    /* The queue has to exist before can_init() arms the Rx mailbox. */
    g_can_queue = xQueueCreate(CAN_RX_QUEUE_LEN, sizeof(can_rx_frame_t));
    if (g_can_queue == NULL)
    {
        PRINTF("Failed to create CAN rx queue\r\n");
    }
    else
    {
        can_init();
        PRINTF("[can] FLEXCAN0 up at %d bps\r\n", (int)CAN_BAUDRATE);
    }

    copyResourceTable();

#ifdef MCMGR_USED
    /* Initialize MCMGR before calling its API */
    (void)MCMGR_Init();
#endif /* MCMGR_USED */

    if (xTaskCreate(app_task, "APP_TASK", APP_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, &app_task_handle) != pdPASS)
    {
        PRINTF("\r\nFailed to create application task\r\n");
        for (;;)
            ;
    }

    if (g_can_queue != NULL)
    {
        if (xTaskCreate(can_task, "CAN_TASK", CAN_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
        {
            PRINTF("\r\nFailed to create CAN task\r\n");
        }
    }

    vTaskStartScheduler();

    PRINTF("Failed to start FreeRTOS on core0.\n");
    for (;;)
        ;
}
