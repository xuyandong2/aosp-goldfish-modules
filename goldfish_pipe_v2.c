// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Intel, Inc.
 * Copyright (C) 2013 Intel, Inc.
 * Copyright (C) 2014 Linaro Limited
 * Copyright (C) 2011-2016 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* This source file contains the implementation of a special device driver
 * that intends to provide a *very* fast communication channel between the
 * guest system and the QEMU emulator.
 *
 * Usage from the guest is simply the following (error handling simplified):
 *
 *    int  fd = open("/dev/qemu_pipe",O_RDWR);
 *    .... write() or read() through the pipe.
 *
 * This driver doesn't deal with the exact protocol used during the session.
 * It is intended to be as simple as something like:
 *
 *    // do this _just_ after opening the fd to connect to a specific
 *    // emulator service.
 *    const char*  msg = "<pipename>";
 *    if (write(fd, msg, strlen(msg)+1) < 0) {
 *       ... could not connect to <pipename> service
 *       close(fd);
 *    }
 *
 *    // after this, simply read() and write() to communicate with the
 *    // service. Exact protocol details left as an exercise to the reader.
 *
 * This driver is very fast because it doesn't copy any data through
 * intermediate buffers, since the emulator is capable of translating
 * guest user addresses into host ones.
 *
 * Note that we must however ensure that each user page involved in the
 * exchange is properly mapped during a transfer.
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/bug.h>

#include "uapi/goldfish_dma.h"
#include "goldfish_pipe_qemu.h"
#include "goldfish_pipe.h"

/*
 * Update this when something changes in the driver's behavior so the host
 * can benefit from knowing it
 * Notes:
 *	version 2 was an intermediate release and isn't supported anymore.
 *	version 3 is goldfish_pipe_v2 without DMA support.
 *	version 4 (current) is goldfish_pipe_v2 with DMA support.
 */
enum {
	PIPE_DRIVER_VERSION = 4,
	PIPE_CURRENT_DEVICE_VERSION = 2
};

enum {
	MAX_BUFFERS_PER_COMMAND = 336,
	MAX_SIGNALLED_PIPES = 64,
	INITIAL_PIPES_CAPACITY = 64,
	DMA_REGION_MIN_SIZE = PAGE_SIZE,
	DMA_REGION_MAX_SIZE = 256 << 20
};

struct goldfish_pipe_dev;

static int goldfish_pipe_device_deinit(void *raw_dev,
				       struct platform_device *pdev);

/* A per-pipe command structure, shared with the host */
struct goldfish_pipe_command {
	s32 cmd;	/* PipeCmdCode, guest -> host */
	s32 id;		/* pipe id, guest -> host */
	s32 status;	/* command execution status, host -> guest */
	s32 reserved;	/* to pad to 64-bit boundary */
	union {
		/* Parameters for PIPE_CMD_{READ,WRITE} */
		struct {
			/* number of buffers, guest -> host */
			u32 buffers_count;
			/* number of consumed bytes, host -> guest */
			s32 consumed_size;
			/* buffer pointers, guest -> host */
			u64 ptrs[MAX_BUFFERS_PER_COMMAND];
			/* buffer sizes, guest -> host */
			u32 sizes[MAX_BUFFERS_PER_COMMAND];
		} rw_params;
		/* Parameters for PIPE_CMD_DMA_HOST_(UN)MAP */
		struct {
			u64 dma_paddr;
			u64 sz;
		} dma_maphost_params;
	};
};

/* A single signalled pipe information */
struct signalled_pipe_buffer {
	u32 id;
	u32 flags;
};

/* Parameters for the PIPE_CMD_OPEN command */
struct open_command_param {
	u64 command_buffer_ptr;
	u32 rw_params_max_count;
};

/* Device-level set of buffers shared with the host */
struct goldfish_pipe_dev_buffers {
	struct open_command_param open_command_params;
	struct signalled_pipe_buffer
		signalled_pipe_buffers[MAX_SIGNALLED_PIPES];
};

/*
 * The main data structure tracking state is
 * struct goldfish_dma_context, which is included
 * as an extra pointer field in struct goldfish_pipe.
 * Each such context is associated with possibly
 * one physical address and size describing the
 * allocated DMA region, and only one allocation
 * is allowed for each pipe fd. Further allocations
 * require more open()'s of pipe fd's.
 */
struct goldfish_dma_context {
	struct device *pdev_dev;	/* pointer to feed to dma_*_coherent */
	void *dma_vaddr;		/* kernel vaddr of dma region */
	size_t dma_size;		/* size of dma region */
	dma_addr_t phys_begin;		/* paddr of dma region */
	dma_addr_t phys_end;		/* paddr of dma region + dma_size */
};

/* This data type models a given pipe instance */
struct goldfish_pipe {
	/* pipe ID - index into goldfish_pipe_dev::pipes array */
	u32 id;

	/* The wake flags pipe is waiting for
	 * Note: not protected with any lock, uses atomic operations
	 *  and barriers to make it thread-safe.
	 */
	unsigned long flags;

