/*
 * xen/arch/arm/vgic.c
 *
 * ARM Virtual Generic Interrupt Controller support
 *
 * Ian Campbell <ian.campbell@citrix.com>
 * Copyright (c) 2011 Citrix Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/bitops.h>
#include <xen/config.h>
#include <xen/lib.h>
#include <xen/init.h>
#include <xen/softirq.h>
#include <xen/irq.h>
#include <xen/sched.h>

#include <asm/current.h>

#include <asm/mmio.h>
#include <asm/gic.h>
#include <asm/vgic.h>

static int vgic_distr_mmio_read(struct vcpu *v, mmio_info_t *info);
static int vgic_distr_mmio_write(struct vcpu *v, mmio_info_t *info);

/*
 * Returns rank corresponding to a GICD_<FOO><n> register for
 * GICD_<FOO> with <b>-bits-per-interrupt.
 */
static struct vgic_irq_rank *vgic_rank_offset(struct vcpu *v, int b, int n,
                                              int s)
{
    int rank = REG_RANK_NR(b, (n >> s));

    if ( rank == 0 )
        return v->arch.vgic.private_irqs;
    else if ( rank <= DOMAIN_NR_RANKS(v->domain) )
        return &v->domain->arch.vgic.shared_irqs[rank - 1];
    else
        return NULL;
}

static struct vgic_irq_rank *vgic_rank_irq(struct vcpu *v, unsigned int irq)
{
    return vgic_rank_offset(v, 8, irq, DABT_WORD);
}

static const struct mmio_handler_ops vgic_distr_mmio_handler = {
    .read_handler  = vgic_distr_mmio_read,
    .write_handler = vgic_distr_mmio_write,
};

int domain_vgic_init(struct domain *d)
{
    int i;

    d->arch.vgic.ctlr = 0;

    /* Currently nr_lines in vgic and gic doesn't have the same meanings
     * Here nr_lines = number of SPIs
     */
    if ( is_hardware_domain(d) )
        d->arch.vgic.nr_lines = gic_number_lines() - 32;
    else
        d->arch.vgic.nr_lines = 0; /* We don't need SPIs for the guest */

    d->arch.vgic.shared_irqs =
        xzalloc_array(struct vgic_irq_rank, DOMAIN_NR_RANKS(d));
    if ( d->arch.vgic.shared_irqs == NULL )
        return -ENOMEM;

    d->arch.vgic.pending_irqs =
        xzalloc_array(struct pending_irq, d->arch.vgic.nr_lines);
    if ( d->arch.vgic.pending_irqs == NULL )
    {
        xfree(d->arch.vgic.shared_irqs);
        return -ENOMEM;
    }

    for (i=0; i<d->arch.vgic.nr_lines; i++)
    {
        INIT_LIST_HEAD(&d->arch.vgic.pending_irqs[i].inflight);
        INIT_LIST_HEAD(&d->arch.vgic.pending_irqs[i].lr_queue);
    }
    for (i=0; i<DOMAIN_NR_RANKS(d); i++)
        spin_lock_init(&d->arch.vgic.shared_irqs[i].lock);

    /*
     * We rely on gicv_setup() to initialize dbase(vGIC distributor base)
     */
    register_mmio_handler(d, &vgic_distr_mmio_handler,
                          d->arch.vgic.dbase, PAGE_SIZE);

    return 0;
}

void domain_vgic_free(struct domain *d)
{
    xfree(d->arch.vgic.shared_irqs);
    xfree(d->arch.vgic.pending_irqs);
}

