/*
 * NVDLA queue and task management for T194
 *
 * Copyright (c) 2016-2023, NVIDIA Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/arm64-barrier.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#include <uapi/linux/nvhost_ioctl.h>

#include "nvdla.h"
#include "dla_channel.h"
#include "dla_queue.h"
#include "nvdla_debug.h"
#include "dla_os_interface.h"

#define CREATE_TRACE_POINTS
#include <trace/events/nvdla_ftrace.h>

#define NVDLA_QUEUE_ABORT_TIMEOUT	10000	/* 10 sec */
#define NVDLA_QUEUE_ABORT_RETRY_PERIOD	500	/* 500 ms */

/* struct to hold information required for nvdla_add_fence_action_cb */
struct nvdla_add_fence_action_cb_args {
	struct nvdla_queue *queue;
	u8 **mem;
};

/* Computing a unique id to identify a task in a particular queue */
static uint32_t nvdla_compute_task_id(u16 sequence_id, u16 queue_id)
{
	return (queue_id << 16 | sequence_id);
}

/* task management API's */
static void nvdla_queue_dump_op(struct nvdla_queue *queue, struct seq_file *s)
{
	struct nvdla_task *task = NULL;
	int i = 0;

	seq_printf(s, "Queue[%p] id[%u]\n", queue, queue->id);

	mutex_lock(&queue->list_lock);
	list_for_each_entry(task, &queue->tasklist, list) {
		int j;

		seq_printf(s, "#[%u]th task[%p]\n", i++, task);

		seq_printf(s, "    num of prefences[%d] \n",
				task->num_prefences);
		for (j = 0; j < task->num_prefences; j++)
			seq_printf(s, "    prefence[%d]\n\t"
				"syncpoint_index=[%u], syncpoint_value=[%u]\n",
				j,
				task->prefences[j].syncpoint_index,
				task->prefences[j].syncpoint_value);

		seq_printf(s, "    num of postfences[%d] \n",
				task->num_postfences);

		for (j = 0; j < task->num_postfences; j++)
			seq_printf(s, "    postfence[%d]\n\t"
				"syncpoint_index=[%u], syncpoint_value=[%u]\n",
				j,
				task->postfences[j].syncpoint_index,
				task->postfences[j].syncpoint_value);


	}
	spec_bar(); /* break_spec_p#5_1 */
	mutex_unlock(&queue->list_lock);
}

int nvdla_get_task_mem(struct nvdla_queue *queue,
			struct nvdla_task **ptask)
{
	int err;
	struct nvdla_task *task = NULL;
	struct nvdla_queue_task_mem_info task_mem_info;
	struct platform_device *pdev = queue->pool->pdev;
	int n_retries = (NVDLA_TASK_MEM_AVAIL_TIMEOUT_MS /
					NVDLA_TASK_MEM_AVAIL_RETRY_PERIOD);

	nvdla_dbg_fn(pdev, "");

	/* get mem task descriptor and task mem from task_mem_pool */
	do {
		n_retries = n_retries - 1;
		err = nvdla_queue_alloc_task_memory(queue, &task_mem_info);
	} while ((n_retries > 0) && (err == -EAGAIN));

	task = task_mem_info.kmem_addr;
	if ((err < 0) || !task)
		goto fail_to_assign_pool;

	/* check if IOVA is correctly aligned */
	if (task_mem_info.dma_addr & 0xff) {
		err = -EFAULT;
		goto fail_to_aligned_dma;
	}

	task->task_desc = task_mem_info.va;
	task->task_desc_pa = task_mem_info.dma_addr;
	task->pool_index = task_mem_info.pool_index;

	*ptask = task;

fail_to_aligned_dma:
fail_to_assign_pool:
	return err;
}

void nvdla_put_task_mem(struct nvdla_task *task)
{
	/* release allocated task desc and task mem */
	nvdla_queue_free_task_memory(task->queue, task->pool_index);

	task = NULL;
}

void task_free(struct kref *ref)
{
	struct nvdla_task *task = container_of(ref, struct nvdla_task, ref);
	struct platform_device *pdev = task->queue->pool->pdev;

	nvdla_dbg_info(pdev, "freeing task[%p]", task);

	nvdla_put_task_mem(task);
}

void nvdla_task_put(struct nvdla_task *task)
{
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;

	nvdla_dbg_fn(pdev, "task:[%p]", task);

	kref_put(&task->ref, task_free);

	/* Queue should be last to update */
	nvdla_queue_put(queue);
}

void nvdla_task_get(struct nvdla_task *task)
{
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;

	nvdla_dbg_fn(pdev, "task:[%p]", task);

	/* update queue refcnt */
	nvdla_queue_get(task->queue);

	kref_get(&task->ref);
}

