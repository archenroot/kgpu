/*
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the GPL-COPYING file in the top-level directory.
 *
 * Copyright (c) 2010-2011 University of Utah and the Flux Group.
 * All rights reserved.
 */
#include <linux/raid/pq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/completion.h>
#include "../../../kgpu/kgpu.h"
#include "../gpq.h"


struct gpq_async_data {
    struct completion *c;
    int disks;
    size_t dsize;
    void **dps;
    struct kgpu_buffer *buf;
};

/* customized log function */
#define gpq_log(level, ...) kgpu_do_log(level, "gpq", ##__VA_ARGS__)
#define dbg(...) gpq_log(KGPU_LOG_DEBUG, ##__VA_ARGS__)

static int replace_global = 0;
static int use_hybrid = 0;

module_param(replace_global, int, 0);
MODULE_PARM_DESC(replace_global, "replace global pq algorithm with gpq");

module_param(use_hybrid, int, 0);
MODULE_PARM_DESC(use_hybrid,
		 "use hybrid pq computing, which uses both CPU and GPU");

static struct raid6_calls oldcall;

static void make_load_policy(void)
{
    ;
}

static size_t decide_gpu_load(int disks, size_t dsize)
{
    return 0;
}

static void cpu_gen_syndrome(int disks, size_t dsize, void **dps)
{
    if (replace_global) 
	oldcall.gen_syndrome(disks, dsize, dps);
    else
	raid6_call.gen_syndrome(disks, dsize, dps);
}

static void end_syndrome_gen(
    int disks, size_t dsize, void **dps, struct kgpu_buffer* buf)
{
    int p, b;
    size_t cpsz;
    p = dsize*(disks-2)/PAGE_SIZE;
    for (b=disks-2; b<disks; b++) {
	cpsz = 0;
	while (cpsz < dsize) {
	    memcpy((u8*)dps[b]+cpsz, __va(buf->pas[p]), PAGE_SIZE);
	    p++;
	    cpsz += PAGE_SIZE;
	}
    }
}

static int async_gpu_callback(
    struct kgpu_req *req, struct kgpu_resp *resp)
{
    struct gpq_async_data *adata = (struct gpq_async_data*)req->data;

    end_syndrome_gen(
	adata->disks, adata->dsize, adata->dps, adata->buf);

    complete(adata->c);

    free_gpu_buffer(adata->buf);
    free_kgpu_request(req);
    free_kgpu_response(resp);
    kfree(adata);

    return 0;
}

/*
 * A NULL completion c means synchronized call
 */