int vcpu_vgic_init(struct vcpu *v)
{
    int i;

    v->arch.vgic.private_irqs = xzalloc(struct vgic_irq_rank);
    if ( v->arch.vgic.private_irqs == NULL )
      return -ENOMEM;

    spin_lock_init(&v->arch.vgic.private_irqs->lock);

    memset(&v->arch.vgic.pending_irqs, 0, sizeof(v->arch.vgic.pending_irqs));
    for (i = 0; i < 32; i++)
    {
        INIT_LIST_HEAD(&v->arch.vgic.pending_irqs[i].inflight);
        INIT_LIST_HEAD(&v->arch.vgic.pending_irqs[i].lr_queue);
    }

    /* For SGI and PPI the target is always this CPU */
    for ( i = 0 ; i < 8 ; i++ )
        v->arch.vgic.private_irqs->itargets[i] =
              (1<<(v->vcpu_id+0))
            | (1<<(v->vcpu_id+8))
            | (1<<(v->vcpu_id+16))
            | (1<<(v->vcpu_id+24));
    INIT_LIST_HEAD(&v->arch.vgic.inflight_irqs);
    INIT_LIST_HEAD(&v->arch.vgic.lr_pending);
    spin_lock_init(&v->arch.vgic.lock);

    return 0;
}

int vcpu_vgic_free(struct vcpu *v)
{
    xfree(v->arch.vgic.private_irqs);
    return 0;
}