static int nvdla_unmap_task_memory(struct nvdla_task *task)
{
	int ii;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;

	nvdla_dbg_fn(pdev, "task:[%p]", task);

	/* unpin address list */
	for (ii = 0; ii < task->num_addresses; ii++) {
		if (task->memory_handles[ii].type ==
				NVDLA_BUFFER_TYPE_INTERNAL) {
			/* No unpinning required for internal buffers */
			continue;
		}
		if (task->memory_handles[ii].handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->memory_handles[ii].handle, 1);
		}
	}
	nvdla_dbg_fn(pdev, "all mem handles unmaped");

	/* unpin prefences memory */
	for (ii = 0; ii < task->num_prefences; ii++) {
		if ((task->prefences[ii].type == NVDEV_FENCE_TYPE_SEMAPHORE ||
		   task->prefences[ii].type == NVDEV_FENCE_TYPE_SEMAPHORE_TS) &&
		   task->prefences[ii].semaphore_handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->prefences[ii].semaphore_handle, 1);
		}
	}
	nvdla_dbg_fn(pdev, "all prefences unmaped");

	/* unpin input task status memory */
	for (ii = 0; ii < task->num_in_task_status; ii++) {
		if (task->in_task_status[ii].handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->in_task_status[ii].handle, 1);
		}
	}
	nvdla_dbg_fn(pdev, "all in task status unmaped");

	/* unpin postfences memory */
	for (ii = 0; ii < task->num_postfences; ii++) {
		if ((task->postfences[ii].type == NVDEV_FENCE_TYPE_SEMAPHORE ||
		  task->postfences[ii].type == NVDEV_FENCE_TYPE_SEMAPHORE_TS) &&
		  task->postfences[ii].semaphore_handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->postfences[ii].semaphore_handle, 1);
		}
	}
	nvdla_dbg_fn(pdev, "all postfences unmaped");

	/* unpin output task status memory */
	for (ii = 0; ii < task->num_sof_task_status; ii++) {
		if (task->sof_task_status[ii].handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->sof_task_status[ii].handle, 1);
		}
	}

	for (ii = 0; ii < task->num_eof_task_status; ii++) {
		if (task->eof_task_status[ii].handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->eof_task_status[ii].handle, 1);
		}
	}
	nvdla_dbg_fn(pdev, "all out task status unmaped");

	/* unpin output timestamp memory */
	for (ii = 0; ii < task->num_sof_timestamps; ii++) {
		if (task->sof_timestamps[ii].handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->sof_timestamps[ii].handle, 1);
		}
	}

	for (ii = 0; ii < task->num_eof_timestamps; ii++) {
		if (task->eof_timestamps[ii].handle) {
			nvdla_buffer_submit_unpin(task->buffers,
				&task->eof_timestamps[ii].handle, 1);
		}
	}
	nvdla_dbg_fn(pdev, "all out timestamps unmaped");

	spec_bar(); /* break_spec_p#5_1 */

	return 0;
}

static void nvdla_task_free_locked(struct nvdla_task *task)
{
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;

	nvdla_dbg_info(pdev,
		"task[%p] completed. syncpt[%d] fence[%d]",
		task, queue->syncpt_id, task->fence);

	/* unmap all memory shared with engine */
	nvdla_unmap_task_memory(task);

	/* update takslist */
	list_del(&task->list);

	/* give taks refs */
	nvdla_task_put(task);
}

static inline int nvdla_get_max_preaction_size(void)
{
	return (((MAX_NVDLA_PREFENCES_PER_TASK + MAX_NVDLA_IN_STATUS_PER_TASK +
			MAX_NVDLA_OUT_STATUS_PER_TASK +
			MAX_NVDLA_OUT_TIMESTAMPS_PER_TASK) *
		sizeof(struct dla_action_opcode)) +
		(MAX_NVDLA_PREFENCES_PER_TASK *
			sizeof(struct dla_action_semaphore)) +
		((MAX_NVDLA_IN_STATUS_PER_TASK + MAX_NVDLA_OUT_STATUS_PER_TASK) *
			sizeof(struct dla_action_task_status)) +
		(MAX_NVDLA_OUT_TIMESTAMPS_PER_TASK *
			sizeof(struct dla_action_timestamp)) +
		sizeof(struct dla_action_opcode));
}

static inline int nvdla_get_max_postaction_size(void)
{
	return (((MAX_NVDLA_POSTFENCES_PER_TASK +
				MAX_NVDLA_OUT_STATUS_PER_TASK +
				MAX_NVDLA_OUT_TIMESTAMPS_PER_TASK +
				NUM_PROFILING_POSTACTION) *
		sizeof(struct dla_action_opcode)) +
		(MAX_NVDLA_POSTFENCES_PER_TASK *
			sizeof(struct dla_action_semaphore)) +
		((MAX_NVDLA_OUT_STATUS_PER_TASK +
			NUM_PROFILING_POSTACTION) *
			sizeof(struct dla_action_task_status)) +
		(MAX_NVDLA_OUT_TIMESTAMPS_PER_TASK *
			sizeof(struct dla_action_timestamp)) +
		sizeof(struct dla_action_opcode));
}

static inline size_t nvdla_profile_status_offset(struct nvdla_task *task)
{
	size_t offset = 0;

	offset += sizeof(struct dla_task_descriptor);
	offset += (2 * MAX_NUM_ACTION_LIST * sizeof(struct dla_action_list));
	offset += nvdla_get_max_preaction_size();
	offset += nvdla_get_max_postaction_size();

	offset = roundup(offset, 8);
	offset += MAX_NUM_NVDLA_BUFFERS_PER_TASK * sizeof(struct dla_mem_addr);

	offset = roundup(offset, 8);

	return offset;
}


#if IS_ENABLED(CONFIG_TEGRA_GRHOST)
/*
 * This function definition can be removed once support
 * for the NVIDIA Linux v5.10 kernel is removed.
 */
static void nvdla_queue_update(void *priv, int unused)
#else
static void nvdla_queue_update(void *priv)
#endif
{
	int task_complete;
	struct nvdla_task *task, *safe;
	struct nvdla_queue *queue = priv;
	struct platform_device *pdev = queue->pool->pdev;
	struct nvhost_notification *tsp_notifier;
	u64 timestamp_start, timestamp_end;
	u64 *timestamp_ptr;
	int n_tasks_completed = 0;
	uint32_t task_id;
	int i;
	mutex_lock(&queue->list_lock);

	nvdla_dbg_fn(pdev, "");

	/* check which task(s) finished */
	list_for_each_entry_safe(task, safe, &queue->tasklist, list) {
		task_id = nvdla_compute_task_id(task->task_desc->sequence,
				task->task_desc->queue_id);
		task_complete = nvhost_syncpt_is_expired_ext(pdev,
					queue->syncpt_id, task->fence);

		/* clean task and remove from list */
		if (task_complete) {
			nvdla_dbg_fn(pdev, "task with syncpt[%d] val[%d] done",
				queue->syncpt_id, task->fence);

			tsp_notifier = (struct nvhost_notification *)
					((uint8_t *)task->task_desc +
					nvdla_profile_status_offset(task));
			timestamp_ptr = (u64 *) &tsp_notifier->time_stamp;
			/* Report timestamps in TSC ticks, so divide by 32 */
			timestamp_end = *timestamp_ptr >> 5;
			timestamp_start = (*timestamp_ptr -
					(tsp_notifier->info32 * 1000)) >> 5;

		if (IS_ENABLED(CONFIG_TRACING)) {
			trace_job_timestamps(task_id, timestamp_start, timestamp_end);

			/* Record task postfences */
			for (i = 0; i < task->num_postfences; i++) {
				trace_job_postfence(task_id, task->postfences[i].syncpoint_index,
						task->postfences[i].syncpoint_value);
			}
		}
			nvdla_task_free_locked(task);
			n_tasks_completed++;
		}
	}

	/* put pm refcount */
	nvhost_module_idle_mult(pdev, n_tasks_completed);

	mutex_unlock(&queue->list_lock);
}

