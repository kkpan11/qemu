/*
 * QEMU model of the Xilinx Ethernet Lite MAC.
 *
 * Copyright (c) 2009 Edgar E. Iglesias.
 *
 * DS580: https://docs.amd.com/v/u/en-US/xps_ethernetlite
 * LogiCORE IP XPS Ethernet Lite Media Access Controller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "exec/tswap.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "net/net.h"
#include "trace.h"

#define R_TX_BUF0     0
#define BUFSZ_MAX      0x07e4
#define A_MDIO_BASE    0x07e4
#define R_TX_LEN0     (0x07f4 / 4)
#define R_TX_GIE0     (0x07f8 / 4)
#define R_TX_CTRL0    (0x07fc / 4)
#define R_TX_BUF1     (0x0800 / 4)
#define R_TX_LEN1     (0x0ff4 / 4)
#define R_TX_CTRL1    (0x0ffc / 4)

#define R_RX_BUF0     (0x1000 / 4)
#define R_RX_CTRL0    (0x17fc / 4)
#define R_RX_BUF1     (0x1800 / 4)
#define R_RX_CTRL1    (0x1ffc / 4)
#define R_MAX         (0x2000 / 4)

#define GIE_GIE    0x80000000

#define CTRL_I     0x8
#define CTRL_P     0x2
#define CTRL_S     0x1

typedef struct XlnxXpsEthLitePort {
    struct {
        uint32_t tx_gie;

        uint32_t rx_ctrl;
    } reg;
} XlnxXpsEthLitePort;

#define TYPE_XILINX_ETHLITE "xlnx.xps-ethernetlite"
OBJECT_DECLARE_SIMPLE_TYPE(XlnxXpsEthLite, XILINX_ETHLITE)

struct XlnxXpsEthLite
{
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;

    uint32_t c_tx_pingpong;
    uint32_t c_rx_pingpong;
    unsigned int port_index; /* dual port RAM index */

    UnimplementedDeviceState mdio;
    XlnxXpsEthLitePort port[2];
    uint32_t regs[R_MAX];
};

static inline void eth_pulse_irq(XlnxXpsEthLite *s)
{
    /* Only the first gie reg is active.  */
    if (s->port[0].reg.tx_gie & GIE_GIE) {
        qemu_irq_pulse(s->irq);
    }
}

static unsigned addr_to_port_index(hwaddr addr)
{
    return extract64(addr, 11, 1);
}

static void *txbuf_ptr(XlnxXpsEthLite *s, unsigned port_index)
{
    unsigned int rxbase = port_index * (0x800 / 4);

    return &s->regs[rxbase + R_TX_BUF0];
}

static void *rxbuf_ptr(XlnxXpsEthLite *s, unsigned port_index)
{
    unsigned int rxbase = port_index * (0x800 / 4);

    return &s->regs[rxbase + R_RX_BUF0];
}

static uint64_t
eth_read(void *opaque, hwaddr addr, unsigned int size)
{
    XlnxXpsEthLite *s = opaque;
    unsigned port_index = addr_to_port_index(addr);
    uint32_t r = 0;

    addr >>= 2;

    switch (addr)
    {
        case R_TX_GIE0:
            r = s->port[port_index].reg.tx_gie;
            break;

        case R_TX_LEN0:
        case R_TX_LEN1:
        case R_TX_CTRL1:
        case R_TX_CTRL0:
            r = s->regs[addr];
            break;

        case R_RX_CTRL1:
        case R_RX_CTRL0:
            r = s->port[port_index].reg.rx_ctrl;
            break;

        default:
            r = tswap32(s->regs[addr]);
            break;
    }
    return r;
}

static void
eth_write(void *opaque, hwaddr addr,
          uint64_t val64, unsigned int size)
{
    XlnxXpsEthLite *s = opaque;
    unsigned int port_index = addr_to_port_index(addr);
    unsigned int base = 0;
    uint32_t value = val64;

    addr >>= 2;
    switch (addr) 
    {
        case R_TX_CTRL0:
        case R_TX_CTRL1:
            if (addr == R_TX_CTRL1)
                base = 0x800 / 4;

            if ((value & (CTRL_P | CTRL_S)) == CTRL_S) {
                qemu_send_packet(qemu_get_queue(s->nic),
                                 txbuf_ptr(s, port_index),
                                 s->regs[base + R_TX_LEN0]);
                if (s->regs[base + R_TX_CTRL0] & CTRL_I)
                    eth_pulse_irq(s);
            } else if ((value & (CTRL_P | CTRL_S)) == (CTRL_P | CTRL_S)) {
                memcpy(&s->conf.macaddr.a[0], txbuf_ptr(s, port_index), 6);
                if (s->regs[base + R_TX_CTRL0] & CTRL_I)
                    eth_pulse_irq(s);
            }

            /* We are fast and get ready pretty much immediately so
               we actually never flip the S nor P bits to one.  */
            s->regs[addr] = value & ~(CTRL_P | CTRL_S);
            break;

        /* Keep these native.  */
        case R_RX_CTRL0:
        case R_RX_CTRL1:
            if (!(value & CTRL_S)) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
            s->port[port_index].reg.rx_ctrl = value;
            break;

        case R_TX_LEN0:
        case R_TX_LEN1:
            s->regs[addr] = value;
            break;

        case R_TX_GIE0:
            s->port[port_index].reg.tx_gie = value;
            break;

        default:
            s->regs[addr] = tswap32(value);
            break;
    }
}