static void gpu_gen_syndrome(
    int disks, size_t dsize, void **dps, struct completion *c)
{
    size_t rsz = roundup(
	dsize*disks+sizeof(struct raid6_pq_data), PAGE_SIZE);

    struct raid6_pq_data *data;
    struct kgpu_req *req;
    struct kgpu_resp *resp;
    struct kgpu_buffer *buf;

    int p=0, b;
    size_t cpsz;
    u8 *gpos;
    unsigned long offset;

    buf = alloc_gpu_buffer(rsz);
    if (unlikely(!buf)) {
	gpq_log(KGPU_LOG_ERROR, "GPU buffer allocation failed\n");
	return;
    }

    req = alloc_kgpu_request();
    resp = alloc_kgpu_response();
    if (unlikely(!req || !resp)) {
	gpq_log(KGPU_LOG_ERROR,
		"GPU request/response allocation failed\n");
	return;
    }

    if (unlikely(dsize%PAGE_SIZE)) {
	gpq_log(KGPU_LOG_ERROR,
		"gpq only handle PAGE aligned memory\n");
	free_gpu_buffer(buf);
	
	return;
    }

    for (b=0; b<disks-2; b++) {
	cpsz=0;
	while (cpsz < dsize) {
	    memcpy(
		__va(buf->pas[p]),
		((u8*)dps[b])+cpsz, PAGE_SIZE);
	    p++;
	    cpsz += PAGE_SIZE;
	}
    }
    
    strcpy(req->kureq.sname, "raid6_pq");
    req->kureq.input     = buf->va;
    req->kureq.output    = ((u8*)(buf->va))+dsize*(disks-2);
    req->kureq.insize    = rsz;
    req->kureq.outsize   = dsize*2;
    req->kureq.data      = (u8*)(buf->va)+dsize*disks;
    req->kureq.datasize  = sizeof(struct raid6_pq_data);

    data = __va(buf->pas[(dsize*disks)>>PAGE_SHIFT]);
    data->dsize = (unsigned long)dsize;
    data->nr_d = (unsigned int)disks;

    if (c) {
	struct gpq_async_data *adata =
	    kmalloc(sizeof(struct gpq_async_data), GFP_KERNEL);
	if (!adata) {
	    gpq_log(KGPU_LOG_ERROR,
		    "out of memory for gpq async data\n");
	} else {	    
	    req->cb = async_gpu_callback;
	    req->data = adata;

	    adata->c = c;
	    adata->disks = disks;
	    adata->dsize = dsize;
	    adata->dps = dps;
	    adata->buf = buf;
	    
	    call_gpu(req, resp);
	}
    } else {
	if (call_gpu_sync(req, resp)) {
	    gpq_log(KGPU_LOG_ERROR, "callgpu failed\n");
	} else {
	    end_syndrome_gen(disks, dsize, dps, buf);
	}

	free_gpu_buffer(buf);
	free_kgpu_request(req);
	free_kgpu_response(resp);
    }    
}

#define NSTREAMS 16

static void multi_gpu_gen_syndrome(int disks, size_t dsize, void **dps)
{
    if ((dsize%(NSTREAMS*PAGE_SIZE)) != 0) {
	gpu_gen_syndrome(disks, dsize, dps, NULL);
    } else {
	struct completion cs[NSTREAMS];
	int i, j;
	void **ps;
	size_t tsksz = dsize/NSTREAMS;

	ps = kmalloc(sizeof(void*)*NSTREAMS*disks, GFP_KERNEL);
	if (!ps) {
	    gpq_log(KGPU_LOG_ERROR, "out of memory for dps\n");
	    gpu_gen_syndrome(disks, dsize, dps, NULL);
	} else {
	    for (i=0; i<NSTREAMS; i++) {
		for (j=0; j<disks; j++) {
		    ps[i*NSTREAMS+j] = ((char*)(dps[j]))+tsksz*i;
		}
		init_completion(cs+i);
		gpu_gen_syndrome(disks, tsksz, ps+i*NSTREAMS, cs+i);
	    }

	    for (i=0; i<NSTREAMS; i++) {
		wait_for_completion_interruptible(cs+i);
	    }

	    kfree(ps);
	}
    }
}

static void gpq_gen_syndrome(int disks, size_t dsize, void **dps)
{
    if (!use_hybrid) {
	/* gpu_gen_syndrome(disks, dsize, dps, NULL); */
	multi_gpu_gen_syndrome(disks, dsize, dps);
    } else {
	size_t gpuload = decide_gpu_load(disks, dsize);

	if (gpuload == dsize) {
	    gpu_gen_syndrome(disks, dsize, dps, NULL);
	} else {	    
	    void *cdps[MAX_DISKS];
	    int i;
	    DECLARE_COMPLETION(gpu_compl);
	   
	    for (i=0; i<disks; i++)
		cdps[i] = (char*)dps[i] + gpuload;
	    
	    gpu_gen_syndrome(disks, gpuload, dps, &gpu_compl);
	    cpu_gen_syndrome(disks, dsize-gpuload, cdps);
	    
	    wait_for_completion_interruptible(&gpu_compl);
	}
    }
}

const struct raid6_calls raid6_gpq = {
    gpq_gen_syndrome,
    NULL,
    "gpq",
    0
};