static size_t nvdla_get_task_desc_size(void)
{
	size_t size = 0;

	/* calculate size of task desc, actions and its list, buffers
	 * this is max possible size for updating task desc and
	 * and allocated mem size can be more than required size
	 */
	size += sizeof(struct dla_task_descriptor);
	size += (2 * MAX_NUM_ACTION_LIST * sizeof(struct dla_action_list));
	size += nvdla_get_max_preaction_size();
	size += nvdla_get_max_postaction_size();

	/* align addresslist to 256 */
	size = roundup(size, 256);
	size += MAX_NUM_NVDLA_BUFFERS_PER_TASK * sizeof(struct dla_mem_addr);

	/* this also, ensure that, addresslist size aligned to 256 */
	size = roundup(size, 256);
	size += sizeof(struct nvhost_notification);

	/* falcon requires IOVA addr to be 256 aligned */
	size = roundup(size, SZ_256);

	return size;
}

static void nvdla_get_task_desc_memsize_op(size_t *dma_size, size_t *kmem_size)
{
	*dma_size = nvdla_get_task_desc_size();
	*kmem_size = nvdla_get_max_task_size();
}

static inline u8 *add_address(u8 *mem, uint64_t addr)
{
	struct dla_mem_addr *address = (struct dla_mem_addr *)mem;

	address->val = addr;

	return mem + sizeof(struct dla_mem_addr);
}

static inline u8 *add_opcode(u8 *mem, uint8_t op)
{
	struct dla_action_opcode *opcode = (struct dla_action_opcode *)mem;

	opcode->value = op;

	return mem + sizeof(struct dla_action_opcode);
}

static u8 *add_fence_action(u8 *mem, uint8_t op, uint64_t addr, uint32_t val)
{
	struct dla_action_semaphore *action;

	mem = add_opcode(mem, op);

	action = (struct dla_action_semaphore *)mem;
	action->address = addr;
	action->value = val;

	return mem + sizeof(struct dla_action_semaphore);
}

static u8 *add_status_action(u8 *mem, uint8_t op, uint64_t addr,
				uint16_t status)
{
	struct dla_action_task_status *action;

	mem = add_opcode(mem, op);

	action = (struct dla_action_task_status *)mem;
	action->address = addr;
	action->status = status;

	return mem + sizeof(struct dla_action_task_status);
}

static u8 *add_timestamp_action(u8 *mem, uint8_t op, uint64_t addr)
{
	struct dla_action_timestamp *action;

	mem = add_opcode(mem, op);

	action = (struct dla_action_timestamp *)mem;
	action->address = addr;

	return mem + sizeof(struct dla_action_timestamp);
}

static int nvdla_add_fence_action_cb(struct nvhost_ctrl_sync_fence_info info, void *data)
{
	u32 id, thresh;
	struct nvdla_add_fence_action_cb_args *args = data;
	struct nvdla_queue *queue = args->queue;
	u8 **next = args->mem;
	struct platform_device *pdev = queue->pool->pdev;
	dma_addr_t syncpt_addr;

	id = info.id;
	thresh = info.thresh;

	if (!id || !nvhost_syncpt_is_valid_pt_ext(pdev, id)) {
		nvdla_dbg_err(pdev, "Invalid sync_fd");
		return -EINVAL;
	}

	syncpt_addr = nvhost_syncpt_address(
			queue->vm_pdev, id);
	nvdla_dbg_info(pdev, "syncfd_pt:[%u]"
		"mss_dma_addr[%pad]",
		id, &syncpt_addr);
	*next = add_fence_action(*next, ACTION_SEM_GE,
			syncpt_addr, thresh);

	return 0;
}

static int nvdla_map_task_memory(struct nvdla_task *task)
{
	int jj;
	int err = 0;
	size_t offset;
	struct nvdla_buffers *buffers = task->buffers;
	struct platform_device *pdev = task->queue->pool->pdev;
	struct dla_task_descriptor *task_desc = task->task_desc;
	u8 *next;

	nvdla_dbg_fn(pdev, "");

	/* get address list offset */
	offset = task_desc->postactions +
	   sizeof(struct dla_action_list) + nvdla_get_max_preaction_size() +
	   sizeof(struct dla_action_list) + nvdla_get_max_postaction_size();
	offset = roundup(offset, 256);
	nvdla_dbg_fn(pdev, "addresslist offset is[%zu]", offset);

	/* get task desc address list to update list from kernel */
	next = (u8 *)task_desc + offset;

	/* send address lists task desc dma to engine */
	task_desc->address_list = (uint64_t)((u8 *)task->task_desc_pa + offset);
	task_desc->num_addresses = task->num_addresses;

	/* update address list with all dma */
	for (jj = 0; jj < task->num_addresses; jj++) {
		dma_addr_t dma_addr;
		size_t dma_size;

		nvdla_dbg_info(pdev, "count[%d] handle[%u] offset[%u]",
				jj,
				task->memory_handles[jj].handle,
				task->memory_handles[jj].offset);

		if (task->memory_handles[jj].type ==
				NVDLA_BUFFER_TYPE_INTERNAL) {
			/* For internal buffers, offset is the final address */
			next = add_address(next,
					task->memory_handles[jj].offset);
			continue;
		}

		if (!task->memory_handles[jj].handle) {
			err = -EFAULT;
			goto fail_to_pin_mem;
		}

		err = nvdla_buffer_submit_pin(buffers,
				&task->memory_handles[jj].handle,
				1, &dma_addr, &dma_size, NULL);
		if (err) {
			nvdla_dbg_err(pdev, "fail to pin address list");
			goto fail_to_pin_mem;
		}
		next = add_address(next,
			dma_addr + task->memory_handles[jj].offset);
	}
	spec_bar(); /* break_spec_p#5_1 */

fail_to_pin_mem:
	return err;
}