	/* wake flags host have signalled,
	 *  - protected by goldfish_pipe_dev::lock
	 */
	unsigned long signalled_flags;

	/* A pointer to command buffer */
	struct goldfish_pipe_command *command_buffer;

	/* doubly linked list of signalled pipes, protected by
	 * goldfish_pipe_dev::lock
	 */
	struct goldfish_pipe *prev_signalled;
	struct goldfish_pipe *next_signalled;

	/*
	 * A pipe's own lock. Protects the following:
	 *  - *command_buffer - makes sure a command can safely write its
	 *    parameters to the host and read the results back.
	 */
	struct mutex lock;

	/* A wake queue for sleeping until host signals an event */
	wait_queue_head_t wake_queue;

	/* Pointer to the parent goldfish_pipe_dev instance */
	struct goldfish_pipe_dev *dev;

	/* A buffer of pages, too large to fit into a stack frame */
	struct page *pages[MAX_BUFFERS_PER_COMMAND];

	/* Holds information about reserved DMA region for this pipe */
	struct goldfish_dma_context *dma;
};

/* The global driver data. Holds a reference to the i/o page used to
 * communicate with the emulator, and a wake queue for blocked tasks
 * waiting to be awoken.
 */
struct goldfish_pipe_dev {
	/* Needed for 'remove' */
	struct goldfish_pipe_dev_base super;

	/*
	 * Global device spinlock. Protects the following members:
	 *  - pipes, pipes_capacity
	 *  - [*pipes, *pipes + pipes_capacity) - array data
	 *  - first_signalled_pipe,
	 *      goldfish_pipe::prev_signalled,
	 *      goldfish_pipe::next_signalled,
	 *      goldfish_pipe::signalled_flags - all singnalled-related fields,
	 *                                       in all allocated pipes
	 *  - open_command_params - PIPE_CMD_OPEN-related buffers
	 *
	 * It looks like a lot of different fields, but the trick is that
	 * the only operation that happens often is the signalled pipes array
	 * manipulation. That's why it's OK for now to keep the rest of the
	 * fields under the same lock. If we notice too much contention because
	 * of PIPE_CMD_OPEN, then we should add a separate lock there.
	 */
	spinlock_t lock;

	/*
	 * Array of the pipes of |pipes_capacity| elements,
	 * indexed by goldfish_pipe::id
	 */
	struct goldfish_pipe **pipes;
	u32 pipes_capacity;

	/* Pointers to the buffers host uses for interaction with this driver */
	struct goldfish_pipe_dev_buffers *buffers;

	/* Head of a doubly linked list of signalled pipes */
	struct goldfish_pipe *first_signalled_pipe;

	/* ptr to platform device's device struct */
	struct device *pdev_dev;

	/* Some device-specific data */
	unsigned char __iomem *base;

	/* an irq tasklet to run goldfish_interrupt_task */
	struct tasklet_struct irq_tasklet;

	struct miscdevice miscdev;

	/* DMA info */
	size_t dma_alloc_total;
};

static int goldfish_pipe_cmd_locked(struct goldfish_pipe *pipe,
				    enum PipeCmdCode cmd)
{
	pipe->command_buffer->cmd = cmd;
	/* failure by default */
	pipe->command_buffer->status = PIPE_ERROR_INVAL;
	writel(pipe->id, pipe->dev->base + PIPE_V2_REG_CMD);
	return pipe->command_buffer->status;
}

static int goldfish_pipe_cmd(struct goldfish_pipe *pipe, enum PipeCmdCode cmd)
{
	int status;

	if (mutex_lock_interruptible(&pipe->lock))
		return PIPE_ERROR_IO;
	status = goldfish_pipe_cmd_locked(pipe, cmd);
	mutex_unlock(&pipe->lock);
	return status;
}

/*
 * This function converts an error code returned by the emulator through
 * the PIPE_V2_REG_STATUS i/o register into a valid negative errno value.
 */
static int goldfish_pipe_error_convert(int status)
{
	switch (status) {
	case PIPE_ERROR_AGAIN:
		return -EAGAIN;
	case PIPE_ERROR_NOMEM:
		return -ENOMEM;
	case PIPE_ERROR_IO:
		return -EIO;
	default:
		return -EINVAL;
	}
}

static int goldfish_pin_user_pages(unsigned long first_page,
				   unsigned long last_page,
				   unsigned int last_page_size,
				   int is_write,
				   struct page *pages[MAX_BUFFERS_PER_COMMAND],
				   unsigned int *iter_last_page_size)
{
	int ret;
	int requested_pages = ((last_page - first_page) >> PAGE_SHIFT) + 1;

	if (requested_pages > MAX_BUFFERS_PER_COMMAND) {
		requested_pages = MAX_BUFFERS_PER_COMMAND;
		*iter_last_page_size = PAGE_SIZE;
	} else {
		*iter_last_page_size = last_page_size;
	}

	ret = get_user_pages_fast(first_page, requested_pages, !is_write,
				  pages);
	if (ret <= 0)
		return -EFAULT;
	if (ret < requested_pages)
		*iter_last_page_size = PAGE_SIZE;

	return ret;
}