static int vgic_distr_mmio_read(struct vcpu *v, mmio_info_t *info)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;
    int gicd_reg = (int)(info->gpa - v->domain->arch.vgic.dbase);

    switch ( gicd_reg )
    {
    case GICD_CTLR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        vgic_lock(v);
        *r = v->domain->arch.vgic.ctlr;
        vgic_unlock(v);
        return 1;
    case GICD_TYPER:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /* No secure world support for guests. */
        vgic_lock(v);
        *r = ( (v->domain->max_vcpus<<5) & GICD_TYPE_CPUS )
            |( ((v->domain->arch.vgic.nr_lines/32)) & GICD_TYPE_LINES );
        vgic_unlock(v);
        return 1;
    case GICD_IIDR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /*
         * XXX Do we need a JEP106 manufacturer ID?
         * Just use the physical h/w value for now
         */
        *r = 0x0000043b;
        return 1;

    /* Implementation defined -- read as zero */
    case 0x020 ... 0x03c:
        goto read_as_zero;

    case GICD_IGROUPR ... GICD_IGROUPRN:
        /* We do not implement security extensions for guests, read zero */
        goto read_as_zero;

    case GICD_ISENABLER ... GICD_ISENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISENABLER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->ienable;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICENABLER ... GICD_ICENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICENABLER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->ienable;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ISPENDR ... GICD_ISPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISPENDR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->ipend, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICPENDR ... GICD_ICPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICPENDR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->ipend, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ISACTIVER ... GICD_ISACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISACTIVER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->iactive;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICACTIVER ... GICD_ICACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICACTIVER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->iactive;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ITARGETSR ... GICD_ITARGETSRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_ITARGETSR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;

        vgic_lock_rank(v, rank);
        *r = rank->itargets[REG_RANK_INDEX(8, gicd_reg - GICD_ITARGETSR,
                                           DABT_WORD)];
        if ( dabt.size == DABT_BYTE )
            *r = vgic_byte_read(*r, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
        if ( dabt.size != 0 && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_IPRIORITYR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;

        vgic_lock_rank(v, rank);
        *r = rank->ipriority[REG_RANK_INDEX(8, gicd_reg - GICD_IPRIORITYR,
                                            DABT_WORD)];
        if ( dabt.size == DABT_BYTE )
            *r = vgic_byte_read(*r, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICFGR ... GICD_ICFGRN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 2, gicd_reg - GICD_ICFGR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->icfg[REG_RANK_INDEX(2, gicd_reg - GICD_ICFGR, DABT_WORD)];
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_NSACR ... GICD_NSACRN:
        /* We do not implement security extensions for guests, read zero */
        goto read_as_zero;

    case GICD_SGIR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /* Write only -- read unknown */
        *r = 0xdeadbeef;
        return 1;

    case GICD_CPENDSGIR ... GICD_CPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_CPENDSGIR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->pendsgi, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_SPENDSGIR ... GICD_SPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_SPENDSGIR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->pendsgi, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    /* Implementation defined -- read as zero */
    case 0xfd0 ... 0xfe4:
        goto read_as_zero;

    case GICD_ICPIDR2:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled read from ICPIDR2\n");
        return 0;

    /* Implementation defined -- read as zero */
    case 0xfec ... 0xffc:
        goto read_as_zero;

    /* Reserved -- read as zero */
    case 0x00c ... 0x01c:
    case 0x040 ... 0x07c:
    case 0x7fc:
    case 0xbfc:
    case 0xf04 ... 0xf0c:
    case 0xf30 ... 0xfcc:
        goto read_as_zero;

    default:
        printk("vGICD: unhandled read r%d offset %#08x\n",
               dabt.reg, gicd_reg);
        return 0;
    }

bad_width:
    printk("vGICD: bad read width %d r%d offset %#08x\n",
           dabt.size, dabt.reg, gicd_reg);
    domain_crash_synchronous();
    return 0;

read_as_zero:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    *r = 0;
    return 1;
}

static void vgic_disable_irqs(struct vcpu *v, uint32_t r, int n)
{
    const unsigned long mask = r;
    struct pending_irq *p;
    unsigned int irq;
    unsigned long flags;
    int i = 0;

    while ( (i = find_next_bit(&mask, 32, i)) < 32 ) {
        irq = i + (32 * n);
        p = irq_to_pending(v, irq);
        clear_bit(GIC_IRQ_GUEST_ENABLED, &p->status);
        gic_remove_from_queues(v, irq);
        if ( p->desc != NULL )
        {
            spin_lock_irqsave(&p->desc->lock, flags);
            p->desc->handler->disable(p->desc);
            spin_unlock_irqrestore(&p->desc->lock, flags);
        }
        i++;
    }
}

static void vgic_enable_irqs(struct vcpu *v, uint32_t r, int n)
{
    const unsigned long mask = r;
    struct pending_irq *p;
    unsigned int irq;
    unsigned long flags;
    int i = 0;

    while ( (i = find_next_bit(&mask, 32, i)) < 32 ) {
        irq = i + (32 * n);
        p = irq_to_pending(v, irq);
        set_bit(GIC_IRQ_GUEST_ENABLED, &p->status);
        /* We need to force the first injection of evtchn_irq because
         * evtchn_upcall_pending is already set by common code on vcpu
         * creation. */
        if ( irq == v->domain->arch.evtchn_irq &&
             vcpu_info(current, evtchn_upcall_pending) &&
             list_empty(&p->inflight) )
            vgic_vcpu_inject_irq(v, irq);
        else {
            unsigned long flags;
            spin_lock_irqsave(&v->arch.vgic.lock, flags);
            if ( !list_empty(&p->inflight) && !test_bit(GIC_IRQ_GUEST_VISIBLE, &p->status) )
                gic_raise_guest_irq(v, irq, p->priority);
            spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
        }
        if ( p->desc != NULL )
        {
            spin_lock_irqsave(&p->desc->lock, flags);
            p->desc->handler->enable(p->desc);
            spin_unlock_irqrestore(&p->desc->lock, flags);
        }
        i++;
    }
}

static int vgic_to_sgi(struct vcpu *v, register_t sgir)
{
    struct domain *d = v->domain;
    int virtual_irq;
    int filter;
    int vcpuid;
    int i;
    unsigned long vcpu_mask = 0;

    ASSERT(d->max_vcpus < 8*sizeof(vcpu_mask));

    filter = (sgir & GICD_SGI_TARGET_LIST_MASK);
    virtual_irq = (sgir & GICD_SGI_INTID_MASK);
    ASSERT( virtual_irq < 16 );

    switch ( filter )
    {
        case GICD_SGI_TARGET_LIST:
            vcpu_mask = (sgir & GICD_SGI_TARGET_MASK) >> GICD_SGI_TARGET_SHIFT;
            break;
        case GICD_SGI_TARGET_OTHERS:
            for ( i = 0; i < d->max_vcpus; i++ )
            {
                if ( i != current->vcpu_id && d->vcpu[i] != NULL &&
                     is_vcpu_online(d->vcpu[i]) )
                    set_bit(i, &vcpu_mask);
            }
            break;
        case GICD_SGI_TARGET_SELF:
            set_bit(current->vcpu_id, &vcpu_mask);
            break;
        default:
            gdprintk(XENLOG_WARNING, "vGICD: unhandled GICD_SGIR write %"PRIregister" with wrong TargetListFilter field\n",
                     sgir);
            return 0;
    }

    for_each_set_bit( vcpuid, &vcpu_mask, d->max_vcpus )
    {
        if ( d->vcpu[vcpuid] != NULL && !is_vcpu_online(d->vcpu[vcpuid]) )
        {
            gdprintk(XENLOG_WARNING, "vGICD: GICD_SGIR write r=%"PRIregister" vcpu_mask=%lx, wrong CPUTargetList\n",
                     sgir, vcpu_mask);
            continue;
        }
        vgic_vcpu_inject_irq(d->vcpu[vcpuid], virtual_irq);
    }
    return 1;
}

static int vgic_distr_mmio_write(struct vcpu *v, mmio_info_t *info)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;
    int gicd_reg = (int)(info->gpa - v->domain->arch.vgic.dbase);
    uint32_t tr;

    switch ( gicd_reg )
    {
    case GICD_CTLR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /* Ignore all but the enable bit */
        v->domain->arch.vgic.ctlr = (*r) & GICD_CTL_ENABLE;
        return 1;

    /* R/O -- write ignored */
    case GICD_TYPER:
    case GICD_IIDR:
        goto write_ignore;

    /* Implementation defined -- write ignored */
    case 0x020 ... 0x03c:
        goto write_ignore;

    case GICD_IGROUPR ... GICD_IGROUPRN:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;

    case GICD_ISENABLER ... GICD_ISENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISENABLER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        tr = rank->ienable;
        rank->ienable |= *r;
        vgic_unlock_rank(v, rank);
        vgic_enable_irqs(v, (*r) & (~tr),
                         (gicd_reg - GICD_ISENABLER) >> DABT_WORD);
        return 1;

    case GICD_ICENABLER ... GICD_ICENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICENABLER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        tr = rank->ienable;
        rank->ienable &= ~*r;
        vgic_unlock_rank(v, rank);
        vgic_disable_irqs(v, (*r) & tr,
                          (gicd_reg - GICD_ICENABLER) >> DABT_WORD);
        return 1;

    case GICD_ISPENDR ... GICD_ISPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ISPENDR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_ISPENDR);
        return 0;

    case GICD_ICPENDR ... GICD_ICPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ICPENDR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_ICPENDR);
        return 0;

    case GICD_ISACTIVER ... GICD_ISACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISACTIVER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->iactive &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICACTIVER ... GICD_ICACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICACTIVER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->iactive &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ITARGETSR ... GICD_ITARGETSR + 7:
        /* SGI/PPI target is read only */
        goto write_ignore;

    case GICD_ITARGETSR + 8 ... GICD_ITARGETSRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_ITARGETSR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        if ( dabt.size == DABT_WORD )
            rank->itargets[REG_RANK_INDEX(8, gicd_reg - GICD_ITARGETSR,
                                          DABT_WORD)] = *r;
        else
        {
            tr = REG_RANK_INDEX(8, gicd_reg - GICD_ITARGETSR, DABT_WORD);
            vgic_byte_write(&rank->itargets[tr], *r, gicd_reg);
        }
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_IPRIORITYR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        if ( dabt.size == DABT_WORD )
            rank->ipriority[REG_RANK_INDEX(8, gicd_reg - GICD_IPRIORITYR,
                                           DABT_WORD)] = *r;
        else
        {
            tr = REG_RANK_INDEX(8, gicd_reg - GICD_IPRIORITYR, DABT_WORD);
            vgic_byte_write(&rank->ipriority[tr], *r, gicd_reg);
        }
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICFGR: /* SGIs */
        goto write_ignore;
    case GICD_ICFGR + 1: /* PPIs */
        /* It is implementation defined if these are writeable. We chose not */
        goto write_ignore;
    case GICD_ICFGR + 2 ... GICD_ICFGRN: /* SPIs */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 2, gicd_reg - GICD_ICFGR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->icfg[REG_RANK_INDEX(2, gicd_reg - GICD_ICFGR, DABT_WORD)] = *r;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_NSACR ... GICD_NSACRN:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;

    case GICD_SGIR:
        if ( dabt.size != 2 )
            goto bad_width;
        return vgic_to_sgi(v, *r);

    case GICD_CPENDSGIR ... GICD_CPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ICPENDSGIR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_CPENDSGIR);
        return 0;

    case GICD_SPENDSGIR ... GICD_SPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ISPENDSGIR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_SPENDSGIR);
        return 0;

    /* Implementation defined -- write ignored */
    case 0xfd0 ... 0xfe4:
        goto write_ignore;

    /* R/O -- write ignore */
    case GICD_ICPIDR2:
        goto write_ignore;

    /* Implementation defined -- write ignored */
    case 0xfec ... 0xffc:
        goto write_ignore;

    /* Reserved -- write ignored */
    case 0x00c ... 0x01c:
    case 0x040 ... 0x07c:
    case 0x7fc:
    case 0xbfc:
    case 0xf04 ... 0xf0c:
    case 0xf30 ... 0xfcc:
        goto write_ignore;

    default:
        printk("vGICD: unhandled write r%d=%"PRIregister" offset %#08x\n",
               dabt.reg, *r, gicd_reg);
        return 0;
    }