static int nvdla_fill_wait_fence_action(struct nvdla_task *task,
	struct nvdev_fence *fence,
	struct dma_buf **dma_buf,
	u8 **mem_next
)
{
	int err = 0;

	struct nvdla_buffers *buffers = task->buffers;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;
	u8 *next = *mem_next;

	switch(fence->type) {
	case NVDEV_FENCE_TYPE_SYNC_FD: {
		struct nvhost_fence *f;
		struct nvdla_add_fence_action_cb_args args;

		f = nvhost_fence_get(fence->sync_fd);
		if (!f) {
			nvdla_dbg_err(pdev, "failed to get sync fd");
			break;
		}

		args.queue = queue;
		args.mem = &next;
		err = nvhost_fence_foreach_pt(f, nvdla_add_fence_action_cb, &args);
		if (err != 0) {
			nvhost_fence_put(f);
		}

		break;
	}
	case NVDEV_FENCE_TYPE_SYNCPT: {

		dma_addr_t syncpt_addr;
		nvdla_dbg_info(pdev, "id[%d] val[%d]",
				fence->syncpoint_index,
				fence->syncpoint_value);

		syncpt_addr = nvhost_syncpt_address(
			queue->vm_pdev, fence->syncpoint_index);
		nvdla_dbg_info(pdev, "syncpt:[%u] dma_addr[%pad]",
			fence->syncpoint_index, &syncpt_addr);

		next = add_fence_action(next, ACTION_SEM_GE,
				syncpt_addr, fence->syncpoint_value);

		break;
	}
	case NVDEV_FENCE_TYPE_SEMAPHORE:
	case NVDEV_FENCE_TYPE_SEMAPHORE_TS: {
		dma_addr_t dma_addr;
		size_t dma_size;

		nvdla_dbg_info(pdev, "semh[%u] semo[%u] val[%d]",
				fence->semaphore_handle,
				fence->semaphore_offset,
				fence->semaphore_value);

		if (nvdla_buffer_submit_pin(buffers,
				&fence->semaphore_handle, 1, &dma_addr, &dma_size, NULL)) {
			nvdla_dbg_err(pdev, "fail to pin WAIT SEM");
			break;
		}

		next = add_fence_action(next, ACTION_SEM_GE,
			dma_addr + fence->semaphore_offset,
			fence->semaphore_value);
		break;
	}
	default:
		nvdla_dbg_err(pdev, "Invalid sync_type[%d]", fence->type);
		err = -EINVAL;
		goto fail;
	}

	*mem_next = next;

fail:
	return err;
}

static int nvdla_fill_signal_fence_action(struct nvdla_task *task,
	struct nvdev_fence *fence,
	struct dma_buf **dma_buf,
	u8 **mem_next)
{
	int err = 0;

	struct nvdla_buffers *buffers = task->buffers;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;
	u8 *next = *mem_next;

	switch (fence->type) {
	case NVDEV_FENCE_TYPE_SYNC_FD:
	case NVDEV_FENCE_TYPE_SYNCPT: {
		dma_addr_t syncpt_addr;

		/* For postaction also update MSS addr */
		syncpt_addr = nvhost_syncpt_address(queue->vm_pdev,
						queue->syncpt_id);
		next = add_fence_action(next, ACTION_WRITE_SEM,
				syncpt_addr, 1);

		task->fence_counter = task->fence_counter + 1;

		nvdla_dbg_info(pdev, "syncpt:[%u] mss:[%pad]",
				queue->syncpt_id, &syncpt_addr);
		break;
	}
	case NVDEV_FENCE_TYPE_SEMAPHORE: {
		dma_addr_t dma_addr;
		size_t dma_size;

		nvdla_dbg_info(pdev, "semh:%u semo:%u v:%d",
				fence->semaphore_handle,
				fence->semaphore_offset,
				fence->semaphore_value);

		if (nvdla_buffer_submit_pin(buffers,
				&fence->semaphore_handle, 1, &dma_addr, &dma_size, NULL)) {
			nvdla_dbg_err(pdev, "fail to pin SIGNAL SEM");
			break;
		}

		if (fence->action == NVDEV_FENCE_SIGNAL_STRIDE) {
			next = add_fence_action(next, ACTION_INCREMENT_SEM,
				dma_addr + fence->semaphore_offset,
				fence->semaphore_value);
		} else {
			next = add_fence_action(next, ACTION_WRITE_SEM,
				dma_addr + fence->semaphore_offset,
				fence->semaphore_value);
		}
		break;
	}
	case NVDEV_FENCE_TYPE_SEMAPHORE_TS: {
		dma_addr_t dma_addr;
		size_t dma_size;

		nvdla_dbg_info(pdev, "semh:%u semo:%u v:%d",
				fence->semaphore_handle,
				fence->semaphore_offset,
				fence->semaphore_value);

		if (nvdla_buffer_submit_pin(buffers,
				&fence->semaphore_handle, 1, &dma_addr, &dma_size, NULL)) {
			nvdla_dbg_err(pdev, "fail to pin SIGNAL SEM");
			break;
		}

		next = add_fence_action(next, ACTION_WRITE_TS_SEM,
			dma_addr + fence->semaphore_offset,
			fence->semaphore_value);
		break;

	}
	default:
		nvdla_dbg_err(pdev, "Invalid sync_type[%d]",
			fence->type);
		err = -EINVAL;
		goto fail;
	}

	*mem_next = next;

fail:
	return err;
}