static const MemoryRegionOps eth_ops = {
    .read = eth_read,
    .write = eth_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4
    }
};

static bool eth_can_rx(NetClientState *nc)
{
    XlnxXpsEthLite *s = qemu_get_nic_opaque(nc);

    return !(s->port[s->port_index].reg.rx_ctrl & CTRL_S);
}

static ssize_t eth_rx(NetClientState *nc, const uint8_t *buf, size_t size)
{
    XlnxXpsEthLite *s = qemu_get_nic_opaque(nc);
    unsigned int port_index = s->port_index;

    /* DA filter.  */
    if (!(buf[0] & 0x80) && memcmp(&s->conf.macaddr.a[0], buf, 6))
        return size;

    if (s->port[port_index].reg.rx_ctrl & CTRL_S) {
        trace_ethlite_pkt_lost(s->port[port_index].reg.rx_ctrl);
        return -1;
    }

    if (size >= BUFSZ_MAX) {
        trace_ethlite_pkt_size_too_big(size);
        return -1;
    }
    memcpy(rxbuf_ptr(s, port_index), buf, size);

    s->port[port_index].reg.rx_ctrl |= CTRL_S;
    if (s->port[port_index].reg.rx_ctrl & CTRL_I) {
        eth_pulse_irq(s);
    }

    /* If c_rx_pingpong was set flip buffers.  */
    s->port_index ^= s->c_rx_pingpong;
    return size;
}

static void xilinx_ethlite_reset(DeviceState *dev)
{
    XlnxXpsEthLite *s = XILINX_ETHLITE(dev);

    s->port_index = 0;
}

static NetClientInfo net_xilinx_ethlite_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = eth_can_rx,
    .receive = eth_rx,
};

static void xilinx_ethlite_realize(DeviceState *dev, Error **errp)
{
    XlnxXpsEthLite *s = XILINX_ETHLITE(dev);

    object_initialize_child(OBJECT(dev), "ethlite.mdio", &s->mdio,
                            TYPE_UNIMPLEMENTED_DEVICE);
    qdev_prop_set_string(DEVICE(&s->mdio), "name", "ethlite.mdio");
    qdev_prop_set_uint64(DEVICE(&s->mdio), "size", 4 * 4);
    sysbus_realize(SYS_BUS_DEVICE(&s->mdio), &error_fatal);
    memory_region_add_subregion(&s->mmio, A_MDIO_BASE,
                           sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->mdio), 0));

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_xilinx_ethlite_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void xilinx_ethlite_init(Object *obj)
{
    XlnxXpsEthLite *s = XILINX_ETHLITE(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    memory_region_init_io(&s->mmio, obj, &eth_ops, s,
                          "xlnx.xps-ethernetlite", R_MAX * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const Property xilinx_ethlite_properties[] = {
    DEFINE_PROP_UINT32("tx-ping-pong", XlnxXpsEthLite, c_tx_pingpong, 1),
    DEFINE_PROP_UINT32("rx-ping-pong", XlnxXpsEthLite, c_rx_pingpong, 1),
    DEFINE_NIC_PROPERTIES(XlnxXpsEthLite, conf),
};

static void xilinx_ethlite_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = xilinx_ethlite_realize;
    device_class_set_legacy_reset(dc, xilinx_ethlite_reset);
    device_class_set_props(dc, xilinx_ethlite_properties);
}

static const TypeInfo xilinx_ethlite_types[] = {
    {
        .name          = TYPE_XILINX_ETHLITE,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(XlnxXpsEthLite),
        .instance_init = xilinx_ethlite_init,
        .class_init    = xilinx_ethlite_class_init,
    },
};

DEFINE_TYPES(xilinx_ethlite_types)
