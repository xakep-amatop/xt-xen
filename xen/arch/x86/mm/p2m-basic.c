/******************************************************************************
 * arch/x86/mm/p2m-basic.c
 *
 * Basic P2M management largely applicable to all domain types.
 *
 * Parts of this code are Copyright (c) 2009 by Citrix Systems, Inc. (Patrick Colp)
 * Parts of this code are Copyright (c) 2007 by Advanced Micro Devices.
 * Parts of this code are Copyright (c) 2006-2007 by XenSource Inc.
 * Parts of this code are Copyright (c) 2006 by Michael A Fetterman
 * Parts based on earlier work by Michael A Fetterman, Ian Pratt et al.
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/types.h>
#include <asm/p2m.h>
#include "mm-locks.h"
#include "p2m.h"

/* Init the datastructures for later use by the p2m code */
static int p2m_initialise(struct domain *d, struct p2m_domain *p2m)
{
    int ret = 0;

#ifdef CONFIG_HVM
    mm_rwlock_init(&p2m->lock);
    INIT_PAGE_LIST_HEAD(&p2m->pages);
    spin_lock_init(&p2m->ioreq.lock);
#endif

    p2m->domain = d;
    p2m->default_access = p2m_access_rwx;
    p2m->p2m_class = p2m_host;

    if ( !is_hvm_domain(d) )
        return 0;

    p2m_pod_init(p2m);
    p2m_nestedp2m_init(p2m);

    if ( hap_enabled(d) && cpu_has_vmx )
        ret = ept_p2m_init(p2m);
    else
        p2m_pt_init(p2m);

    return ret;
}

struct p2m_domain *p2m_init_one(struct domain *d)
{
    struct p2m_domain *p2m = xzalloc(struct p2m_domain);

    if ( !p2m )
        return NULL;

    if ( !zalloc_cpumask_var(&p2m->dirty_cpumask) )
        goto free_p2m;

    if ( p2m_initialise(d, p2m) )
        goto free_cpumask;
    return p2m;

 free_cpumask:
    free_cpumask_var(p2m->dirty_cpumask);
 free_p2m:
    xfree(p2m);
    return NULL;
}

void p2m_free_one(struct p2m_domain *p2m)
{
    p2m_free_logdirty(p2m);
    if ( hap_enabled(p2m->domain) && cpu_has_vmx )
        ept_p2m_uninit(p2m);
    free_cpumask_var(p2m->dirty_cpumask);
    xfree(p2m);
}

static int p2m_init_hostp2m(struct domain *d)
{
    struct p2m_domain *p2m = p2m_init_one(d);
    int rc;

    if ( !p2m )
        return -ENOMEM;

    rc = p2m_init_logdirty(p2m);

    if ( !rc )
        d->arch.p2m = p2m;
    else
        p2m_free_one(p2m);

    return rc;
}

static void p2m_teardown_hostp2m(struct domain *d)
{
    /* Iterate over all p2m tables per domain */
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( p2m )
    {
        p2m_free_one(p2m);
        d->arch.p2m = NULL;
    }
}

int p2m_init(struct domain *d)
{
    int rc;

    rc = p2m_init_hostp2m(d);
    if ( rc || !is_hvm_domain(d) )
        return rc;

    /*
     * Must initialise nestedp2m unconditionally
     * since nestedhvm_enabled(d) returns false here.
     * (p2m_init runs too early for HVM_PARAM_* options)
     */
    rc = p2m_init_nestedp2m(d);
    if ( rc )
    {
        p2m_teardown_hostp2m(d);
        return rc;
    }

    rc = p2m_init_altp2m(d);
    if ( rc )
    {
        p2m_teardown_hostp2m(d);
        p2m_teardown_nestedp2m(d);
    }

    return rc;
}

/*
 * Return all the p2m pages to Xen.
 * We know we don't have any extra mappings to these pages.
 *
 * hvm fixme: when adding support for pvh non-hardware domains, this path must
 * cleanup any foreign p2m types (release refcnts on them).
 */
void p2m_teardown(struct p2m_domain *p2m)
{
#ifdef CONFIG_HVM
    struct page_info *pg;
    struct domain *d;

    if ( !p2m )
        return;

    d = p2m->domain;

    p2m_lock(p2m);

#ifdef CONFIG_MEM_SHARING
    ASSERT(atomic_read(&d->shr_pages) == 0);
#endif

    p2m->phys_table = pagetable_null();

    while ( (pg = page_list_remove_head(&p2m->pages)) )
        d->arch.paging.free_page(d, pg);

    p2m_unlock(p2m);
#endif
}

void p2m_final_teardown(struct domain *d)
{
    if ( is_hvm_domain(d) )
    {
        /*
         * We must tear down both of them unconditionally because
         * we initialise them unconditionally.
         */
        p2m_teardown_altp2m(d);
        p2m_teardown_nestedp2m(d);
    }

    /* Iterate over all p2m tables per domain */
    p2m_teardown_hostp2m(d);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