static void release_user_pages(struct page **pages, int pages_count,
			       int is_write, s32 consumed_size)
{
	int i;

	for (i = 0; i < pages_count; i++) {
		if (!is_write && consumed_size > 0)
			set_page_dirty(pages[i]);
		put_page(pages[i]);
	}
}

/* Populate the call parameters, merging adjacent pages together */
static void populate_rw_params(struct page **pages,
			       int pages_count,
			       unsigned long address,
			       unsigned long address_end,
			       unsigned long first_page,
			       unsigned long last_page,
			       unsigned int iter_last_page_size,
			       int is_write,
			       struct goldfish_pipe_command *command)
{
	/*
	 * Process the first page separately - it's the only page that
	 * needs special handling for its start address.
	 */
	unsigned long xaddr = page_to_phys(pages[0]);
	unsigned long xaddr_prev = xaddr;
	int buffer_idx = 0;
	int i = 1;
	int size_on_page = first_page == last_page
			? (int)(address_end - address)
			: (PAGE_SIZE - (address & ~PAGE_MASK));
	command->rw_params.ptrs[0] = (u64)(xaddr | (address & ~PAGE_MASK));
	command->rw_params.sizes[0] = size_on_page;
	for (; i < pages_count; ++i) {
		xaddr = page_to_phys(pages[i]);
		size_on_page = (i == pages_count - 1) ?
			iter_last_page_size : PAGE_SIZE;
		if (xaddr == xaddr_prev + PAGE_SIZE) {
			command->rw_params.sizes[buffer_idx] += size_on_page;
		} else {
			++buffer_idx;
			command->rw_params.ptrs[buffer_idx] = (u64)xaddr;
			command->rw_params.sizes[buffer_idx] = size_on_page;
		}
		xaddr_prev = xaddr;
	}
	command->rw_params.buffers_count = buffer_idx + 1;
}

static int transfer_max_buffers(struct goldfish_pipe *pipe,
				unsigned long address,
				unsigned long address_end,
				int is_write,
				unsigned long last_page,
				unsigned int last_page_size,
				s32 *consumed_size,
				int *status)
{
	unsigned long first_page = address & PAGE_MASK;
	unsigned int iter_last_page_size;
	int pages_count;

	/* Serialize access to the pipe command buffers */
	if (mutex_lock_interruptible(&pipe->lock))
		return -ERESTARTSYS;

	pages_count = goldfish_pin_user_pages(first_page,
					      last_page,
					      last_page_size,
					      is_write,
					      pipe->pages,
					      &iter_last_page_size);
	if (pages_count < 0) {
		mutex_unlock(&pipe->lock);
		return pages_count;
	}

	populate_rw_params(pipe->pages, pages_count, address, address_end,
			   first_page, last_page, iter_last_page_size, is_write,
			   pipe->command_buffer);

	/* Transfer the data */
	*status = goldfish_pipe_cmd_locked(pipe,
				is_write ? PIPE_CMD_WRITE : PIPE_CMD_READ);

	*consumed_size = pipe->command_buffer->rw_params.consumed_size;

	release_user_pages(pipe->pages, pages_count, is_write, *consumed_size);

	mutex_unlock(&pipe->lock);
	return 0;
}

static int wait_for_host_signal(struct goldfish_pipe *pipe, int is_write)
{
	u32 wake_bit = is_write ? BIT_WAKE_ON_WRITE : BIT_WAKE_ON_READ;

	set_bit(wake_bit, &pipe->flags);

	/* Tell the emulator we're going to wait for a wake event */
	goldfish_pipe_cmd(pipe,
		is_write ? PIPE_CMD_WAKE_ON_WRITE : PIPE_CMD_WAKE_ON_READ);

	while (test_bit(wake_bit, &pipe->flags)) {
		if (wait_event_interruptible(pipe->wake_queue,
					     !test_bit(wake_bit, &pipe->flags)))
			return -ERESTARTSYS;

		if (test_bit(BIT_CLOSED_ON_HOST, &pipe->flags))
			return -EIO;
	}

	return 0;
}