#include <linux/timex.h>

static long test_pq(int disks, size_t dsize, const struct raid6_calls *rc)
{
    struct timeval t0, t1;
    long t;
    int i;
    void **dps = vmalloc(sizeof(void*)*disks); //kmalloc(sizeof(void*)*disks, GFP_KERNEL);
    char *data = vmalloc(disks*dsize); //kmalloc(disks*dsize, GFP_KERNEL);

    if (!data || !dps) {
	gpq_log(KGPU_LOG_ERROR,
		"out of memory for %s test\n",
		rc->name);
	if (dps) vfree(dps);
	if (data) vfree(data);
	return 0;
    }

    for (i=0; i<disks; i++) {
	dps[i] = data + i*dsize;
    }

    do_gettimeofday(&t0);
    rc->gen_syndrome(disks, dsize, dps);
    do_gettimeofday(&t1);

    t = 1000000*(t1.tv_sec-t0.tv_sec) +
	((int)(t1.tv_usec) - (int)(t0.tv_usec));

    vfree(dps);
    vfree(data);

    return t;
}

long test_gpq(int disks, size_t dsize)
{
    return test_pq(disks, dsize, &raid6_gpq);
}
EXPORT_SYMBOL_GPL(test_gpq);

long test_cpq(int disks, size_t dsize)
{
    return test_pq(disks, dsize, replace_global? &oldcall:&raid6_call);
}
EXPORT_SYMBOL_GPL(test_cpq);

#define TEST_NDISKS 18
#define MIN_DSZ (1024*64)
#define MAX_DSZ (1024*1024)
#define TEST_TIMES_SHIFT 1
#define TEST_TIMES (1<<TEST_TIMES_SHIFT)

static void do_benchmark(void)
{
    size_t sz;
    const struct raid6_calls *gcall;
    const struct raid6_calls *const *rc;

    gcall = &raid6_gpq;

    /* init CUDA context */
    test_gpq(TEST_NDISKS, PAGE_SIZE);
    gpq_log(KGPU_LOG_PRINT, "init CUDA done\n");

    for (sz = MIN_DSZ; sz <= MAX_DSZ; sz += MIN_DSZ)
    {
	int i;
	long t;
	size_t tsz = sz*(TEST_NDISKS-2);

	t=0;
	for (i=0; i<TEST_TIMES; i++) {
	    t += test_pq(TEST_NDISKS, sz, gcall);		
	}
	t >>= TEST_TIMES_SHIFT; 
	
	gpq_log(KGPU_LOG_PRINT,
		"PQ Size: %10lu, %10s: %8luMB/s\n",
		tsz,
		gcall->name,
		tsz/t
	    );

	for (rc = raid6_algos; *rc; rc++) {
	    if (!(*rc)->valid || (*rc)->valid()) {
		t=0;
		for (i=0; i<TEST_TIMES; i++) {
		    t += test_pq(TEST_NDISKS, sz, *rc);		
		}
		t >>= TEST_TIMES_SHIFT; 
		
		gpq_log(KGPU_LOG_PRINT,
			"PQ Size: %10luKB, %10s: %8luMB/s\n",
			tsz>>10,
			(*rc)->name,
			tsz/t
		    );
	    }
	}
    }
}

static int __init raid6_gpq_init(void)
{
    if (replace_global) {
	oldcall = raid6_call;
	raid6_call = raid6_gpq;
	gpq_log(KGPU_LOG_PRINT,
		"global pq algorithm replaced with gpq\n");
    }
    if (use_hybrid) {
	make_load_policy();
    }

    do_benchmark();
    
    return 0;
}

static void __exit raid6_gpq_exit(void)
{
    if (replace_global) {
	raid6_call = oldcall;
    }
}

module_init(raid6_gpq_init);
module_exit(raid6_gpq_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gpq - GPU RAID6 PQ computing module");
MODULE_AUTHOR("Weibin Sun");