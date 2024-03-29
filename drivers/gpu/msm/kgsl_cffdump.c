/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* #define DEBUG */
#define ALIGN_CPU

#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/relay.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/sched.h>

#include "kgsl.h"
#include "kgsl_cffdump.h"
#include "kgsl_debugfs.h"

static struct rchan	*chan;
static struct dentry	*dir;
static int		suspended;
static size_t		dropped;
static size_t		subbuf_size = 256*1024;
static size_t		n_subbufs = 64;

/* forward declarations */
static void destroy_channel(void);
static struct rchan *create_channel(unsigned subbuf_size, unsigned n_subbufs);

static spinlock_t cffdump_lock;
static ulong serial_nr;
static ulong total_bytes;
static ulong total_syncmem;
static long last_sec;

#define MEMBUF_SIZE	64

#define CFF_OP_WRITE_REG        0x00000002
struct cff_op_write_reg {
	unsigned char op;
	uint addr;
	uint value;
} __attribute__((packed));

#define CFF_OP_POLL_REG         0x00000004
struct cff_op_poll_reg {
	unsigned char op;
	uint addr;
	uint value;
	uint mask;
} __attribute__((packed));

#define CFF_OP_WAIT_IRQ         0x00000005
struct cff_op_wait_irq {
	unsigned char op;
} __attribute__((packed));

#define CFF_OP_VERIFY_MEM_FILE  0x00000007
#define CFF_OP_RMW              0x0000000a

#define CFF_OP_WRITE_MEM        0x0000000b
struct cff_op_write_mem {
	unsigned char op;
	uint addr;
	uint value;
} __attribute__((packed));

#define CFF_OP_WRITE_MEMBUF     0x0000000c
struct cff_op_write_membuf {
	unsigned char op;
	uint addr;
	ushort count;
	uint buffer[MEMBUF_SIZE];
} __attribute__((packed));

#define CFF_OP_EOF              0xffffffff
struct cff_op_eof {
	unsigned char op;
} __attribute__((packed));


static void b64_encodeblock(unsigned char in[3], unsigned char out[4], int len)
{
	static const char tob64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmno"
		"pqrstuvwxyz0123456789+/";

	out[0] = tob64[in[0] >> 2];
	out[1] = tob64[((in[0] & 0x03) << 4) | ((in[1] & 0xf0) >> 4)];
	out[2] = (unsigned char) (len > 1 ? tob64[((in[1] & 0x0f) << 2)
		| ((in[2] & 0xc0) >> 6)] : '=');
	out[3] = (unsigned char) (len > 2 ? tob64[in[2] & 0x3f] : '=');
}

static void b64_encode(const unsigned char *in_buf, int in_size,
	unsigned char *out_buf, int out_bufsize, int *out_size)
{
	unsigned char in[3], out[4];
	int i, len;

	*out_size = 0;
	while (in_size > 0) {
		len = 0;
		for (i = 0; i < 3; ++i) {
			if (in_size-- > 0) {
				in[i] = *in_buf++;
				++len;
			} else
				in[i] = 0;
		}
		if (len) {
			b64_encodeblock(in, out, len);
			if (out_bufsize < 4) {
				pr_warn("kgsl: cffdump: %s: out of buffer\n",
					__func__);
				return;
			}
			for (i = 0; i < 4; ++i)
				*out_buf++ = out[i];
			*out_size += 4;
			out_bufsize -= 4;
		}
	}
}

#define KLOG_TMPBUF_SIZE (1024)
static void klog_printk(const char *fmt, ...)
{
	/* per-cpu klog formatting temporary buffer */
	static char klog_buf[NR_CPUS][KLOG_TMPBUF_SIZE];

	va_list args;
	int len;
	char *cbuf;
	unsigned long flags;

	local_irq_save(flags);
	cbuf = klog_buf[smp_processor_id()];
	va_start(args, fmt);
	len = vsnprintf(cbuf, KLOG_TMPBUF_SIZE, fmt, args);
	total_bytes += len;
	va_end(args);
	relay_write(chan, cbuf, len);
	local_irq_restore(flags);
}