static ssize_t goldfish_pipe_read_write(struct file *filp,
					char __user *buffer,
					size_t bufflen,
					int is_write)
{
	struct goldfish_pipe *pipe = filp->private_data;
	int count = 0, ret = -EINVAL;
	unsigned long address, address_end, last_page;
	unsigned int last_page_size;

	/* If the emulator already closed the pipe, no need to go further */
	if (unlikely(test_bit(BIT_CLOSED_ON_HOST, &pipe->flags)))
		return -EIO;
	/* Null reads or writes succeeds */
	if (unlikely(bufflen == 0))
		return 0;
	/* Check the buffer range for access */
	if (unlikely(!access_ok(buffer, bufflen)))
		return -EFAULT;

	address = (unsigned long)buffer;
	address_end = address + bufflen;
	last_page = (address_end - 1) & PAGE_MASK;
	last_page_size = ((address_end - 1) & ~PAGE_MASK) + 1;

	while (address < address_end) {
		s32 consumed_size;
		int status;

		ret = transfer_max_buffers(pipe, address, address_end, is_write,
					   last_page, last_page_size,
					   &consumed_size, &status);
		if (ret < 0)
			break;

		if (consumed_size > 0) {
			/* No matter what's the status, we've transferred
			 * something.
			 */
			count += consumed_size;
			address += consumed_size;
		}
		if (status > 0)
			continue;
		if (status == 0) {
			/* EOF */
			ret = 0;
			break;
		}
		if (count > 0) {
			/*
			 * An error occurred, but we already transferred
			 * something on one of the previous iterations.
			 * Just return what we already copied and log this
			 * err.
			 */
			if (status != PIPE_ERROR_AGAIN)
				dev_err_ratelimited(pipe->dev->pdev_dev,
					"backend error %d on %s\n",
					status, is_write ? "write" : "read");
			break;
		}

		/*
		 * If the error is not PIPE_ERROR_AGAIN, or if we are in
		 * non-blocking mode, just return the error code.
		 */
		if (status != PIPE_ERROR_AGAIN ||
			(filp->f_flags & O_NONBLOCK) != 0) {
			ret = goldfish_pipe_error_convert(status);
			break;
		}

		status = wait_for_host_signal(pipe, is_write);
		if (status < 0)
			return status;
	}

	if (count > 0)
		return count;
	return ret;
}

static ssize_t goldfish_pipe_read(struct file *filp, char __user *buffer,
				  size_t bufflen, loff_t *ppos)
{
	return goldfish_pipe_read_write(filp, buffer, bufflen,
					/* is_write */ 0);
}

static ssize_t goldfish_pipe_write(struct file *filp,
				   const char __user *buffer, size_t bufflen,
				   loff_t *ppos)
{
	/* cast away the const */
	char __user *no_const_buffer = (char __user *)buffer;

	return goldfish_pipe_read_write(filp, no_const_buffer, bufflen,
					/* is_write */ 1);
}

static unsigned int goldfish_pipe_poll(struct file *filp, poll_table *wait)
{
	struct goldfish_pipe *pipe = filp->private_data;
	unsigned int mask = 0;
	int status;

	poll_wait(filp, &pipe->wake_queue, wait);

	status = goldfish_pipe_cmd(pipe, PIPE_CMD_POLL);
	if (status < 0)
		return -ERESTARTSYS;

	if (status & PIPE_POLL_IN)
		mask |= POLLIN | POLLRDNORM;
	if (status & PIPE_POLL_OUT)
		mask |= POLLOUT | POLLWRNORM;
	if (status & PIPE_POLL_HUP)
		mask |= POLLHUP;
	if (test_bit(BIT_CLOSED_ON_HOST, &pipe->flags))
		mask |= POLLERR;

	return mask;
}

static void signalled_pipes_add_locked(struct goldfish_pipe_dev *dev,
				       u32 id, u32 flags)
{
	struct goldfish_pipe *pipe;

	if (WARN_ON(id >= dev->pipes_capacity))
		return;

	pipe = dev->pipes[id];
	if (!pipe)
		return;
	pipe->signalled_flags |= flags;

	if (pipe->prev_signalled || pipe->next_signalled ||
		dev->first_signalled_pipe == pipe)
		return;	/* already in the list */
	pipe->next_signalled = dev->first_signalled_pipe;
	if (dev->first_signalled_pipe)
		dev->first_signalled_pipe->prev_signalled = pipe;
	dev->first_signalled_pipe = pipe;
}

static void signalled_pipes_remove_locked(struct goldfish_pipe_dev *dev,
					  struct goldfish_pipe *pipe)
{
	if (pipe->prev_signalled)
		pipe->prev_signalled->next_signalled = pipe->next_signalled;
	if (pipe->next_signalled)
		pipe->next_signalled->prev_signalled = pipe->prev_signalled;
	if (pipe == dev->first_signalled_pipe)
		dev->first_signalled_pipe = pipe->next_signalled;
	pipe->prev_signalled = NULL;
	pipe->next_signalled = NULL;
}

static struct goldfish_pipe *signalled_pipes_pop_front(
		struct goldfish_pipe_dev *dev, int *wakes)
{
	struct goldfish_pipe *pipe;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);

	pipe = dev->first_signalled_pipe;
	if (pipe) {
		*wakes = pipe->signalled_flags;
		pipe->signalled_flags = 0;
		/*
		 * This is an optimized version of
		 * signalled_pipes_remove_locked()
		 * - We want to make it as fast as possible to
		 * wake the sleeping pipe operations faster.
		 */
		dev->first_signalled_pipe = pipe->next_signalled;
		if (dev->first_signalled_pipe)
			dev->first_signalled_pipe->prev_signalled = NULL;
		pipe->next_signalled = NULL;
	}

	spin_unlock_irqrestore(&dev->lock, flags);
	return pipe;
}