static int nvdla_fill_taskstatus_read_action(struct nvdla_task *task,
	struct nvdla_status_notify *task_status,
	struct dma_buf **dma_buf,
	u8 **mem_next)
{
	int err = 0;

	struct nvdla_buffers *buffers = task->buffers;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;
	dma_addr_t dma_addr;
	size_t dma_size;

	u8 *next = *mem_next;

	nvdla_dbg_info(pdev, "h[%u] o[%u] status[%d]",
			task_status->handle,
			task_status->offset,
			task_status->status);

	if (nvdla_buffer_submit_pin(buffers,
			&task_status->handle, 1, &dma_addr, &dma_size, NULL)) {
		nvdla_dbg_err(pdev, "fail to pin in status");
		err = -EINVAL;
		goto fail;
	}

	next = add_status_action(next, ACTION_TASK_STATUS_EQ,
			dma_addr + task_status->offset,
			task_status->status);

	*mem_next = next;

fail:
	return err;
}

static int nvdla_fill_taskstatus_write_action(struct nvdla_task *task,
	struct nvdla_status_notify *task_status,
	struct dma_buf **dma_buf,
	u8 **mem_next)
{
	int err = 0;

	struct nvdla_buffers *buffers = task->buffers;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;
	dma_addr_t dma_addr;
	size_t dma_size;

	u8 *next = *mem_next;

	nvdla_dbg_info(pdev, "h[%u] o[%u] status[%d]",
			task_status->handle,
			task_status->offset,
			task_status->status);

	if (nvdla_buffer_submit_pin(buffers,
			&task_status->handle, 1, &dma_addr, &dma_size, NULL)) {
		nvdla_dbg_err(pdev, "fail to pin status");
		err = -EINVAL;
		goto fail;
	}

	next = add_status_action(next, ACTION_WRITE_TASK_STATUS,
			dma_addr + task_status->offset,
			task_status->status);

	*mem_next = next;

fail:
	return err;
}

static int nvdla_fill_timestamp_write_action(struct nvdla_task *task,
	struct nvdla_mem_handle *timestamp,
	struct dma_buf **dma_buf,
	u8 **mem_next)
{
	int err = 0;

	struct nvdla_buffers *buffers = task->buffers;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;
	dma_addr_t dma_addr;
	size_t dma_size;

	u8 *next = *mem_next;

	nvdla_dbg_info(pdev, "h[%u] o[%u]",
			timestamp->handle,
			timestamp->offset);

	if (nvdla_buffer_submit_pin(buffers,
			&timestamp->handle, 1, &dma_addr, &dma_size, NULL)) {
		nvdla_dbg_err(pdev, "fail to pin timestamp");
		err = -EINVAL;
		goto fail;
	}

	next = add_timestamp_action(next, ACTION_WRITE_TIMESTAMP,
			dma_addr + timestamp->offset);

	*mem_next = next;

fail:
	return err;
}


static int nvdla_fill_postactions(struct nvdla_task *task)
{
	int err = 0;

	struct dla_task_descriptor *task_desc = task->task_desc;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;
	struct dla_action_list *postactionl;
	uint16_t postactionlist_of;
	u8 *next, *start;
	void *mem;
	int i;

	/* update postaction list offset */
	postactionlist_of = task_desc->postactions +
		sizeof(struct dla_action_list) + nvdla_get_max_preaction_size();

	start = next = (u8 *)task_desc + postactionlist_of;

	/* Action to write the status notifier after task finishes (for TSP). */
	next = add_status_action(next, ACTION_WRITE_TASK_STATUS,
		task->task_desc_pa + nvdla_profile_status_offset(task), 0);

	/* fill eof timestamp actions */
	for (i = 0; i < task->num_eof_timestamps; i++) {
		err = nvdla_fill_timestamp_write_action(task,
				&task->eof_timestamps[i],
				&task->eof_timestamps_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_err(pdev,
				"failed to fill eof timestamp[%d]",
				i);
			goto fail;
		}
	}

	/* fill output task status */
	for (i = 0; i < task->num_eof_task_status; i++) {
		err = nvdla_fill_taskstatus_write_action(task,
				&task->eof_task_status[i],
				&task->eof_task_status_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_err(pdev,
				"failed to fill eof taskstatus[%d]",
				i);
			goto fail;
		}
	}

	/* fill all postactions */
	for (i = 0; i < task->num_postfences; i++) {
		/* update action */
		err = nvdla_fill_signal_fence_action(task,
				&task->postfences[i],
				&task->postfences_sem_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_info(pdev, "failed to fill postfences[%u]", i);
			goto fail;
		}
	}

	/* update end of action list */
	next = add_opcode(next, ACTION_TERMINATE);

	mem = (char *)task_desc + task_desc->postactions;
	postactionl = (struct dla_action_list *)mem;
	postactionl->offset = postactionlist_of;
	postactionl->size = next - start;

	spec_bar(); /* break_spec_p#5_1 */
fail:
	return err;
}

static int nvdla_fill_preactions(struct nvdla_task *task)
{
	int err = 0;

	struct dla_task_descriptor *task_desc = task->task_desc;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;
	struct dla_action_list *preactionl;
	uint16_t preactionlist_of;
	u8 *next, *start;
	void *mem;
	int i;

	/* preaction list offset update */
	preactionlist_of = task_desc->postactions +
					sizeof(struct dla_action_list);

	start = next = (u8 *)task_desc + preactionlist_of;

	/* fill all preactions wait */
	for (i = 0; i < task->num_prefences; i++) {
		if (task->prefences[i].action != NVDEV_FENCE_WAIT)
			continue;

		/* update action */
		err = nvdla_fill_wait_fence_action(task,
				&task->prefences[i],
				&task->prefences_sem_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_info(pdev, "failed to fill prefences[%u]", i);
			goto fail;
		}
	}

	/* fill input status after filling sem/syncpt */
	for (i = 0; i < task->num_in_task_status; i++) {
		err = nvdla_fill_taskstatus_read_action(task,
				&task->in_task_status[i],
				&task->in_task_status_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_err(pdev,
				"failed to fill in taskstatus[%d]",
				i);
			goto fail;
		}
	}

	/* fill sof task status actions */
	for (i = 0; i < task->num_sof_task_status; i++) {
		err = nvdla_fill_taskstatus_write_action(task,
				&task->sof_task_status[i],
				&task->sof_task_status_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_err(pdev,
				"failed to fill sof taskstatus[%d]",
				i);
			goto fail;
		}
	}

	/* fill sof timestamp actions */
	for (i = 0; i < task->num_sof_timestamps; i++) {
		err = nvdla_fill_timestamp_write_action(task,
				&task->sof_timestamps[i],
				&task->sof_timestamps_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_err(pdev,
				"failed to fill sof timestamp[%d]",
				i);
			goto fail;
		}
	}

	/* fill all preactions signals */
	for (i = 0; i < task->num_prefences; i++) {
		/* update action */
		if ((task->prefences[i].action != NVDEV_FENCE_SIGNAL) &&
			(task->prefences[i].action != NVDEV_FENCE_SIGNAL_STRIDE))
			continue;

		err = nvdla_fill_signal_fence_action(task,
				&task->prefences[i],
				&task->prefences_sem_dmabuf[i],
				&next);
		if (err < 0) {
			nvdla_dbg_err(pdev,
					"fail to fill fence sig action [%d]",
					i);
			goto fail;
		}
	}

	/* update end of action list */
	next = add_opcode(next, ACTION_TERMINATE);

	/* actually update lists data */
	mem = (char *)task_desc + task_desc->preactions;
	preactionl = (struct dla_action_list *)mem;
	preactionl->offset = preactionlist_of;
	preactionl->size = next - start;

	spec_bar(); /* break_spec_p#5_1 */
fail:
	return err;
}