static struct cff_op_write_membuf cff_op_write_membuf;
static void cffdump_membuf(int id, unsigned char *out_buf, int out_bufsize)
{
	void *data;
	int len, out_size;
	struct cff_op_write_mem cff_op_write_mem;

	uint addr = cff_op_write_membuf.addr
		- sizeof(uint)*cff_op_write_membuf.count;

	if (!cff_op_write_membuf.count) {
		pr_warn("kgsl: cffdump: membuf: count == 0, skipping");
		return;
	}

	if (cff_op_write_membuf.count != 1) {
		cff_op_write_membuf.op = CFF_OP_WRITE_MEMBUF;
		cff_op_write_membuf.addr = addr;
		len = sizeof(cff_op_write_membuf) -
			sizeof(uint)*(MEMBUF_SIZE - cff_op_write_membuf.count);
		data = &cff_op_write_membuf;
	} else {
		cff_op_write_mem.op = CFF_OP_WRITE_MEM;
		cff_op_write_mem.addr = addr;
		cff_op_write_mem.value = cff_op_write_membuf.buffer[0];
		data = &cff_op_write_mem;
		len = sizeof(cff_op_write_mem);
	}
	b64_encode(data, len, out_buf, out_bufsize, &out_size);
	out_buf[out_size] = 0;
	klog_printk("%ld:%d;%s\n", ++serial_nr, id, out_buf);
	cff_op_write_membuf.count = 0;
	cff_op_write_membuf.addr = 0;
}

static void cffdump_printline(int id, uint opcode, uint op1, uint op2,
	uint op3)
{
	struct cff_op_write_reg cff_op_write_reg;
	struct cff_op_poll_reg cff_op_poll_reg;
	struct cff_op_wait_irq cff_op_wait_irq;
	struct cff_op_eof cff_op_eof;
	unsigned char out_buf[sizeof(cff_op_write_membuf)/3*4 + 16];
	void *data;
	int len = 0, out_size;
	long cur_secs;

	spin_lock(&cffdump_lock);
	if (opcode == CFF_OP_WRITE_MEM) {
		if (op1 < 0x40000000 || op1 >= 0x60000000)
			KGSL_CORE_ERR("addr out-of-range: op1=%08x", op1);
		if ((cff_op_write_membuf.addr != op1 &&
			cff_op_write_membuf.count)
			|| (cff_op_write_membuf.count == MEMBUF_SIZE))
			cffdump_membuf(id, out_buf, sizeof(out_buf));

		cff_op_write_membuf.buffer[cff_op_write_membuf.count++] = op2;
		cff_op_write_membuf.addr = op1 + sizeof(uint);
		spin_unlock(&cffdump_lock);
		return;
	} else if (cff_op_write_membuf.count)
		cffdump_membuf(id, out_buf, sizeof(out_buf));
	spin_unlock(&cffdump_lock);

	switch (opcode) {
	case CFF_OP_WRITE_REG:
		cff_op_write_reg.op = opcode;
		cff_op_write_reg.addr = op1;
		cff_op_write_reg.value = op2;
		data = &cff_op_write_reg;
		len = sizeof(cff_op_write_reg);
		break;

	case CFF_OP_POLL_REG:
		cff_op_poll_reg.op = opcode;
		cff_op_poll_reg.addr = op1;
		cff_op_poll_reg.value = op2;
		cff_op_poll_reg.mask = op3;
		data = &cff_op_poll_reg;
		len = sizeof(cff_op_poll_reg);
		break;

	case CFF_OP_WAIT_IRQ:
		cff_op_wait_irq.op = opcode;
		data = &cff_op_wait_irq;
		len = sizeof(cff_op_wait_irq);
		break;

	case CFF_OP_EOF:
		cff_op_eof.op = opcode;
		data = &cff_op_eof;
		len = sizeof(cff_op_eof);
		break;
	}

	if (len) {
		b64_encode(data, len, out_buf, sizeof(out_buf), &out_size);
		out_buf[out_size] = 0;
		klog_printk("%ld:%d;%s\n", ++serial_nr, id, out_buf);
	} else
		pr_warn("kgsl: cffdump: unhandled opcode: %d\n", opcode);

	cur_secs = get_seconds();
	if ((cur_secs - last_sec) > 10 || (last_sec - cur_secs) > 10) {
		pr_info("kgsl: cffdump: total [bytes:%lu kB, syncmem:%lu kB], "
			"seq#: %lu\n", total_bytes/1024, total_syncmem/1024,
			serial_nr);
		last_sec = cur_secs;
	}
}