static void goldfish_interrupt_task(unsigned long dev_addr)
{
	/* Iterate over the signalled pipes and wake them one by one */
	struct goldfish_pipe_dev *dev = (struct goldfish_pipe_dev *)dev_addr;
	struct goldfish_pipe *pipe;
	int wakes;

	while ((pipe = signalled_pipes_pop_front(dev, &wakes)) != NULL) {
		if (wakes & PIPE_WAKE_CLOSED) {
			pipe->flags = 1 << BIT_CLOSED_ON_HOST;
		} else {
			if (wakes & PIPE_WAKE_READ)
				clear_bit(BIT_WAKE_ON_READ, &pipe->flags);
			if (wakes & PIPE_WAKE_WRITE)
				clear_bit(BIT_WAKE_ON_WRITE, &pipe->flags);
		}
		/*
		 * wake_up_interruptible() implies a write barrier, so don't
		 * explicitly add another one here.
		 */
		wake_up_interruptible(&pipe->wake_queue);
	}
}

/*
 * The general idea of the interrupt handling:
 *
 *  1. device raises an interrupt if there's at least one signalled pipe
 *  2. IRQ handler reads the signalled pipes and their count from the device
 *  3. device writes them into a shared buffer and returns the count
 *      it only resets the IRQ if it has returned all signalled pipes,
 *      otherwise it leaves it raised, so IRQ handler will be called
 *      again for the next chunk
 *  4. IRQ handler adds all returned pipes to the device's signalled pipes list
 *  5. IRQ handler launches a tasklet to process the signalled pipes from the
 *      list in a separate context
 */
static irqreturn_t goldfish_pipe_interrupt(int irq, void *dev_id)
{
	u32 count;
	u32 i;
	unsigned long flags;
	struct goldfish_pipe_dev *dev = dev_id;

	if (dev->super.deinit != &goldfish_pipe_device_deinit)
		return IRQ_NONE;

	/* Request the signalled pipes from the device */
	spin_lock_irqsave(&dev->lock, flags);

	count = readl(dev->base + PIPE_V2_REG_GET_SIGNALLED);
	if (count == 0) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return IRQ_NONE;
	}
	if (count > MAX_SIGNALLED_PIPES)
		count = MAX_SIGNALLED_PIPES;

	for (i = 0; i < count; ++i)
		signalled_pipes_add_locked(dev,
			dev->buffers->signalled_pipe_buffers[i].id,
			dev->buffers->signalled_pipe_buffers[i].flags);

	spin_unlock_irqrestore(&dev->lock, flags);

	tasklet_schedule(&dev->irq_tasklet);
	return IRQ_HANDLED;
}

static int get_free_pipe_id_locked(struct goldfish_pipe_dev *dev)
{
	int id;

	for (id = 0; id < dev->pipes_capacity; ++id)
		if (!dev->pipes[id])
			return id;

	{
		/* Reallocate the array.
		 * Since get_free_pipe_id_locked runs with interrupts disabled,
		 * we don't want to make calls that could lead to sleep.
		 */
		u32 new_capacity = 2 * dev->pipes_capacity;
		struct goldfish_pipe **pipes =
			kcalloc(new_capacity, sizeof(*pipes), GFP_ATOMIC);
		if (!pipes)
			return -ENOMEM;
		memcpy(pipes, dev->pipes, sizeof(*pipes) * dev->pipes_capacity);
		kfree(dev->pipes);
		dev->pipes = pipes;
		id = dev->pipes_capacity;
		dev->pipes_capacity = new_capacity;
	}
	return id;
}

/* A helper function to get the instance of goldfish_pipe_dev from file */
static struct goldfish_pipe_dev *to_goldfish_pipe_dev(struct file *file)
{
	struct miscdevice *miscdev = file->private_data;

	return container_of(miscdev, struct goldfish_pipe_dev, miscdev);
}

/**
 *	goldfish_pipe_open - open a channel to the AVD
 *	@inode: inode of device
 *	@file: file struct of opener
 *
 *	Create a new pipe link between the emulator and the use application.
 *	Each new request produces a new pipe.
 *
 *	Note: we use the pipe ID as a mux. All goldfish emulations are 32bit
 *	right now so this is fine. A move to 64bit will need this addressing
 */