int nvdla_fill_task_desc(struct nvdla_task *task, bool bypass_exec)
{
	int err;
	struct dla_task_descriptor *task_desc;
	struct nvdla_queue *queue = task->queue;
	struct platform_device *pdev = queue->pool->pdev;

	nvdla_dbg_fn(pdev, "");

	/* update task desc fields */
	task_desc = task->task_desc;
	task_desc->version = DLA_DESCRIPTOR_VERSION;
	task_desc->engine_id = DLA_ENGINE_ID;
	task_desc->size = nvdla_get_task_desc_size();
	task_desc->timeout = task->timeout;

	task_desc->flags = 0U;
	if (bypass_exec) {
		task_desc->flags =
			(task_desc->flags | DLA_DESC_FLAGS_BYPASS_EXEC);
	}

	/* update current task sequeue, make sure wrap around condition */
	queue->sequence = queue->sequence + 1;
	if (unlikely(queue->sequence >= (UINT_MAX - 1)))
		queue->sequence = 0;

	task_desc->sequence = queue->sequence;

	/* below are actual number of action lists
	 * DLA has one preaction list and one postaction list
	 */
	task_desc->num_preactions = MAX_NUM_ACTION_LIST;
	task_desc->num_postactions = MAX_NUM_ACTION_LIST;

	task_desc->queue_id = queue->id;

	nvdla_dbg_info(pdev, "Queue id[%d]", task_desc->queue_id);
	nvdla_dbg_info(pdev, "version[%d]", task_desc->version);
	nvdla_dbg_info(pdev, "engine_id[%d]", task_desc->engine_id);
	nvdla_dbg_info(pdev, "task desc size[%u]", task_desc->size);
	nvdla_dbg_info(pdev, "task desc sequence[%u]", task_desc->sequence);

	/* get pre/post action list HEAD mem offset
	 * - preactions list HEAD stored after dla_task_descriptor
	 * - postactions list HEAD followed after preaction list head offset
	 * - DLA has only one list of actions for each of pre and post
	 */
	task_desc->preactions = sizeof(struct dla_task_descriptor);
	task_desc->postactions = task_desc->preactions +
					sizeof(struct dla_action_list);

	/* reset fence counter */
	task->fence_counter = 0;

	/* fill pre actions */
	err = nvdla_fill_preactions(task);
	if (err != 0) {
		nvdla_dbg_err(pdev, "fail to fill preactions");
		goto fail_to_map_mem;
	}

	/* fill post actions */
	err = nvdla_fill_postactions(task);
	if (err != 0) {
		nvdla_dbg_err(pdev, "fail to fill postactions");
		goto fail_to_map_mem;
	}

	/* ping user memory before submit to engine */
	err = nvdla_map_task_memory(task);
	if (err != 0) {
		nvdla_dbg_err(pdev, "fail to pin mem");
		goto fail_to_map_mem;
	}

	nvdla_dbg_info(pdev, "task[%p] initialized", task);

	return 0;

fail_to_map_mem:
	(void) nvdla_unmap_task_memory(task);
	return err;
}