void kgsl_cffdump_init()
{
	struct dentry *debugfs_dir = kgsl_get_debugfs_dir();

#ifdef ALIGN_CPU
	cpumask_t mask;

	cpumask_clear(&mask);
	cpumask_set_cpu(1, &mask);
	sched_setaffinity(0, &mask);
#endif
	if (!debugfs_dir || IS_ERR(debugfs_dir)) {
		KGSL_CORE_ERR("Debugfs directory is bad\n");
		return;
	}

	kgsl_cff_dump_enable = 1;

	spin_lock_init(&cffdump_lock);

	dir = debugfs_create_dir("cff", debugfs_dir);
	if (!dir) {
		KGSL_CORE_ERR("debugfs_create_dir failed\n");
		return;
	}

	chan = create_channel(subbuf_size, n_subbufs);
}

void kgsl_cffdump_destroy()
{
	if (chan)
		relay_flush(chan);
	destroy_channel();
	if (dir)
		debugfs_remove(dir);
}

void kgsl_cffdump_open(enum kgsl_deviceid device_id)
{
}

void kgsl_cffdump_close(enum kgsl_deviceid device_id)
{
	cffdump_printline(device_id, CFF_OP_EOF, 0, 0, 0);
}

void kgsl_cffdump_syncmem(struct kgsl_device_private *dev_priv,
	const struct kgsl_memdesc *memdesc, uint gpuaddr, uint sizebytes,
	bool clean_cache)
{
	const void *src;
	uint physaddr;

	if (!kgsl_cff_dump_enable)
		return;

	total_syncmem += sizebytes;

	if (memdesc == NULL) {
		struct kgsl_mem_entry *entry;
		spin_lock(&dev_priv->process_priv->mem_lock);
		entry = kgsl_sharedmem_find_region(dev_priv->process_priv,
			gpuaddr, sizebytes);
		spin_unlock(&dev_priv->process_priv->mem_lock);
		if (entry == NULL) {
			KGSL_CORE_ERR("did not find mapping "
				"for gpuaddr: 0x%08x\n", gpuaddr);
			return;
		}
		memdesc = &entry->memdesc;
	}
	physaddr = kgsl_get_realaddr(memdesc) + (gpuaddr - memdesc->gpuaddr);

	src = (uint *)kgsl_gpuaddr_to_vaddr(memdesc, gpuaddr);
	if (memdesc->hostptr == NULL) {
		KGSL_CORE_ERR("no kernel mapping for "
			"gpuaddr: 0x%08x, m->host: 0x%p, phys: 0x%08x\n",
			gpuaddr, memdesc->hostptr, memdesc->physaddr);
		return;
	}

	if (clean_cache) {
		/* Ensure that this memory region is not read from the
		 * cache but fetched fresh */

		mb();

		kgsl_cache_range_op(memdesc->hostptr, memdesc->size,
				    memdesc->type, KGSL_CACHE_OP_INV);
	}

	BUG_ON(physaddr > 0x66000000 && physaddr < 0x66ffffff);
	while (sizebytes > 3) {
		cffdump_printline(-1, CFF_OP_WRITE_MEM, physaddr, *(uint *)src,
			0);
		physaddr += 4;
		src += 4;
		sizebytes -= 4;
	}
	if (sizebytes > 0)
		cffdump_printline(-1, CFF_OP_WRITE_MEM, physaddr, *(uint *)src,
			0);
}