static int goldfish_pipe_open(struct inode *inode, struct file *file)
{
	struct goldfish_pipe_dev *dev = to_goldfish_pipe_dev(file);
	unsigned long flags;
	int id;
	int status;

	/* Allocate new pipe kernel object */
	struct goldfish_pipe *pipe = kzalloc(sizeof(*pipe), GFP_KERNEL);

	if (!pipe)
		return -ENOMEM;

	pipe->dev = dev;
	mutex_init(&pipe->lock);
	init_waitqueue_head(&pipe->wake_queue);

	/*
	 * Command buffer needs to be allocated on its own page to make sure
	 * it is physically contiguous in host's address space.
	 */
	BUILD_BUG_ON(sizeof(struct goldfish_pipe_command) > PAGE_SIZE);
	pipe->command_buffer =
		(struct goldfish_pipe_command *)__get_free_page(GFP_KERNEL);
	if (!pipe->command_buffer) {
		status = -ENOMEM;
		goto err_pipe;
	}

	spin_lock_irqsave(&dev->lock, flags);

	id = get_free_pipe_id_locked(dev);
	if (id < 0) {
		status = id;
		goto err_id_locked;
	}

	dev->pipes[id] = pipe;
	pipe->id = id;
	pipe->command_buffer->id = id;

	/* Now tell the emulator we're opening a new pipe. */
	dev->buffers->open_command_params.rw_params_max_count =
			MAX_BUFFERS_PER_COMMAND;
	dev->buffers->open_command_params.command_buffer_ptr =
			(u64)(unsigned long)__pa(pipe->command_buffer);
	status = goldfish_pipe_cmd_locked(pipe, PIPE_CMD_OPEN);
	spin_unlock_irqrestore(&dev->lock, flags);
	if (status < 0)
		goto err_cmd;
	pipe->dma = NULL;

	/* All is done, save the pipe into the file's private data field */
	file->private_data = pipe;
	return 0;

err_cmd:
	spin_lock_irqsave(&dev->lock, flags);
	dev->pipes[id] = NULL;
err_id_locked:
	spin_unlock_irqrestore(&dev->lock, flags);
	free_page((unsigned long)pipe->command_buffer);
err_pipe:
	kfree(pipe);
	return status;
}

static void goldfish_pipe_dma_release_host(struct goldfish_pipe *pipe)
{
	struct goldfish_dma_context *dma = pipe->dma;
	struct device *pdev_dev;

	if (!dma)
		return;

	pdev_dev = pipe->dev->pdev_dev;

	if (dma->dma_vaddr) {
		pipe->command_buffer->dma_maphost_params.dma_paddr =
			dma->phys_begin;
		pipe->command_buffer->dma_maphost_params.sz = dma->dma_size;
		goldfish_pipe_cmd(pipe, PIPE_CMD_DMA_HOST_UNMAP);
	}
}

static void goldfish_pipe_dma_release_guest(struct goldfish_pipe *pipe)
{
	struct goldfish_dma_context *dma = pipe->dma;

	if (!dma)
		return;

	if (dma->dma_vaddr) {
		dma_free_coherent(dma->pdev_dev,
				  dma->dma_size,
				  dma->dma_vaddr,
				  dma->phys_begin);
		pipe->dev->dma_alloc_total -= dma->dma_size;
	}
}

static int goldfish_pipe_release(struct inode *inode, struct file *filp)
{
	unsigned long flags;
	struct goldfish_pipe *pipe = filp->private_data;
	struct goldfish_pipe_dev *dev = pipe->dev;

	/* The guest is closing the channel, so tell the emulator right now */
	goldfish_pipe_dma_release_host(pipe);
	goldfish_pipe_cmd(pipe, PIPE_CMD_CLOSE);

	spin_lock_irqsave(&dev->lock, flags);
	dev->pipes[pipe->id] = NULL;
	signalled_pipes_remove_locked(dev, pipe);
	spin_unlock_irqrestore(&dev->lock, flags);

	filp->private_data = NULL;

	/* Even if a fd is duped or involved in a forked process,
	 * open/release methods are called only once, ever.
	 * This makes goldfish_pipe_release a safe point
	 * to delete the DMA region.
	 */
	goldfish_pipe_dma_release_guest(pipe);

	kfree(pipe->dma);
	free_page((unsigned long)pipe->command_buffer);
	kfree(pipe);

	return 0;
}

/* VMA open/close are for debugging purposes only.
 * One might think that fork() (and thus pure calls to open())
 * will require some sort of bookkeeping or refcounting
 * for dma contexts (incl. when to call dma_free_coherent),
 * but |vm_private_data| field and |vma_open/close| are only
 * for situations where the driver needs to interact with vma's
 * directly with its own per-VMA data structure (which does
 * need to be refcounted).
 *
 * Here, we just use the kernel's existing
 * VMA processing; we don't do anything on our own.
 * The only reason we would want to do so is if we had to do
 * special processing for the virtual (not physical) memory
 * already associated with DMA memory; it is much less related
 * to the task of knowing when to alloc/dealloc DMA memory.
 */
static void goldfish_dma_vma_open(struct vm_area_struct *vma)
{
	/* Not used */
}

static void goldfish_dma_vma_close(struct vm_area_struct *vma)
{
	/* Not used */
}

static const struct vm_operations_struct goldfish_dma_vm_ops = {
	.open = goldfish_dma_vma_open,
	.close = goldfish_dma_vma_close,
};

static bool is_page_size_multiple(unsigned long sz)
{
	return !(sz & (PAGE_SIZE - 1));
}

static bool check_region_size_valid(size_t size)
{
	if (size < DMA_REGION_MIN_SIZE)
		return false;

	if (size > DMA_REGION_MAX_SIZE)
		return false;

	return is_page_size_multiple(size);
}