int nvdla_emulator_submit(struct nvdla_queue *queue, struct nvdla_emu_task *task)
{
	int i;
	uint32_t counter;
	struct platform_device *pdev = queue->pool->pdev;

	/* reset fence counter */
	task->fence_counter = 0;

	/* fill all preactions */
	for (i = 0; i < task->num_prefences; i++) {
		if (task->prefences[i].action != NVDEV_FENCE_SIGNAL)
			continue;

		/* update action */
		switch (task->prefences[i].type) {
		case NVDEV_FENCE_TYPE_SYNCPT:
		case NVDEV_FENCE_TYPE_SYNC_FD: {
			task->fence_counter = task->fence_counter + 1;
			break;
		}
		default:
			nvdla_dbg_err(pdev, "Invalid prefence sync type[%d]",
				task->prefences[i].type);
			return -EINVAL;
		}
	}

	/* fill all postactions */
	for (i = 0; i < task->num_postfences; i++) {
		if (task->postfences[i].action != NVDEV_FENCE_SIGNAL)
			continue;

		/* update action */
		switch (task->postfences[i].type) {
		case NVDEV_FENCE_TYPE_SYNCPT:
		case NVDEV_FENCE_TYPE_SYNC_FD: {
			task->fence_counter = task->fence_counter + 1;
			break;
		}
		default:
			nvdla_dbg_err(pdev, "Invalid postfence sync type[%d]",
				task->postfences[i].type);
			return -EINVAL;
		}
	}

	/* get fence from nvhost */
	task->fence = nvhost_syncpt_incr_max_ext(pdev, queue->syncpt_id,
						task->fence_counter);

	nvdla_dbg_fn(pdev, "syncpt[%d] fence[%d] task[%p] fence_counter[%u]",
				queue->syncpt_id, task->fence,
				task, task->fence_counter);

	/* Update signal fences for all */
	counter = task->fence_counter - 1;
	for (i = 0; i < task->num_prefences; i++) {
		if (task->prefences[i].action != NVDEV_FENCE_SIGNAL)
			continue;

		if ((task->prefences[i].type == NVDEV_FENCE_TYPE_SYNCPT) ||
			(task->prefences[i].type == NVDEV_FENCE_TYPE_SYNC_FD)) {
			task->prefences[i].syncpoint_index =
					queue->syncpt_id;
			task->prefences[i].syncpoint_value =
					task->fence - counter;

			nvdla_dbg_info(pdev, "[%d] prefence set[%u]:[%u]",
				i, task->prefences[i].syncpoint_index,
				task->prefences[i].syncpoint_value);

			counter = counter - 1;
		}
	}

	for (i = 0; i < task->num_postfences; i++) {
		if (task->postfences[i].action != NVDEV_FENCE_SIGNAL)
			continue;

		if ((task->postfences[i].type == NVDEV_FENCE_TYPE_SYNCPT) ||
			(task->postfences[i].type == NVDEV_FENCE_TYPE_SYNC_FD)) {
			task->postfences[i].syncpoint_index =
					queue->syncpt_id;
			task->postfences[i].syncpoint_value =
					task->fence - counter;

			nvdla_dbg_info(pdev, "[%d] postfence set[%u]:[%u]",
				i, task->postfences[i].syncpoint_index,
				task->postfences[i].syncpoint_value);

			counter = counter - 1;
		}
	}

	spec_bar(); /* break_spec_p#5_1 */
	return 0;
}

int nvdla_get_signal_fences(struct nvdla_queue *queue, void *in_task)
{
	struct nvdla_task *task = (struct nvdla_task *)in_task;
	struct platform_device *pdev = queue->pool->pdev;
	uint32_t counter, task_fence;
	int i;

	nvdla_dbg_fn(pdev, "");

	if (task->fence_counter == 0)
		task->fence_counter = 1;

	task_fence = nvhost_syncpt_read_maxval(pdev, queue->syncpt_id) +
			task->fence_counter;

	/* Update fences signal updates for both prefence and postfence */
	counter = task->fence_counter - 1;
	for (i = 0; i < task->num_prefences; i++) {
		if (task->prefences[i].action != NVDEV_FENCE_SIGNAL)
			continue;

		if ((task->prefences[i].type == NVDEV_FENCE_TYPE_SYNCPT) ||
			(task->prefences[i].type == NVDEV_FENCE_TYPE_SYNC_FD)) {
			task->prefences[i].syncpoint_index =
					queue->syncpt_id;
			task->prefences[i].syncpoint_value =
					task_fence - counter;

			nvdla_dbg_info(pdev, "[%d] prefence set[%u]:[%u]",
				i, task->prefences[i].syncpoint_index,
				task->prefences[i].syncpoint_value);

			counter = counter - 1;
		}
	}

	for (i = 0; i < task->num_postfences; i++) {
		if (task->postfences[i].action != NVDEV_FENCE_SIGNAL)
			continue;

		if ((task->postfences[i].type == NVDEV_FENCE_TYPE_SYNCPT) ||
			(task->postfences[i].type == NVDEV_FENCE_TYPE_SYNC_FD)) {
			task->postfences[i].syncpoint_index =
					queue->syncpt_id;
			task->postfences[i].syncpoint_value =
					task_fence - counter;

			nvdla_dbg_info(pdev, "[%d] postfence set[%u]:[%u]",
				i, task->postfences[i].syncpoint_index,
				task->postfences[i].syncpoint_value);

			counter = counter - 1;
		}
	}
	spec_bar(); /* break_spec_p#5_1 */
	return 0;
}