void kgsl_cffdump_setmem(uint addr, uint value, uint sizebytes)
{
	if (!kgsl_cff_dump_enable)
		return;

	BUG_ON(addr > 0x66000000 && addr < 0x66ffffff);
	while (sizebytes > 3) {
		/* Use 32bit memory writes as long as there's at least
		 * 4 bytes left */
		cffdump_printline(-1, CFF_OP_WRITE_MEM, addr, value, 0);
		addr += 4;
		sizebytes -= 4;
	}
	if (sizebytes > 0)
		cffdump_printline(-1, CFF_OP_WRITE_MEM, addr, value, 0);
}

void kgsl_cffdump_regwrite(enum kgsl_deviceid device_id, uint addr,
	uint value)
{
	if (!kgsl_cff_dump_enable)
		return;

	cffdump_printline(device_id, CFF_OP_WRITE_REG, addr, value, 0);
}

void kgsl_cffdump_regpoll(enum kgsl_deviceid device_id, uint addr,
	uint value, uint mask)
{
	if (!kgsl_cff_dump_enable)
		return;

	cffdump_printline(device_id, CFF_OP_POLL_REG, addr, value, mask);
}

void kgsl_cffdump_slavewrite(uint addr, uint value)
{
	if (!kgsl_cff_dump_enable)
		return;

	cffdump_printline(-1, CFF_OP_WRITE_REG, addr, value, 0);
}

int kgsl_cffdump_waitirq(void)
{
	if (!kgsl_cff_dump_enable)
		return 0;

	cffdump_printline(-1, CFF_OP_WAIT_IRQ, 0, 0, 0);

	return 1;
}
EXPORT_SYMBOL(kgsl_cffdump_waitirq);

#define ADDRESS_STACK_SIZE 256
#define GET_PM4_TYPE3_OPCODE(x) ((*(x) >> 8) & 0xFF)
static unsigned int kgsl_cffdump_addr_count;

static bool kgsl_cffdump_handle_type3(struct kgsl_device_private *dev_priv,
	uint *hostaddr, bool check_only)
{
	static uint addr_stack[ADDRESS_STACK_SIZE];
	static uint size_stack[ADDRESS_STACK_SIZE];

	switch (GET_PM4_TYPE3_OPCODE(hostaddr)) {
	case PM4_INDIRECT_BUFFER_PFD:
	case PM4_INDIRECT_BUFFER:
	{
		/* traverse indirect buffers */
		int i;
		uint ibaddr = hostaddr[1];
		uint ibsize = hostaddr[2];

		/* is this address already in encountered? */
		for (i = 0;
			i < kgsl_cffdump_addr_count && addr_stack[i] != ibaddr;
			++i)
			;

		if (kgsl_cffdump_addr_count == i) {
			addr_stack[kgsl_cffdump_addr_count] = ibaddr;
			size_stack[kgsl_cffdump_addr_count++] = ibsize;

			if (kgsl_cffdump_addr_count >= ADDRESS_STACK_SIZE) {
				KGSL_CORE_ERR("stack overflow\n");
				return false;
			}

			return kgsl_cffdump_parse_ibs(dev_priv, NULL,
				ibaddr, ibsize, check_only);
		} else if (size_stack[i] != ibsize) {
			KGSL_CORE_ERR("gpuaddr: 0x%08x, "
				"wc: %u, with size wc: %u already on the "
				"stack\n", ibaddr, ibsize, size_stack[i]);
			return false;
		}
	}
	break;
	}

	return true;
}

/*
 * Traverse IBs and dump them to test vector. Detect swap by inspecting
 * register writes, keeping note of the current state, and dump
 * framebuffer config to test vector
 */