static int goldfish_pipe_dma_alloc_locked(struct goldfish_pipe *pipe)
{
	struct goldfish_dma_context *dma = pipe->dma;

	if (dma->dma_vaddr)
		return 0;

	dma->phys_begin = 0;
	dma->dma_vaddr = dma_alloc_coherent(dma->pdev_dev,
					    dma->dma_size,
					    &dma->phys_begin,
					    GFP_KERNEL);
	if (!dma->dma_vaddr)
		return -ENOMEM;

	dma->phys_end = dma->phys_begin + dma->dma_size;
	pipe->dev->dma_alloc_total += dma->dma_size;
	pipe->command_buffer->dma_maphost_params.dma_paddr = dma->phys_begin;
	pipe->command_buffer->dma_maphost_params.sz = dma->dma_size;

	goldfish_pipe_cmd_locked(pipe, PIPE_CMD_DMA_HOST_MAP);
	/* A workaround for b/110152998 */
	return 0;
}

static int goldfish_dma_mmap_locked(struct goldfish_pipe *pipe,
				    struct vm_area_struct *vma)
{
	struct goldfish_dma_context *dma = pipe->dma;
	struct device *pdev_dev = pipe->dev->pdev_dev;
	size_t sz_requested = vma->vm_end - vma->vm_start;
	int status;

	if (!check_region_size_valid(sz_requested)) {
		dev_err(pdev_dev, "%s: bad size (%zu) requested\n", __func__,
			sz_requested);
		return -EINVAL;
	}

	/* Alloc phys region if not allocated already. */
	status = goldfish_pipe_dma_alloc_locked(pipe);
	if (status)
		return status;

	status = remap_pfn_range(vma,
				 vma->vm_start,
				 dma->phys_begin >> PAGE_SHIFT,
				 sz_requested,
				 vma->vm_page_prot);
	if (status < 0) {
		dev_err(pdev_dev, "Cannot remap pfn range....\n");
		return -EAGAIN;
	}
	vma->vm_ops = &goldfish_dma_vm_ops;
	return 0;
}

/* When we call mmap() on a pipe fd, we obtain a pointer into
 * the physically contiguous DMA region of the pipe device
 * (Goldfish DMA).
 */
static int goldfish_dma_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct goldfish_pipe *pipe =
		(struct goldfish_pipe *)(filp->private_data);
	int status;

	if (mutex_lock_interruptible(&pipe->lock))
		return -ERESTARTSYS;

	status = goldfish_dma_mmap_locked(pipe, vma);
	mutex_unlock(&pipe->lock);
	return status;
}

static int goldfish_pipe_dma_create_region(struct goldfish_pipe *pipe,
					   size_t size)
{
	struct goldfish_dma_context *dma =
		kzalloc(sizeof(struct goldfish_dma_context), GFP_KERNEL);
	struct device *pdev_dev = pipe->dev->pdev_dev;

	if (dma) {
		if (mutex_lock_interruptible(&pipe->lock)) {
			kfree(dma);
			return -ERESTARTSYS;
		}

		if (pipe->dma) {
			mutex_unlock(&pipe->lock);
			kfree(dma);
			dev_err(pdev_dev, "The DMA region already allocated\n");
			return -EBUSY;
		}

		dma->dma_size = size;
		dma->pdev_dev = pipe->dev->pdev_dev;
		pipe->dma = dma;
		mutex_unlock(&pipe->lock);
		return 0;
	}

	dev_err(pdev_dev, "Could not allocate DMA context info!\n");
	return -ENOMEM;
}

static long goldfish_dma_ioctl_getoff(struct goldfish_pipe *pipe,
				      unsigned long arg)
{
	struct device *pdev_dev = pipe->dev->pdev_dev;
	struct goldfish_dma_ioctl_info ioctl_data;
	struct goldfish_dma_context *dma;

	BUILD_BUG_ON(FIELD_SIZEOF(struct goldfish_dma_ioctl_info, phys_begin) <
		FIELD_SIZEOF(struct goldfish_dma_context, phys_begin));

	if (mutex_lock_interruptible(&pipe->lock)) {
		dev_err(pdev_dev, "DMA_GETOFF: the pipe is not locked\n");
		return -EACCES;
	}

	dma = pipe->dma;
	if (dma) {
		ioctl_data.phys_begin = dma->phys_begin;
		ioctl_data.size = dma->dma_size;
	} else {
		ioctl_data.phys_begin = 0;
		ioctl_data.size = 0;
	}

	if (copy_to_user((void __user *)arg, &ioctl_data,
			 sizeof(ioctl_data))) {
		mutex_unlock(&pipe->lock);
		return -EFAULT;
	}

	mutex_unlock(&pipe->lock);
	return 0;
}

static long goldfish_dma_ioctl_create_region(struct goldfish_pipe *pipe,
					     unsigned long arg)
{
	struct goldfish_dma_ioctl_info ioctl_data;

	if (copy_from_user(&ioctl_data, (void __user *)arg, sizeof(ioctl_data)))
		return -EFAULT;

	if (!check_region_size_valid(ioctl_data.size)) {
		dev_err(pipe->dev->pdev_dev,
			"DMA_CREATE_REGION: bad size (%lld) requested\n",
			ioctl_data.size);
		return -EINVAL;
	}

	return goldfish_pipe_dma_create_region(pipe, ioctl_data.size);
}