/* Queue management API */
static int nvdla_queue_submit_op(struct nvdla_queue *queue, void *in_task)
{
	struct nvdla_task *task = (struct nvdla_task *)in_task;
	struct nvdla_task *last_task = NULL;
	struct platform_device *pdev = queue->pool->pdev;
	struct nvhost_device_data *pdata = platform_get_drvdata(pdev);
	struct nvdla_device *nvdla_dev = pdata->private_data;
	struct nvdla_cmd_data cmd_data;
	uint32_t method_data;
	uint32_t method_id;
	uint32_t task_id;
	int err = 0;
	int i;
	u64 timestamp;

	nvdla_dbg_fn(pdev, "");

	mutex_lock(&queue->list_lock);

	/* Get a reference before registration or submission */
	nvdla_task_get(task);

	task_id = nvdla_compute_task_id(task->task_desc->sequence, task->task_desc->queue_id);

	/* get fence from nvhost for MMIO mode*/
	if (nvdla_dev->submit_mode == NVDLA_SUBMIT_MODE_MMIO) {
		task->fence = nvhost_syncpt_incr_max_ext(pdev,
						queue->syncpt_id,
						task->fence_counter);
	}

	/* update last task desc's next */
	if (!list_empty(&queue->tasklist)) {
		last_task = list_last_entry(&queue->tasklist,
						struct nvdla_task, list);
		last_task->task_desc->next = (uint64_t)task->task_desc_pa;

		nvdla_dbg_info(pdev, "last task[%p] last_task_desc_pa[%llu]",
				last_task, task->task_desc_pa);
	}
	list_add_tail(&task->list, &queue->tasklist);

	nvdla_dbg_info(pdev, "task[%p] added to list", task);

	nvdla_dbg_fn(pdev, "syncpt[%d] fence[%d] task[%p] fence_counter[%u]",
				queue->syncpt_id, task->fence,
				task, task->fence_counter);

	/* enable INT_ON_COMPLETE and INT_ON_ERROR falcon interrupts */
	method_id = (DLA_CMD_SUBMIT_TASK & DLA_METHOD_ID_CMD_MASK) |
			(1 << DLA_INT_ON_COMPLETE_SHIFT) |
			(1 << DLA_INT_ON_ERROR_SHIFT);
	method_data = ALIGNED_DMA(task->task_desc_pa);

	/* Report timestamp in TSC ticks. */
	timestamp = arch_timer_read_counter();

	/* get pm refcount */
	if (nvhost_module_busy(pdev))
		goto fail_to_poweron;

	/* prepare command for channel submit */
	if (nvdla_dev->submit_mode == NVDLA_SUBMIT_MODE_CHANNEL) {

		cmd_data.method_id = method_id;
		cmd_data.method_data = method_data;
		cmd_data.wait = true;

		/* submit task to engine */
		err = nvdla_send_cmd_channel(pdev, queue, &cmd_data, task);
		if (err) {
			nvdla_dbg_err(pdev, "task[%p] submit failed", task);
			goto fail_to_channel_submit;
		}
	}

	/* register notifier with fence */
	err = nvhost_intr_register_notifier(pdev, queue->syncpt_id,
		task->fence, nvdla_queue_update, queue);
	if (err)
		goto fail_to_register;

	/* prepare command for MMIO submit */
	if (nvdla_dev->submit_mode == NVDLA_SUBMIT_MODE_MMIO) {
		cmd_data.method_id = method_id;
		cmd_data.method_data = method_data;
		cmd_data.wait = true;

		/* submit task to engine */
		err = nvdla_send_cmd(pdev, &cmd_data);
		if (err) {
			nvdla_dbg_err(pdev, "task[%p] submit failed", task);
			/* deletes invalid task from queue, puts refs */
			nvhost_syncpt_set_min_update(pdev, queue->syncpt_id,
							 task->fence);
		}
	}

	if (IS_ENABLED(CONFIG_TRACING)) {
		if (!err) {
			/* If submitted, record task submit and prefences */
			trace_job_submit(&pdev->dev, pdata->class, task_id, task->num_prefences, timestamp);

			/* Record task prefences */
			for (i = 0; i < task->num_prefences; i++) {
				trace_job_prefence(task_id, task->prefences[i].syncpoint_index,
						task->prefences[i].syncpoint_value);
			}
		}
	}

	mutex_unlock(&queue->list_lock);
	return err;

fail_to_register:
fail_to_channel_submit:
	nvhost_module_idle(pdev);
fail_to_poweron:
	nvdla_task_free_locked(task);
	mutex_unlock(&queue->list_lock);

	return err;
}

int nvdla_set_queue_state(struct nvdla_queue *queue, int cmd)
{
	struct platform_device *pdev = queue->pool->pdev;
	struct nvdla_cmd_data cmd_data;
	int err;

	nvdla_dbg_fn(pdev, "");

	if ((cmd != DLA_CMD_QUEUE_SUSPEND) &&
		(cmd != DLA_CMD_QUEUE_RESUME)) {
		nvdla_dbg_err(pdev, "invalid cmd %d", cmd);
		return -EINVAL;
	}

	/* get pm refcount */
	err = nvhost_module_busy(pdev);
	if (err) {
		nvdla_dbg_err(pdev, "failed to poweron, err: %d", err);
		goto fail_to_poweron;
	}

	/* prepare command */
	cmd_data.method_id = cmd;
	cmd_data.method_data = queue->id;
	cmd_data.wait = true;

	err = nvdla_send_cmd(pdev, &cmd_data);
	if (err) {
		nvdla_dbg_err(pdev, "failed to suspend queue %d", err);
		goto fail_to_suspend;
	}

fail_to_suspend:
	nvhost_module_idle(pdev);
fail_to_poweron:
	return err;
}

static int nvdla_queue_abort_op(struct nvdla_queue *queue)
{
	int err = 0, fence;
	struct nvdla_task *t;
	struct nvdla_cmd_data cmd_data;
	struct platform_device *pdev = queue->pool->pdev;
	int retry = NVDLA_QUEUE_ABORT_TIMEOUT / NVDLA_QUEUE_ABORT_RETRY_PERIOD;

	nvdla_dbg_fn(pdev, "");

	mutex_lock(&queue->list_lock);
	if (list_empty(&queue->tasklist))
		goto list_empty;

	/* get pm refcount */
	err = nvhost_module_busy(pdev);
	if (err) {
		nvdla_dbg_err(pdev, "failed to poweron, err: %d", err);
		goto fail_to_poweron;
	}

	/* prepare command */
	cmd_data.method_id = DLA_CMD_QUEUE_FLUSH;
	cmd_data.method_data = queue->id;
	cmd_data.wait = true;

	/* flush engine side queues */
	do {
		err = nvdla_send_cmd(pdev, &cmd_data);
		if (err == DLA_ERR_PROCESSOR_BUSY)
			mdelay(NVDLA_QUEUE_ABORT_RETRY_PERIOD);
		else
			break;
	} while (--retry);

	if (!retry || err) {
		nvdla_dbg_err(pdev,
		"Q %d abort fail. err:%d, retry:%d",
			queue->id, err, retry);
		goto done;
	}

	nvdla_dbg_info(pdev, "Engine Q[%d] flush done", queue->id);

	/* if task present free them by reset syncpoint */
	if (!list_empty(&queue->tasklist)) {
		t = list_last_entry(&queue->tasklist, struct nvdla_task, list);

		/* reset syncpoint to release all tasks */
		fence = nvhost_syncpt_read_maxval(pdev, queue->syncpt_id);
		nvhost_syncpt_set_min_update(pdev, queue->syncpt_id, fence);

		/* dump details */
		nvdla_dbg_info(pdev, "Q id %d reset syncpt[%d] done",
			queue->id, queue->syncpt_id);
	}

done:
	nvhost_module_idle(pdev);
fail_to_poweron:
list_empty:
	mutex_unlock(&queue->list_lock);
	return err;
}

struct nvdla_queue_ops nvdla_queue_ops = {
	.abort = nvdla_queue_abort_op,
	.submit = nvdla_queue_submit_op,
	.get_task_size =  nvdla_get_task_desc_memsize_op,
	.dump = nvdla_queue_dump_op,
};