bool kgsl_cffdump_parse_ibs(struct kgsl_device_private *dev_priv,
	const struct kgsl_memdesc *memdesc, uint gpuaddr, int sizedwords,
	bool check_only)
{
	static uint level; /* recursion level */
	bool ret = true;
	uint *hostaddr, *hoststart;
	int dwords_left = sizedwords; /* dwords left in the current command
					 buffer */

	if (level == 0)
		kgsl_cffdump_addr_count = 0;

	if (memdesc == NULL) {
		struct kgsl_mem_entry *entry;
		spin_lock(&dev_priv->process_priv->mem_lock);
		entry = kgsl_sharedmem_find_region(dev_priv->process_priv,
			gpuaddr, sizedwords * sizeof(uint));
		spin_unlock(&dev_priv->process_priv->mem_lock);
		if (entry == NULL) {
			KGSL_CORE_ERR("did not find mapping "
				"for gpuaddr: 0x%08x\n", gpuaddr);
			return true;
		}
		memdesc = &entry->memdesc;
	}

	hostaddr = (uint *)kgsl_gpuaddr_to_vaddr(memdesc, gpuaddr);	
	if (hostaddr == NULL) {
		KGSL_CORE_ERR("no kernel mapping for "		
			"gpuaddr: 0x%08x\n", gpuaddr);
		return true;
	}

	hoststart = hostaddr;

	level++;

	if (!memdesc->physaddr) {
		KGSL_CORE_ERR("no physaddr");
		return true;
	} else {
		mb();
		kgsl_cache_range_op(memdesc->hostptr, memdesc->size,
				    memdesc->type, KGSL_CACHE_OP_INV);
	}

#ifdef DEBUG
	pr_info("kgsl: cffdump: ib: gpuaddr:0x%08x, wc:%d, hptr:%p\n",
		gpuaddr, sizedwords, hostaddr);
#endif

	while (dwords_left > 0) {
		int count = 0; /* dword count including packet header */
		bool cur_ret = true;

		switch (*hostaddr >> 30) {
		case 0x0: /* type-0 */
			count = (*hostaddr >> 16)+2;
			break;
		case 0x1: /* type-1 */
			count = 2;
			break;
		case 0x3: /* type-3 */
			count = ((*hostaddr >> 16) & 0x3fff) + 2;
			cur_ret = kgsl_cffdump_handle_type3(dev_priv,
				hostaddr, check_only);
			break;
		default:
			pr_warn("kgsl: cffdump: parse-ib: unexpected type: "
				"type:%d, word:0x%08x @ 0x%p, gpu:0x%08x\n",
				*hostaddr >> 30, *hostaddr, hostaddr,
				gpuaddr+4*(sizedwords-dwords_left));
			cur_ret = false;
			count = dwords_left;
			break;
		}

#ifdef DEBUG
		if (!cur_ret) {
			pr_info("kgsl: cffdump: bad sub-type: #:%d/%d, v:0x%08x"
				" @ 0x%p[gb:0x%08x], level:%d\n",
				sizedwords-dwords_left, sizedwords, *hostaddr,
				hostaddr, gpuaddr+4*(sizedwords-dwords_left),
				level);

			print_hex_dump(KERN_ERR, level == 1 ? "IB1:" : "IB2:",
				DUMP_PREFIX_OFFSET, 32, 4, hoststart,
				sizedwords*4, 0);
		}
#endif
		ret = ret && cur_ret;

		/* jump to next packet */
		dwords_left -= count;
		hostaddr += count;
		cur_ret = dwords_left >= 0;

#ifdef DEBUG
		if (!cur_ret) {
			pr_info("kgsl: cffdump: bad count: c:%d, #:%d/%d, "
				"v:0x%08x @ 0x%p[gb:0x%08x], level:%d\n",
				count, sizedwords-(dwords_left+count),
				sizedwords, *(hostaddr-count), hostaddr-count,
				gpuaddr+4*(sizedwords-(dwords_left+count)),
				level);

			print_hex_dump(KERN_ERR, level == 1 ? "IB1:" : "IB2:",
				DUMP_PREFIX_OFFSET, 32, 4, hoststart,
				sizedwords*4, 0);
		}
#endif

		ret = ret && cur_ret;
	}

	if (!ret)
		pr_info("kgsl: cffdump: parsing failed: gpuaddr:0x%08x, "
			"host:0x%p, wc:%d\n", gpuaddr, hoststart, sizedwords);

	if (!check_only) {
#ifdef DEBUG
		uint offset = gpuaddr - memdesc->gpuaddr;
		pr_info("kgsl: cffdump: ib-dump: hostptr:%p, gpuaddr:%08x, "
			"physaddr:%08x, offset:%d, size:%d", hoststart,
			gpuaddr, memdesc->physaddr + offset, offset,
			sizedwords*4);
#endif
		kgsl_cffdump_syncmem(dev_priv, memdesc, gpuaddr, sizedwords*4,
			false);
	}

	level--;

	return ret;
}

static int subbuf_start_handler(struct rchan_buf *buf,
	void *subbuf, void *prev_subbuf, uint prev_padding)
{
	pr_debug("kgsl: cffdump: subbuf_start_handler(subbuf=%p, prev_subbuf"
		"=%p, prev_padding=%08x)\n", subbuf, prev_subbuf, prev_padding);

	if (relay_buf_full(buf)) {
		if (!suspended) {
			suspended = 1;
			pr_warn("kgsl: cffdump: relay: cpu %d buffer full!!!\n",
				smp_processor_id());
		}
		dropped++;
		return 0;
	} else if (suspended) {
		suspended = 0;
		pr_warn("kgsl: cffdump: relay: cpu %d buffer no longer full.\n",
			smp_processor_id());
	}

	subbuf_start_reserve(buf, 0);
	return 1;
}

static struct dentry *create_buf_file_handler(const char *filename,
	struct dentry *parent, int mode, struct rchan_buf *buf,
	int *is_global)
{
	return debugfs_create_file(filename, mode, parent, buf,
				       &relay_file_operations);
}

/*
 * file_remove() default callback.  Removes relay file in debugfs.
 */
static int remove_buf_file_handler(struct dentry *dentry)
{
	pr_info("kgsl: cffdump: %s()\n", __func__);
	debugfs_remove(dentry);
	return 0;
}

/*
 * relay callbacks
 */
static struct rchan_callbacks relay_callbacks = {
	.subbuf_start = subbuf_start_handler,
	.create_buf_file = create_buf_file_handler,
	.remove_buf_file = remove_buf_file_handler,
};

/**
 *	create_channel - creates channel /debug/klog/cpuXXX
 *
 *	Creates channel along with associated produced/consumed control files
 *
 *	Returns channel on success, NULL otherwise
 */
static struct rchan *create_channel(unsigned subbuf_size, unsigned n_subbufs)
{
	struct rchan *chan;

	pr_info("kgsl: cffdump: relay: create_channel: subbuf_size %u, "
		"n_subbufs %u, dir 0x%p\n", subbuf_size, n_subbufs, dir);

	chan = relay_open("cpu", dir, subbuf_size,
			  n_subbufs, &relay_callbacks, NULL);
	if (!chan) {
		KGSL_CORE_ERR("relay_open failed\n");
		return NULL;
	}

	suspended = 0;
	dropped = 0;

	return chan;
}

/**
 *	destroy_channel - destroys channel /debug/kgsl/cff/cpuXXX
 *
 *	Destroys channel along with associated produced/consumed control files
 */
static void destroy_channel(void)
{
	pr_info("kgsl: cffdump: relay: destroy_channel\n");
	if (chan) {
		relay_close(chan);
		chan = NULL;
	}
}