static long goldfish_dma_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct goldfish_pipe *pipe =
		(struct goldfish_pipe *)(file->private_data);

	switch (cmd) {
	case GOLDFISH_DMA_IOC_LOCK:
		return 0;
	case GOLDFISH_DMA_IOC_UNLOCK:
		wake_up_interruptible(&pipe->wake_queue);
		return 0;
	case GOLDFISH_DMA_IOC_GETOFF:
		return goldfish_dma_ioctl_getoff(pipe, arg);
	case GOLDFISH_DMA_IOC_CREATE_REGION:
		return goldfish_dma_ioctl_create_region(pipe, arg);
	}
	return -ENOTTY;
}

static const struct file_operations goldfish_pipe_fops = {
	.owner = THIS_MODULE,
	.read = goldfish_pipe_read,
	.write = goldfish_pipe_write,
	.poll = goldfish_pipe_poll,
	.open = goldfish_pipe_open,
	.release = goldfish_pipe_release,
	/* DMA-related operations */
	.mmap = goldfish_dma_mmap,
	.unlocked_ioctl = goldfish_dma_ioctl,
	.compat_ioctl = goldfish_dma_ioctl,
};

static void init_miscdevice(struct miscdevice *miscdev)
{
	memset(miscdev, 0, sizeof(*miscdev));

	miscdev->minor = MISC_DYNAMIC_MINOR;
	miscdev->name = DEVICE_NAME;
	miscdev->fops = &goldfish_pipe_fops;
}

static void write_pa_addr(void *addr, void __iomem *portl, void __iomem *porth)
{
	const unsigned long paddr = __pa(addr);

	writel(upper_32_bits(paddr), porth);
	writel(lower_32_bits(paddr), portl);
}

int goldfish_pipe_device_v2_init(struct platform_device *pdev,
				 char __iomem *base,
				 int irq)
{
	struct goldfish_pipe_dev *dev;
	int err;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->super.deinit = &goldfish_pipe_device_deinit;
	spin_lock_init(&dev->lock);

	tasklet_init(&dev->irq_tasklet, &goldfish_interrupt_task,
		     (unsigned long)dev);

	err = devm_request_irq(&pdev->dev, irq,
			       goldfish_pipe_interrupt,
			       IRQF_SHARED, DEVICE_NAME, dev);
	if (err) {
		dev_err(&pdev->dev, "unable to allocate IRQ for v2\n");
		return err;
	}

	init_miscdevice(&dev->miscdev);
	err = misc_register(&dev->miscdev);
	if (err) {
		dev_err(&pdev->dev, "unable to register v2 device\n");
		return err;
	}

	dev->base = base;
	dev->pdev_dev = &pdev->dev;
	dev->first_signalled_pipe = NULL;
	dev->pipes_capacity = INITIAL_PIPES_CAPACITY;
	dev->pipes = kcalloc(dev->pipes_capacity, sizeof(*dev->pipes),
			     GFP_KERNEL);
	if (!dev->pipes) {
		misc_deregister(&dev->miscdev);
		return -ENOMEM;
	}

	/*
	 * We're going to pass two buffers, open_command_params and
	 * signalled_pipe_buffers, to the host. This means each of those buffers
	 * needs to be contained in a single physical page. The easiest choice
	 * is to just allocate a page and place the buffers in it.
	 */
	BUILD_BUG_ON(sizeof(struct goldfish_pipe_dev_buffers) > PAGE_SIZE);
	dev->buffers = (struct goldfish_pipe_dev_buffers *)
		__get_free_page(GFP_KERNEL);
	if (!dev->buffers) {
		kfree(dev->pipes);
		misc_deregister(&dev->miscdev);
		return -ENOMEM;
	}

	/* Send the buffer addresses to the host */
	write_pa_addr(&dev->buffers->signalled_pipe_buffers,
		      dev->base + PIPE_V2_REG_SIGNAL_BUFFER,
		      dev->base + PIPE_V2_REG_SIGNAL_BUFFER_HIGH);

	writel(MAX_SIGNALLED_PIPES,
	       dev->base + PIPE_V2_REG_SIGNAL_BUFFER_COUNT);

	write_pa_addr(&dev->buffers->open_command_params,
		      dev->base + PIPE_V2_REG_OPEN_BUFFER,
		      dev->base + PIPE_V2_REG_OPEN_BUFFER_HIGH);

	platform_set_drvdata(pdev, dev);
	return 0;
}

static int goldfish_pipe_device_deinit(void *raw_dev,
				       struct platform_device *pdev)
{
	struct goldfish_pipe_dev *dev = raw_dev;

	misc_deregister(&dev->miscdev);
	tasklet_kill(&dev->irq_tasklet);
	kfree(dev->pipes);
	free_page((unsigned long)dev->buffers);

	return 0;
}