bad_width:
    printk("vGICD: bad write width %d r%d=%"PRIregister" offset %#08x\n",
           dabt.size, dabt.reg, *r, gicd_reg);
    domain_crash_synchronous();
    return 0;

write_ignore:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    return 1;
}

struct pending_irq *irq_to_pending(struct vcpu *v, unsigned int irq)
{
    struct pending_irq *n;
    /* Pending irqs allocation strategy: the first vgic.nr_lines irqs
     * are used for SPIs; the rests are used for per cpu irqs */
    if ( irq < 32 )
        n = &v->arch.vgic.pending_irqs[irq];
    else
        n = &v->domain->arch.vgic.pending_irqs[irq - 32];
    return n;
}

void vgic_clear_pending_irqs(struct vcpu *v)
{
    struct pending_irq *p, *t;
    unsigned long flags;

    spin_lock_irqsave(&v->arch.vgic.lock, flags);
    list_for_each_entry_safe ( p, t, &v->arch.vgic.inflight_irqs, inflight )
        list_del_init(&p->inflight);
    gic_clear_pending_irqs(v);
    spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
}

void vgic_vcpu_inject_irq(struct vcpu *v, unsigned int irq)
{
    uint8_t priority;
    struct vgic_irq_rank *rank = vgic_rank_irq(v, irq);
    struct pending_irq *iter, *n = irq_to_pending(v, irq);
    unsigned long flags;
    bool_t running;

    spin_lock_irqsave(&v->arch.vgic.lock, flags);

    if ( !list_empty(&n->inflight) )
    {
        set_bit(GIC_IRQ_GUEST_QUEUED, &n->status);
        gic_raise_inflight_irq(v, irq);
        goto out;
    }

    /* vcpu offline */
    if ( test_bit(_VPF_down, &v->pause_flags) )
    {
        spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
        return;
    }

    priority = vgic_byte_read(rank->ipriority[REG_RANK_INDEX(8, irq, DABT_WORD)], 0, irq & 0x3);

    n->irq = irq;
    set_bit(GIC_IRQ_GUEST_QUEUED, &n->status);
    n->priority = priority;

    /* the irq is enabled */
    if ( test_bit(GIC_IRQ_GUEST_ENABLED, &n->status) )
        gic_raise_guest_irq(v, irq, priority);

    list_for_each_entry ( iter, &v->arch.vgic.inflight_irqs, inflight )
    {
        if ( iter->priority > priority )
        {
            list_add_tail(&n->inflight, &iter->inflight);
            goto out;
        }
    }
    list_add_tail(&n->inflight, &v->arch.vgic.inflight_irqs);
out:
    spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
    /* we have a new higher priority irq, inject it into the guest */
    running = v->is_running;
    vcpu_unblock(v);
    if ( running && v != current )
        smp_send_event_check_mask(cpumask_of(v->processor));
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */

