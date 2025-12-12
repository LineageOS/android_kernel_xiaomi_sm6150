// SPDX-License-Identifier: GPL-2.0
/*
 * Adaptive Deadline I/O Scheduler (ADIOS) v3.1.7
 * Copyright (C) 2025 Masahito Suzuki
 *
 * Backported to kernel 4.14 by Miguel (2025)
 * - Replaced scoped_guard/guard with explicit spinlock calls
 * - Replaced timer_shutdown_sync with del_timer_sync
 * - Replaced timer_container_of with container_of
 * - Adapted function signatures for kernel 4.14 APIs
 * - Removed RCU protection (using seqlock instead)
 * - Adapted elevator registration for 4.14
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/percpu.h>
#include <linux/elevator.h>
#include <linux/list_sort.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-sched.h"
#include "blk-mq-tag.h"

#define ADIOS_VERSION "3.1.7-k414"

/*
 * Request Types - 4-Tier Priority System:
 *
 * Tier 0 (Highest): Emergency & System Integrity (BLK_MQ_INSERT_AT_HEAD)
 * Tier 1 (High): I/O Barrier Guarantees (REQ_OP_FLUSH)
 * Tier 2 (Medium): Synchronous requests (application responsiveness)
 * Tier 3 (Normal): Asynchronous requests (background throughput)
 */

/* Global latency window defaults */
static u64 default_global_latency_window            = 16000000ULL;
static u64 default_global_latency_window_rotational = 22000000ULL;
static u8  default_bq_refill_below_ratio = 20;
static u64 default_lat_model_latency_limit = 500000000ULL;
static u64 default_batch_order = 0;

/* Compliance flags */
enum adios_compliance_flags {
	ADIOS_CF_FIXORDER  = 1U << 0,
};

static u64 default_compliance_flags = 0x0;

/* Dynamic thresholds for shrinkage */
static u32 default_lm_shrink_at_kreqs  =  5000;
static u32 default_lm_shrink_at_gbytes =    50;
static u32 default_lm_shrink_resist    =     2;

enum adios_optype {
	ADIOS_READ    = 0,
	ADIOS_WRITE   = 1,
	ADIOS_DISCARD = 2,
	ADIOS_OTHER   = 3,
	ADIOS_OPTYPES = 4,
};

/* Latency targets for each operation type */
static u64 default_latency_target[ADIOS_OPTYPES] = {
	[ADIOS_READ]    =     2ULL * NSEC_PER_MSEC,
	[ADIOS_WRITE]   =  2000ULL * NSEC_PER_MSEC,
	[ADIOS_DISCARD] =  8000ULL * NSEC_PER_MSEC,
	[ADIOS_OTHER]   =     0ULL * NSEC_PER_MSEC,
};

/* Maximum batch size limits for each operation type */
static u32 default_batch_limit[ADIOS_OPTYPES] = {
	[ADIOS_READ]    = 36,
	[ADIOS_WRITE]   = 72,
	[ADIOS_DISCARD] =  1,
	[ADIOS_OTHER]   =  1,
};

enum adios_batch_order {
	ADIOS_BO_OPTYPE   = 0,
	ADIOS_BO_ELEVATOR = 1,
};

/* Latency model thresholds */
#define LM_BLOCK_SIZE_THRESHOLD 4096
#define LM_SAMPLES_THRESHOLD    1024
#define LM_INTERVAL_THRESHOLD   1500
#define LM_OUTLIER_PERCENTILE     99
#define LM_LAT_BUCKET_COUNT       64

#define ADIOS_PQ_LEVELS 2
#define ADIOS_DL_TYPES  2
#define ADIOS_BQ_PAGES  2

static u32 default_dl_prio[ADIOS_DL_TYPES] = {8, 0};

/* State flags */
enum adios_state_flags {
	ADIOS_STATE_PQ_0      = 1U << 0,
	ADIOS_STATE_PQ_1      = 1U << 1,
	ADIOS_STATE_DL_0      = 1U << 2,
	ADIOS_STATE_DL_1      = 1U << 3,
	ADIOS_STATE_BQ_PAGE_0 = 1U << 4,
	ADIOS_STATE_BQ_PAGE_1 = 1U << 5,
	ADIOS_STATE_BARRIER   = 1U << 6,
};
#define ADIOS_STATE_PQ 0
#define ADIOS_STATE_DL 2
#define ADIOS_STATE_BQ 4
#define ADIOS_STATE_BP 6

#define ADIOS_QUANTUM_SHIFT 20
#define ADIOS_MAX_INSERTS_PER_LOCK 72
#define ADIOS_MAX_DELETES_PER_LOCK 24

/* Latency bucket for small requests */
struct latency_bucket_small {
	u64 weighted_sum_latency;
	u64 sum_of_weights;
};

/* Latency bucket for large requests */
struct latency_bucket_large {
	u64 weighted_sum_latency;
	u64 weighted_sum_block_size;
	u64 sum_of_weights;
};

/* Per-cpu buckets */
struct lm_buckets {
	struct latency_bucket_small small_bucket[LM_LAT_BUCKET_COUNT];
	struct latency_bucket_large large_bucket[LM_LAT_BUCKET_COUNT];
};

/* Latency model */
struct latency_model {
	spinlock_t update_lock;
	u64 base;
	u64 slope;
	u64 small_sum_delay;
	u64 small_count;
	u64 large_sum_delay;
	u64 large_sum_bsize;
	u64 last_update_jiffies;

	struct lm_buckets __percpu *pcpu_buckets;

	u32 lm_shrink_at_kreqs;
	u32 lm_shrink_at_gbytes;
	u8  lm_shrink_resist;
};

union adios_in_flight_rqs {
	atomic64_t	atomic;
	u64			scalar;
	struct {
		u64 	count:          16;
		u64 	total_pred_lat: 48;
	};
};

/* Main scheduler data */
struct adios_data {
	spinlock_t pq_lock;
	struct list_head prio_queue[2];

	struct rb_root_cached dl_tree[2];
	spinlock_t lock;
	s64 dl_bias;
	s32 dl_prio[2];

	atomic_t state;
	u8  bq_state[ADIOS_BQ_PAGES];

	u64 global_latency_window;
	u64 compliance_flags;
	u64 latency_target[ADIOS_OPTYPES];
	u32 batch_limit[ADIOS_OPTYPES];
	u32 batch_actual_max_size[ADIOS_OPTYPES];
	u32 batch_actual_max_total;
	u32 async_depth;
	u32 lat_model_latency_limit;
	u8  bq_refill_below_ratio;
	u8  is_rotational;
	u8  batch_order;
	u8  elv_direction;
	sector_t head_pos;
	sector_t last_completed_pos;

	bool bq_page;
	struct list_head batch_queue[ADIOS_BQ_PAGES][ADIOS_OPTYPES];
	u32 batch_count[ADIOS_BQ_PAGES][ADIOS_OPTYPES];
	u8  bq_batch_order[ADIOS_BQ_PAGES];
	spinlock_t bq_lock;
	spinlock_t barrier_lock;
	struct list_head barrier_queue;

	struct lm_buckets *aggr_buckets;

	struct latency_model latency_model[ADIOS_OPTYPES];
	struct timer_list update_timer;

	union adios_in_flight_rqs in_flight_rqs;
	u64 last_completed_time;

	struct kmem_cache *rq_data_pool;
	struct kmem_cache *dl_group_pool;

	struct request_queue *queue;
};

/* Deadline group in RB tree */
struct dl_group {
	struct rb_node node;
	struct list_head rqs;
	u64 deadline;
} __aligned(64);

/* Per-request scheduler data */
struct adios_rq_data {
	struct list_head *dl_group;
	struct list_head dl_node;

	struct request *rq;
	u64 deadline;
	u64 pred_lat;
	u32 block_size;
	bool managed;
} __aligned(64);

static const int adios_prio_to_wmult[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

static inline bool compliant(struct adios_data *ad, u32 flag)
{
	return ad->compliance_flags & flag;
}

/* Count entries in aggregated small buckets */
static u64 lm_count_small_entries(struct latency_bucket_small *buckets)
{
	u64 total_weight = 0;
	u8 i;

	for (i = 0; i < LM_LAT_BUCKET_COUNT; i++)
		total_weight += buckets[i].sum_of_weights;
	return total_weight;
}

/* Update small buckets in the latency model */
static bool lm_update_small_buckets(struct latency_model *model,
		struct latency_bucket_small *buckets,
		u64 total_weight, bool count_all)
{
	u64 sum_latency = 0;
	u64 sum_weight = 0;
	u64 cumulative_weight = 0, threshold_weight = 0;
	u8  outlier_threshold_bucket = 0;
	u8  outlier_percentile = LM_OUTLIER_PERCENTILE;
	u8  reduction;
	u8  i;

	if (count_all)
		outlier_percentile = 100;

	threshold_weight = (total_weight * outlier_percentile) / 100;

	for (i = 0; i < LM_LAT_BUCKET_COUNT; i++) {
		cumulative_weight += buckets[i].sum_of_weights;
		if (cumulative_weight >= threshold_weight) {
			outlier_threshold_bucket = i;
			break;
		}
	}

	for (i = 0; i <= outlier_threshold_bucket; i++) {
		struct latency_bucket_small *bucket = &buckets[i];
		if (i < outlier_threshold_bucket) {
			sum_latency += bucket->weighted_sum_latency;
			sum_weight += bucket->sum_of_weights;
		} else {
			u64 remaining_weight =
				threshold_weight - (cumulative_weight - bucket->sum_of_weights);
			if (bucket->sum_of_weights > 0) {
				sum_latency += div_u64(bucket->weighted_sum_latency *
					remaining_weight, bucket->sum_of_weights);
				sum_weight += remaining_weight;
			}
		}
	}

	if (model->small_count >= 1000ULL * model->lm_shrink_at_kreqs) {
		reduction = model->lm_shrink_resist;
		if (model->small_count >> reduction) {
			model->small_sum_delay -= model->small_sum_delay >> reduction;
			model->small_count     -= model->small_count     >> reduction;
		}
	}

	if (!sum_weight)
		return false;

	model->small_sum_delay += sum_latency;
	model->small_count     += sum_weight;

	return true;
}

/* Count entries in aggregated large buckets */
static u64 lm_count_large_entries(struct latency_bucket_large *buckets)
{
	u64 total_weight = 0;
	u8 i;

	for (i = 0; i < LM_LAT_BUCKET_COUNT; i++)
		total_weight += buckets[i].sum_of_weights;
	return total_weight;
}

/* Update large buckets in the latency model */
static bool lm_update_large_buckets(struct latency_model *model,
		struct latency_bucket_large *buckets,
		u64 total_weight, bool count_all)
{
	s64 sum_latency = 0;
	u64 sum_block_size = 0, intercept;
	u64 cumulative_weight = 0, threshold_weight = 0;
	u64 sum_weight = 0;
	u8  outlier_threshold_bucket = 0;
	u8  outlier_percentile = LM_OUTLIER_PERCENTILE;
	u8  reduction;
	u8  i;

	if (count_all)
		outlier_percentile = 100;

	threshold_weight = (total_weight * outlier_percentile) / 100;

	for (i = 0; i < LM_LAT_BUCKET_COUNT; i++) {
		cumulative_weight += buckets[i].sum_of_weights;
		if (cumulative_weight >= threshold_weight) {
			outlier_threshold_bucket = i;
			break;
		}
	}

	for (i = 0; i <= outlier_threshold_bucket; i++) {
		struct latency_bucket_large *bucket = &buckets[i];
		if (i < outlier_threshold_bucket) {
			sum_latency += bucket->weighted_sum_latency;
			sum_block_size += bucket->weighted_sum_block_size;
			sum_weight += bucket->sum_of_weights;
		} else {
			u64 remaining_weight =
				threshold_weight - (cumulative_weight - bucket->sum_of_weights);
			if (bucket->sum_of_weights > 0) {
				sum_latency += div_u64(bucket->weighted_sum_latency *
					remaining_weight, bucket->sum_of_weights);
				sum_block_size += div_u64(bucket->weighted_sum_block_size *
					remaining_weight, bucket->sum_of_weights);
				sum_weight += remaining_weight;
			}
		}
	}

	if (!sum_weight)
		return false;

	if (model->large_sum_bsize >= 0x40000000ULL * model->lm_shrink_at_gbytes) {
		reduction = model->lm_shrink_resist;
		if (model->large_sum_bsize >> reduction) {
			model->large_sum_delay -= model->large_sum_delay >> reduction;
			model->large_sum_bsize -= model->large_sum_bsize >> reduction;
		}
	}

	intercept = model->base;
	if (sum_latency > (s64)intercept)
		sum_latency -= intercept;

	model->large_sum_delay += sum_latency;
	model->large_sum_bsize += sum_block_size;

	return true;
}

static void reset_buckets(struct lm_buckets *buckets)
{
	memset(buckets, 0, sizeof(*buckets));
}

static void lm_reset_pcpu_buckets(struct latency_model *model)
{
	int cpu;

	for_each_possible_cpu(cpu)
		reset_buckets(per_cpu_ptr(model->pcpu_buckets, cpu));
}

/* Update latency model parameters */
static void latency_model_update(struct adios_data *ad, struct latency_model *model)
{
	u64 now;
	u64 small_weight, large_weight;
	bool time_elapsed;
	bool small_processed = false, large_processed = false;
	struct lm_buckets *aggr = ad->aggr_buckets;
	struct latency_bucket_small *asb;
	struct latency_bucket_large *alb;
	struct lm_buckets *pcpu_b;
	unsigned long flags;
	int cpu;
	u8 i;

	spin_lock_irqsave(&model->update_lock, flags);

	/* Aggregate data from all CPUs */
	for_each_possible_cpu(cpu) {
		pcpu_b = per_cpu_ptr(model->pcpu_buckets, cpu);

		for (i = 0; i < LM_LAT_BUCKET_COUNT; i++) {
			if (pcpu_b->small_bucket[i].sum_of_weights) {
				asb = &aggr->small_bucket[i];
				asb->sum_of_weights +=
					pcpu_b->small_bucket[i].sum_of_weights;
				asb->weighted_sum_latency +=
					pcpu_b->small_bucket[i].weighted_sum_latency;
			}
			if (pcpu_b->large_bucket[i].sum_of_weights) {
				alb = &aggr->large_bucket[i];
				alb->sum_of_weights +=
					pcpu_b->large_bucket[i].sum_of_weights;
				alb->weighted_sum_latency +=
					pcpu_b->large_bucket[i].weighted_sum_latency;
				alb->weighted_sum_block_size +=
					pcpu_b->large_bucket[i].weighted_sum_block_size;
			}
		}
		reset_buckets(pcpu_b);
	}

	small_weight = lm_count_small_entries(aggr->small_bucket);
	large_weight = lm_count_large_entries(aggr->large_bucket);

	now = jiffies;
	time_elapsed = unlikely(!model->base) ||
		model->last_update_jiffies +
		msecs_to_jiffies(LM_INTERVAL_THRESHOLD) <= now;

	if (small_weight && (time_elapsed ||
			LM_SAMPLES_THRESHOLD <= small_weight || !model->base)) {
		small_processed = lm_update_small_buckets(model,
			aggr->small_bucket, small_weight, !model->base);
		memset(&aggr->small_bucket[0], 0, sizeof(aggr->small_bucket));
	}

	if (large_weight && (time_elapsed ||
			LM_SAMPLES_THRESHOLD <= large_weight || !model->slope)) {
		large_processed = lm_update_large_buckets(model,
			aggr->large_bucket, large_weight, !model->slope);
		memset(&aggr->large_bucket[0], 0, sizeof(aggr->large_bucket));
	}

	if (small_processed && likely(model->small_count))
		model->base = div_u64(model->small_sum_delay, model->small_count);

	if (large_processed && likely(model->large_sum_bsize))
		model->slope = div_u64(model->large_sum_delay,
			DIV_ROUND_UP_ULL(model->large_sum_bsize, 1024));

	if (small_processed || large_processed || time_elapsed)
		model->last_update_jiffies = now;

	spin_unlock_irqrestore(&model->update_lock, flags);
}

/* Determine bucket index for latency input */
static u8 lm_input_bucket_index(u64 measured, u64 predicted)
{
	u8 bucket_index;

	if (measured < predicted * 2)
		bucket_index = div_u64((measured * 20), predicted);
	else if (measured < predicted * 5)
		bucket_index = div_u64((measured * 10), predicted) + 20;
	else
		bucket_index = div_u64((measured * 3), predicted) + 40;

	return bucket_index;
}

/* Input latency data */
static void latency_model_input(struct adios_data *ad,
		struct latency_model *model,
		u32 block_size, u64 latency, u64 pred_lat, u32 weight)
{
	unsigned long flags;
	u8 bucket_index;
	struct lm_buckets *buckets;
	u64 current_base;

	local_irq_save(flags);
	buckets = this_cpu_ptr(model->pcpu_buckets);
	current_base = model->base;

	if (block_size <= LM_BLOCK_SIZE_THRESHOLD) {
		bucket_index = lm_input_bucket_index(latency, current_base ?: 1);

		if (bucket_index >= LM_LAT_BUCKET_COUNT)
			bucket_index = LM_LAT_BUCKET_COUNT - 1;

		buckets->small_bucket[bucket_index].sum_of_weights += weight;
		buckets->small_bucket[bucket_index].weighted_sum_latency +=
			latency * weight;

		local_irq_restore(flags);

		if (unlikely(!current_base)) {
			latency_model_update(ad, model);
			return;
		}
	} else {
		if (!current_base || !pred_lat) {
			local_irq_restore(flags);
			return;
		}

		bucket_index = lm_input_bucket_index(latency, pred_lat);

		if (bucket_index >= LM_LAT_BUCKET_COUNT)
			bucket_index = LM_LAT_BUCKET_COUNT - 1;

		buckets->large_bucket[bucket_index].sum_of_weights += weight;
		buckets->large_bucket[bucket_index].weighted_sum_latency +=
			latency * weight;
		buckets->large_bucket[bucket_index].weighted_sum_block_size +=
			block_size * weight;

		local_irq_restore(flags);
	}
}

/* Predict latency */
static u64 latency_model_predict(struct latency_model *model, u32 block_size)
{
	u64 result;

	result = model->base;
	if (block_size > LM_BLOCK_SIZE_THRESHOLD)
		result += model->slope *
			DIV_ROUND_UP_ULL(block_size - LM_BLOCK_SIZE_THRESHOLD, 1024);

	return result;
}

/* Determine operation type */
static u8 adios_optype(struct request *rq)
{
	switch (req_op(rq)) {
	case REQ_OP_READ:
		return ADIOS_READ;
	case REQ_OP_WRITE:
		return ADIOS_WRITE;
	case REQ_OP_DISCARD:
		return ADIOS_DISCARD;
	default:
		return ADIOS_OTHER;
	}
}

static inline u8 adios_optype_not_read(struct request *rq)
{
	return req_op(rq) != REQ_OP_READ;
}

static inline struct adios_rq_data *get_rq_data(struct request *rq)
{
	return rq->elv.priv[0];
}

static inline void set_adios_state(struct adios_data *ad, u32 shift, u32 idx, bool flag)
{
	if (flag)
		atomic_or(1U << (idx + shift), &ad->state);
	else
		atomic_andnot(1U << (idx + shift), &ad->state);
}

static inline u32 get_adios_state(struct adios_data *ad)
{
	return atomic_read(&ad->state);
}

static inline u32 eval_this_adios_state(u32 state, u32 shift)
{
	return (state >> shift) & 0x3;
}

static inline u32 eval_adios_state(struct adios_data *ad, u32 shift)
{
	return eval_this_adios_state(get_adios_state(ad), shift);
}

/* Add request to deadline tree */
static void add_to_dl_tree(struct adios_data *ad, bool dl_idx, struct request *rq)
{
	struct rb_root_cached *root = &ad->dl_tree[dl_idx];
	struct rb_node **link = &(root->rb_root.rb_node), *parent = NULL;
	bool leftmost = true;
	struct adios_rq_data *rd = get_rq_data(rq);
	struct dl_group *dlg;
	u64 deadline;
	bool was_empty = RB_EMPTY_ROOT(&root->rb_root);
	s64 diff;

	/* Tier-2: Synchronous - FIFO within same optype */
	rd->deadline = ktime_get_ns();

	/* Tier-3: Asynchronous - can be reordered */
	if (!(rq->cmd_flags & REQ_SYNC)) {
		rd->deadline += ad->latency_target[adios_optype(rq)];
		if (!compliant(ad, ADIOS_CF_FIXORDER))
			rd->deadline += rd->pred_lat;
	}

	deadline = rd->deadline & ~((1ULL << ADIOS_QUANTUM_SHIFT) - 1);

	while (*link) {
		dlg = rb_entry(*link, struct dl_group, node);
		diff = deadline - dlg->deadline;

		parent = *link;
		if (diff < 0) {
			link = &((*link)->rb_left);
		} else if (diff > 0) {
			link = &((*link)->rb_right);
			leftmost = false;
		} else {
			goto found;
		}
	}

	dlg = rb_entry_safe(parent, struct dl_group, node);
	if (!dlg || dlg->deadline != deadline) {
		dlg = kmem_cache_zalloc(ad->dl_group_pool, GFP_ATOMIC);
		if (!dlg)
			return;
		dlg->deadline = deadline;
		INIT_LIST_HEAD(&dlg->rqs);
		rb_link_node(&dlg->node, parent, link);
		rb_insert_color_cached(&dlg->node, root, leftmost);
	}
found:
	list_add_tail(&rd->dl_node, &dlg->rqs);
	rd->dl_group = &dlg->rqs;

	if (was_empty)
		set_adios_state(ad, ADIOS_STATE_DL, dl_idx, true);
}

/* Remove request from deadline tree */
static void del_from_dl_tree(struct adios_data *ad, bool dl_idx, struct request *rq)
{
	struct rb_root_cached *root = &ad->dl_tree[dl_idx];
	struct adios_rq_data *rd = get_rq_data(rq);
	struct dl_group *dlg = container_of(rd->dl_group, struct dl_group, rqs);

	list_del_init(&rd->dl_node);
	if (list_empty(&dlg->rqs)) {
		rb_erase_cached(&dlg->node, root);
		kmem_cache_free(ad->dl_group_pool, dlg);
	}
	rd->dl_group = NULL;

	if (RB_EMPTY_ROOT(&ad->dl_tree[dl_idx].rb_root))
		set_adios_state(ad, ADIOS_STATE_DL, dl_idx, false);
}

/* Remove request from scheduler */
static void remove_request(struct adios_data *ad, struct request *rq)
{
	bool dl_idx = adios_optype_not_read(rq);
	struct request_queue *q = rq->q;
	struct adios_rq_data *rd = get_rq_data(rq);

	list_del_init(&rq->queuelist);

	if (rd->dl_group)
		del_from_dl_tree(ad, dl_idx, rq);

	elv_rqhash_del(q, rq);
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

/* Convert queue depth to word depth */
static int to_word_depth(struct blk_mq_hw_ctx *hctx, unsigned int qdepth)
{
	struct sbitmap_queue *bt = &hctx->sched_tags->bitmap_tags;
	const unsigned int nrr = hctx->queue->nr_requests;

	return ((qdepth << bt->sb.shift) + nrr - 1) / nrr;
}

/* Limit allocation depth for async/write requests */
static void adios_limit_depth(unsigned int op, struct blk_mq_alloc_data *data)
{
	struct adios_data *ad = data->q->elevator->elevator_data;

	if (op_is_sync(op) && !op_is_write(op))
		return;

	data->shallow_depth = to_word_depth(data->hctx, ad->async_depth);
}

/* Depth updated callback */
static void adios_depth_updated(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	struct blk_mq_tags *tags = hctx->sched_tags;

	ad->async_depth = q->nr_requests;
	sbitmap_queue_min_shallow_depth(&tags->bitmap_tags, 1);
}

/* Handle request merged */
static void adios_request_merged(struct request_queue *q, struct request *req,
				  enum elv_merge type)
{
	bool dl_idx = adios_optype_not_read(req);
	struct adios_data *ad = q->elevator->elevator_data;

	del_from_dl_tree(ad, dl_idx, req);
	add_to_dl_tree(ad, dl_idx, req);
}

/* Handle requests merged */
static void adios_merged_requests(struct request_queue *q, struct request *req,
				   struct request *next)
{
	struct adios_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);
	remove_request(ad, next);
}

/* Try to merge bio */
static bool adios_bio_merge(struct blk_mq_hw_ctx *hctx, struct bio *bio)
{
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	struct request *free = NULL;
	unsigned long flags;
	bool ret;

	if (eval_adios_state(ad, ADIOS_STATE_BP))
		return false;

	if (!spin_trylock_irqsave(&ad->lock, flags))
		return false;

	ret = blk_mq_sched_try_merge(q, bio, &free);
	spin_unlock_irqrestore(&ad->lock, flags);

	if (free)
		blk_mq_free_request(free);

	return ret;
}

/* Insert to priority queue */
static void insert_to_prio_queue(struct adios_data *ad, struct request *rq, bool pq_idx)
{
	struct adios_rq_data *rd = get_rq_data(rq);
	unsigned long flags;
	bool was_empty;

	if (rd->managed) {
		union adios_in_flight_rqs ifr = {
			.count          = 1,
			.total_pred_lat = rd->pred_lat,
		};
		atomic64_add(ifr.scalar, &ad->in_flight_rqs.atomic);
	}

	spin_lock_irqsave(&ad->pq_lock, flags);
	was_empty = list_empty(&ad->prio_queue[pq_idx]);
	list_add_tail(&rq->queuelist, &ad->prio_queue[pq_idx]);
	if (was_empty)
		set_adios_state(ad, ADIOS_STATE_PQ, pq_idx, true);
	spin_unlock_irqrestore(&ad->pq_lock, flags);
}

/* Insert request */
static void insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq, bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	struct adios_rq_data *rd = get_rq_data(rq);
	u8 optype = adios_optype(rq);
	bool rq_is_flush;

	rd->managed = true;
	rd->block_size = blk_rq_bytes(rq);
	rd->pred_lat = latency_model_predict(&ad->latency_model[optype], rd->block_size);

	/* Tier-0: BLK_MQ_INSERT_AT_HEAD */
	if (at_head) {
		insert_to_prio_queue(ad, rq, 0);
		return;
	}

	/* Barrier handling for REQ_OP_FLUSH */
	rq_is_flush = rq->cmd_flags & REQ_PREFLUSH;
	if (eval_adios_state(ad, ADIOS_STATE_BP) || rq_is_flush) {
		unsigned long flags;

		spin_lock_irqsave(&ad->barrier_lock, flags);
		if (rq_is_flush)
			set_adios_state(ad, ADIOS_STATE_BP, 0, true);
		list_add_tail(&rq->queuelist, &ad->barrier_queue);
		spin_unlock_irqrestore(&ad->barrier_lock, flags);
		return;
	}

	if (blk_mq_sched_try_insert_merge(q, rq))
		return;

	add_to_dl_tree(ad, adios_optype_not_read(rq), rq);

	if (rq_mergeable(rq)) {
		elv_rqhash_add(q, rq);
		if (!q->last_merge)
			q->last_merge = rq;
	}
}

/* Insert multiple requests */
static void adios_insert_requests(struct blk_mq_hw_ctx *hctx,
				   struct list_head *list, bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	struct request *rq;
	unsigned long flags;
	bool stop = false;
	int i;

	do {
		spin_lock_irqsave(&ad->lock, flags);
		for (i = 0; i < ADIOS_MAX_INSERTS_PER_LOCK; i++) {
			if (list_empty(list)) {
				stop = true;
				break;
			}
			rq = list_first_entry(list, struct request, queuelist);
			list_del_init(&rq->queuelist);
			insert_request(hctx, rq, at_head);
		}
		spin_unlock_irqrestore(&ad->lock, flags);
	} while (!stop);
}

/* Prepare request */
static void adios_prepare_request(struct request *rq, struct bio *bio)
{
	struct adios_data *ad = rq->q->elevator->elevator_data;
	struct adios_rq_data *rd;

	rq->elv.priv[0] = NULL;

	rd = kmem_cache_zalloc(ad->rq_data_pool, GFP_ATOMIC);
	if (WARN(!rd, "adios: Failed to allocate rq_data\n"))
		return;

	rd->rq = rq;
	rq->elv.priv[0] = rd;
}

static struct adios_rq_data *get_dl_first_rd(struct adios_data *ad, bool idx)
{
	struct rb_root_cached *root = &ad->dl_tree[idx];
	struct rb_node *first = rb_first_cached(root);
	struct dl_group *dl_group = rb_entry(first, struct dl_group, node);

	return list_first_entry(&dl_group->rqs, struct adios_rq_data, dl_node);
}

/* Compare requests by position */
static int cmp_rq_pos(void *priv, struct list_head *a, struct list_head *b)
{
	struct request *rq_a = list_entry(a, struct request, queuelist);
	struct request *rq_b = list_entry(b, struct request, queuelist);
	u64 pos_a = blk_rq_pos(rq_a);
	u64 pos_b = blk_rq_pos(rq_b);

	return (int)(pos_a > pos_b) - (int)(pos_a < pos_b);
}

/* Update elevator direction */
static void update_elv_direction(struct adios_data *ad)
{
	bool page;
	struct list_head *q;
	struct request *rq_a, *rq_b;
	u64 pos_a, pos_b, avg_rq_pos;

	if (!ad->is_rotational)
		return;

	page = ad->bq_page;
	q = &ad->batch_queue[page][1];
	if (ad->bq_batch_order[page] < ADIOS_BO_ELEVATOR || list_empty(q)) {
		ad->elv_direction = 0;
		return;
	}

	rq_a = list_first_entry(q, struct request, queuelist);
	rq_b = list_last_entry(q, struct request, queuelist);
	pos_a = blk_rq_pos(rq_a);
	pos_b = blk_rq_pos(rq_b);
	avg_rq_pos = (pos_a + pos_b) >> 1;

	ad->elv_direction = !!(ad->head_pos > avg_rq_pos);
}

/* Fill batch queues from deadline tree */
static bool fill_batch_queues(struct adios_data *ad, u64 tpl)
{
	struct adios_rq_data *rd;
	struct request *rq;
	struct list_head *dest_q;
	u8  dest_idx;
	u64 added_lat = 0;
	u32 optype_count[ADIOS_OPTYPES] = {0};
	u32 count = 0;
	u8 optype;
	bool page = !ad->bq_page, dl_idx, bias_idx, update_bias;
	u32 dl_queued;
	u8 bq_batch_order;
	bool stop = false;
	unsigned long flags;
	int i;

	memset(&ad->batch_count[page], 0, sizeof(ad->batch_count[page]));
	ad->bq_batch_order[page] = bq_batch_order = ad->batch_order;

	do {
		spin_lock_irqsave(&ad->lock, flags);
		for (i = 0; i < ADIOS_MAX_DELETES_PER_LOCK; i++) {
			bool has_base = false;

			dl_queued = eval_adios_state(ad, ADIOS_STATE_DL);
			if (!dl_queued) {
				stop = true;
				break;
			}

			dl_idx = dl_queued >> 1;
			rd = get_dl_first_rd(ad, dl_idx);

			bias_idx = ad->dl_bias < 0;
			if (dl_queued == 0x3) {
				struct adios_rq_data *trd[2] = {get_dl_first_rd(ad, 0), rd};
				rd = trd[bias_idx];
				update_bias = (trd[bias_idx]->deadline > trd[!bias_idx]->deadline);
			} else {
				update_bias = (bias_idx == dl_idx);
			}

			rq = rd->rq;
			optype = adios_optype(rq);

			has_base = !!ad->latency_model[optype].base;

			if (count && (!has_base ||
					ad->batch_count[page][optype] >= ad->batch_limit[optype] ||
					(tpl + added_lat + rd->pred_lat) > ad->global_latency_window)) {
				stop = true;
				break;
			}

			if (update_bias) {
				s64 sign = ((s64)bias_idx << 1) - 1;
				if (unlikely(!rd->pred_lat))
					ad->dl_bias = sign;
				else
					ad->dl_bias += sign * (s64)((rd->pred_lat *
						adios_prio_to_wmult[ad->dl_prio[bias_idx] + 20]) >> 10);
			}

			remove_request(ad, rq);

			dest_idx = (bq_batch_order == ADIOS_BO_OPTYPE || optype == ADIOS_OTHER)?
				optype : !!(rd->deadline != ktime_get_ns());
			dest_q = &ad->batch_queue[page][dest_idx];
			list_add_tail(&rq->queuelist, dest_q);
			ad->bq_state[page] |= 1U << dest_idx;
			ad->batch_count[page][optype]++;
			optype_count[optype]++;
			added_lat += rd->pred_lat;
			count++;
		}
		spin_unlock_irqrestore(&ad->lock, flags);
	} while (!stop);

	if (bq_batch_order == ADIOS_BO_ELEVATOR && ad->batch_count[page][1] > 1)
		list_sort(NULL, &ad->batch_queue[page][1], cmp_rq_pos);

	if (count) {
		union adios_in_flight_rqs ifr = {
			.count          = count,
			.total_pred_lat = added_lat,
		};
		atomic64_add(ifr.scalar, &ad->in_flight_rqs.atomic);

		set_adios_state(ad, ADIOS_STATE_BQ, page, true);

		for (optype = 0; optype < ADIOS_OPTYPES; optype++)
			if (ad->batch_actual_max_size[optype] < optype_count[optype])
				ad->batch_actual_max_size[optype] = optype_count[optype];
		if (ad->batch_actual_max_total < count)
			ad->batch_actual_max_total = count;
	}
	return count;
}

/* Flip batch queue page */
static void flip_bq_page(struct adios_data *ad)
{
	ad->bq_page = !ad->bq_page;
	update_elv_direction(ad);
}

/* Pop request from batch queue */
static inline struct request *pop_bq_request(struct adios_data *ad, u8 idx, bool direction)
{
	bool page = ad->bq_page;
	struct list_head *q = &ad->batch_queue[page][idx];
	struct request *rq;

	if (list_empty(q))
		return NULL;

	rq = direction ?
		list_last_entry(q, struct request, queuelist) :
		list_first_entry(q, struct request, queuelist);

	list_del_init(&rq->queuelist);
	if (list_empty(q))
		ad->bq_state[page] &= ~(1U << idx);

	return rq;
}

static struct request *pop_next_bq_request_optype(struct adios_data *ad)
{
	u32 bq_state = ad->bq_state[ad->bq_page];
	u32 bq_idx;

	if (!bq_state)
		return NULL;

	bq_idx = __ffs(bq_state);
	return pop_bq_request(ad, bq_idx, false);
}

static struct request *pop_next_bq_request_elevator(struct adios_data *ad)
{
	u32 bq_state = ad->bq_state[ad->bq_page];
	u32 bq_idx;
	bool direction;

	if (!bq_state)
		return NULL;

	bq_idx = __ffs(bq_state);
	direction = (bq_idx == 1) & ad->elv_direction;

	return pop_bq_request(ad, bq_idx, direction);
}

static inline bool bq_page_has_rq(u32 bq_state, bool page)
{
	return bq_state & (1U << page);
}

/* Dispatch from batch queue */
static struct request *dispatch_from_bq(struct adios_data *ad)
{
	struct request *rq;
	unsigned long flags;
	u32 state, bq_state, bq_curr_page_has_rq;
	union adios_in_flight_rqs ifr;
	u64 tpl;
	bool page;
	bool is_empty;

	spin_lock_irqsave(&ad->bq_lock, flags);

	state = get_adios_state(ad);
	bq_state = eval_this_adios_state(state, ADIOS_STATE_BQ);
	bq_curr_page_has_rq = bq_page_has_rq(bq_state, ad->bq_page);
	ifr.scalar = atomic64_read(&ad->in_flight_rqs.atomic);
	tpl = ifr.total_pred_lat;

	if (!bq_page_has_rq(bq_state, !ad->bq_page) &&
			(!bq_curr_page_has_rq || (!tpl || tpl < div_u64(
			ad->global_latency_window * ad->bq_refill_below_ratio, 100))) &&
			eval_this_adios_state(state, ADIOS_STATE_DL))
		fill_batch_queues(ad, tpl);

	if (!bq_curr_page_has_rq &&
			bq_page_has_rq(eval_adios_state(ad, ADIOS_STATE_BQ), !ad->bq_page))
		flip_bq_page(ad);

	rq = (ad->bq_batch_order[ad->bq_page] == ADIOS_BO_ELEVATOR) ?
		pop_next_bq_request_elevator(ad) :
		pop_next_bq_request_optype(ad);

	if (rq) {
		page = ad->bq_page;
		is_empty = !ad->bq_state[page];
		if (is_empty)
			set_adios_state(ad, ADIOS_STATE_BQ, page, false);
		spin_unlock_irqrestore(&ad->bq_lock, flags);
		return rq;
	}

	spin_unlock_irqrestore(&ad->bq_lock, flags);
	return NULL;
}

/* Dispatch from priority queue */
static struct request *dispatch_from_pq(struct adios_data *ad)
{
	struct request *rq = NULL;
	unsigned long flags;
	u32 pq_state;
	u8  pq_idx;
	struct list_head *q;

	spin_lock_irqsave(&ad->pq_lock, flags);
	pq_state = eval_adios_state(ad, ADIOS_STATE_PQ);
	pq_idx = pq_state >> 1;
	q = &ad->prio_queue[pq_idx];

	if (unlikely(list_empty(q))) {
		spin_unlock_irqrestore(&ad->pq_lock, flags);
		return NULL;
	}

	rq = list_first_entry(q, struct request, queuelist);
	list_del_init(&rq->queuelist);
	if (list_empty(q)) {
		set_adios_state(ad, ADIOS_STATE_PQ, pq_idx, false);
		update_elv_direction(ad);
	}
	spin_unlock_irqrestore(&ad->pq_lock, flags);
	return rq;
}

/* Release barrier requests */
static bool release_barrier_requests(struct adios_data *ad)
{
	u32 moved_count = 0;
	unsigned long flags;
	struct request *trq, *next;
	bool first_barrier_moved = false;
	LIST_HEAD(local_list);

	spin_lock_irqsave(&ad->barrier_lock, flags);
	if (!list_empty(&ad->barrier_queue)) {
		list_for_each_entry_safe(trq, next, &ad->barrier_queue, queuelist) {
			if (!first_barrier_moved) {
				list_del_init(&trq->queuelist);
				spin_unlock_irqrestore(&ad->barrier_lock, flags);
				insert_to_prio_queue(ad, trq, 1);
				spin_lock_irqsave(&ad->barrier_lock, flags);
				moved_count++;
				first_barrier_moved = true;
				continue;
			}

			if (trq->cmd_flags & REQ_PREFLUSH)
				break;

			list_move_tail(&trq->queuelist, &local_list);
			moved_count++;
		}

		if (list_empty(&ad->barrier_queue))
			set_adios_state(ad, ADIOS_STATE_BP, 0, false);
	}
	spin_unlock_irqrestore(&ad->barrier_lock, flags);

	if (!moved_count)
		return false;

	if (!list_empty(&local_list)) {
		unsigned long lock_flags;

		spin_lock_irqsave(&ad->lock, lock_flags);
		list_for_each_entry_safe(trq, next, &local_list, queuelist) {
			list_del_init(&trq->queuelist);
			if (!blk_mq_sched_try_insert_merge(ad->queue, trq)) {
				add_to_dl_tree(ad, adios_optype_not_read(trq), trq);
				if (rq_mergeable(trq)) {
					elv_rqhash_add(ad->queue, trq);
					if (!ad->queue->last_merge)
						ad->queue->last_merge = trq;
				}
			}
		}
		spin_unlock_irqrestore(&ad->lock, lock_flags);
	}

	return true;
}

/* Main dispatch function */
static struct request *adios_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct adios_data *ad = hctx->queue->elevator->elevator_data;
	struct request *rq;

retry:
	rq = dispatch_from_pq(ad);
	if (rq)
		goto found;

	rq = dispatch_from_bq(ad);
	if (rq)
		goto found;

	if (eval_adios_state(ad, ADIOS_STATE_BP)) {
		union adios_in_flight_rqs ifr;
		ifr.scalar = atomic64_read(&ad->in_flight_rqs.atomic);
		if (!ifr.count) {
			unsigned long flags;
			bool barrier_released = false;

			spin_lock_irqsave(&ad->lock, flags);
			barrier_released = release_barrier_requests(ad);
			spin_unlock_irqrestore(&ad->lock, flags);
			if (barrier_released)
				goto retry;
		}
	}

	return NULL;

found:
	if (ad->is_rotational)
		ad->head_pos = blk_rq_pos(rq) + blk_rq_sectors(rq);

	rq->rq_flags |= RQF_STARTED;
	return rq;
}

/* Timer callback */
static void update_timer_callback(unsigned long data)
{
	struct adios_data *ad = (struct adios_data *)data;
	u8 optype;

	for (optype = 0; optype < ADIOS_OPTYPES; optype++)
		latency_model_update(ad, &ad->latency_model[optype]);
}

/* Request completion handler */
static void adios_completed_request(struct request *rq)
{
	struct adios_data *ad = rq->q->elevator->elevator_data;
	struct adios_rq_data *rd = get_rq_data(rq);
	union adios_in_flight_rqs ifr = { .scalar = 0 };
	u64 now, lct, latency;
	u8 optype;
	u32 weight = 1;

	if (!rd)
		return;

	if (rd->managed) {
		union adios_in_flight_rqs ifr_to_sub = {
			.count          = 1,
			.total_pred_lat = rd->pred_lat,
		};
		ifr.scalar = atomic64_sub_return(ifr_to_sub.scalar, &ad->in_flight_rqs.atomic);
	}

	optype = adios_optype(rq);

	if (optype == ADIOS_OTHER) {
		if (ad->is_rotational)
			ad->last_completed_pos = 0;
		return;
	}

	now = ktime_get_ns();
	lct = ad->last_completed_time ?: now;
	ad->last_completed_time = (ifr.count) ? now : 0;

	if (!rd->block_size || unlikely(now < lct))
		return;

	latency = now - lct;
	if (latency > ad->lat_model_latency_limit)
		return;

	if (ad->is_rotational) {
		sector_t current_pos = blk_rq_pos(rq);
		if (ad->last_completed_pos > 0) {
			u64 seek_distance = abs((s64)current_pos - (s64)ad->last_completed_pos);
			weight = 65 - __fls(seek_distance);
		}
		ad->last_completed_pos = current_pos + blk_rq_sectors(rq);
	}

	latency_model_input(ad, &ad->latency_model[optype],
		rd->block_size, latency, rd->pred_lat, weight);
	mod_timer(&ad->update_timer, jiffies + msecs_to_jiffies(100));
}

/* Finish request cleanup */
static void adios_finish_request(struct request *rq)
{
	struct adios_data *ad = rq->q->elevator->elevator_data;

	if (rq->elv.priv[0]) {
		kmem_cache_free(ad->rq_data_pool, get_rq_data(rq));
		rq->elv.priv[0] = NULL;
	}
}

/* Check if work available */
static bool adios_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct adios_data *ad = hctx->queue->elevator->elevator_data;

	return atomic_read(&ad->state) != 0;
}

/* Init hctx callback */
static int adios_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	adios_depth_updated(hctx);
	return 0;
}

/* Initialize scheduler */
static int adios_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct adios_data *ad;
	struct elevator_queue *eq;
	int ret = -ENOMEM;
	u8 optype, page;
	int i;

	eq = elevator_alloc(q, e);
	if (!eq)
		return ret;

	ad = kzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad) {
		pr_err("adios: Failed to create adios_data\n");
		goto put_eq;
	}

	ad->rq_data_pool = kmem_cache_create("adios_rq_data_pool",
						sizeof(struct adios_rq_data),
						0, SLAB_HWCACHE_ALIGN, NULL);
	if (!ad->rq_data_pool) {
		pr_err("adios: Failed to create rq_data_pool\n");
		goto free_ad;
	}

	ad->dl_group_pool = kmem_cache_create("adios_dl_group_pool",
						sizeof(struct dl_group),
						0, SLAB_HWCACHE_ALIGN, NULL);
	if (!ad->dl_group_pool) {
		pr_err("adios: Failed to create dl_group_pool\n");
		goto destroy_rq_data_pool;
	}

	for (i = 0; i < ADIOS_PQ_LEVELS; i++)
		INIT_LIST_HEAD(&ad->prio_queue[i]);

	for (i = 0; i < ADIOS_DL_TYPES; i++) {
		ad->dl_tree[i] = RB_ROOT_CACHED;
		ad->dl_prio[i] = default_dl_prio[i];
	}
	ad->dl_bias = 0;

	for (page = 0; page < ADIOS_BQ_PAGES; page++)
		for (optype = 0; optype < ADIOS_OPTYPES; optype++)
			INIT_LIST_HEAD(&ad->batch_queue[page][optype]);

	ad->aggr_buckets = kzalloc(sizeof(*ad->aggr_buckets), GFP_KERNEL);
	if (!ad->aggr_buckets) {
		pr_err("adios: Failed to allocate aggregation buckets\n");
		goto destroy_dl_group_pool;
	}

	for (optype = 0; optype < ADIOS_OPTYPES; optype++) {
		struct latency_model *model = &ad->latency_model[optype];

		spin_lock_init(&model->update_lock);

		model->pcpu_buckets = alloc_percpu(struct lm_buckets);
		if (!model->pcpu_buckets) {
			pr_err("adios: Failed to allocate per-CPU buckets\n");
			goto free_buckets;
		}

		model->last_update_jiffies = jiffies;
		model->lm_shrink_at_kreqs  = default_lm_shrink_at_kreqs;
		model->lm_shrink_at_gbytes = default_lm_shrink_at_gbytes;
		model->lm_shrink_resist    = default_lm_shrink_resist;
	}

	for (optype = 0; optype < ADIOS_OPTYPES; optype++) {
		ad->latency_target[optype] = default_latency_target[optype];
		ad->batch_limit[optype] = default_batch_limit[optype];
	}

	eq->elevator_data = ad;

	ad->is_rotational = !blk_queue_nonrot(q);
	ad->global_latency_window = (ad->is_rotational) ?
		default_global_latency_window_rotational :
		default_global_latency_window;
	ad->bq_refill_below_ratio = default_bq_refill_below_ratio;
	ad->lat_model_latency_limit = default_lat_model_latency_limit;
	ad->batch_order = default_batch_order;
	ad->compliance_flags = default_compliance_flags;

	atomic_set(&ad->state, 0);

	spin_lock_init(&ad->lock);
	spin_lock_init(&ad->pq_lock);
	spin_lock_init(&ad->bq_lock);
	spin_lock_init(&ad->barrier_lock);
	INIT_LIST_HEAD(&ad->barrier_queue);

	setup_timer(&ad->update_timer, update_timer_callback, (unsigned long)ad);

	ad->queue = q;

	q->elevator = eq;
	return 0;

free_buckets:
	while (optype-- > 0) {
		struct latency_model *prev_model = &ad->latency_model[optype];
		free_percpu(prev_model->pcpu_buckets);
	}
	kfree(ad->aggr_buckets);
destroy_dl_group_pool:
	kmem_cache_destroy(ad->dl_group_pool);
destroy_rq_data_pool:
	kmem_cache_destroy(ad->rq_data_pool);
free_ad:
	kfree(ad);
put_eq:
	kobject_put(&eq->kobj);
	return ret;
}

/* Exit scheduler */
static void adios_exit_sched(struct elevator_queue *e)
{
	struct adios_data *ad = e->elevator_data;
	u8 i;

	del_timer_sync(&ad->update_timer);

	WARN_ON_ONCE(!list_empty(&ad->barrier_queue));
	for (i = 0; i < 2; i++)
		WARN_ON_ONCE(!list_empty(&ad->prio_queue[i]));

	for (i = 0; i < ADIOS_OPTYPES; i++) {
		struct latency_model *model = &ad->latency_model[i];
		free_percpu(model->pcpu_buckets);
	}

	kfree(ad->aggr_buckets);

	if (ad->rq_data_pool)
		kmem_cache_destroy(ad->rq_data_pool);

	if (ad->dl_group_pool)
		kmem_cache_destroy(ad->dl_group_pool);

	kfree(ad);
}

/* Sysfs attributes */
#define SYSFS_OPTYPE_DECL(name, optype) \
static ssize_t adios_lat_model_##name##_show( \
		struct elevator_queue *e, char *page) { \
	struct adios_data *ad = e->elevator_data; \
	struct latency_model *model = &ad->latency_model[optype]; \
	ssize_t len = 0; \
	len += sprintf(page,       "base : %llu ns\n", model->base); \
	len += sprintf(page + len, "slope: %llu ns/KiB\n", model->slope); \
	return len; \
} \
static ssize_t adios_lat_target_##name##_show( \
		struct elevator_queue *e, char *page) { \
	struct adios_data *ad = e->elevator_data; \
	return sprintf(page, "%llu\n", ad->latency_target[optype]); \
} \
static ssize_t adios_lat_target_##name##_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	struct adios_data *ad = e->elevator_data; \
	unsigned long nsec; \
	int ret; \
	ret = kstrtoul(page, 10, &nsec); \
	if (ret) \
		return ret; \
	ad->latency_model[optype].base = 0ULL; \
	ad->latency_target[optype] = nsec; \
	return count; \
} \
static ssize_t adios_batch_limit_##name##_show( \
		struct elevator_queue *e, char *page) { \
	struct adios_data *ad = e->elevator_data; \
	return sprintf(page, "%u\n", ad->batch_limit[optype]); \
} \
static ssize_t adios_batch_limit_##name##_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	unsigned long max_batch; \
	int ret; \
	struct adios_data *ad = e->elevator_data; \
	ret = kstrtoul(page, 10, &max_batch); \
	if (ret || max_batch == 0) \
		return -EINVAL; \
	ad->batch_limit[optype] = max_batch; \
	return count; \
}

SYSFS_OPTYPE_DECL(read, ADIOS_READ);
SYSFS_OPTYPE_DECL(write, ADIOS_WRITE);
SYSFS_OPTYPE_DECL(discard, ADIOS_DISCARD);

static ssize_t adios_batch_actual_max_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page,
		"Total  : %u\nDiscard: %u\nRead   : %u\nWrite  : %u\n",
		ad->batch_actual_max_total,
		ad->batch_actual_max_size[ADIOS_DISCARD],
		ad->batch_actual_max_size[ADIOS_READ],
		ad->batch_actual_max_size[ADIOS_WRITE]);
}

static ssize_t adios_global_latency_window_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%llu\n", ad->global_latency_window);
}

static ssize_t adios_global_latency_window_store(struct elevator_queue *e,
		const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	int ret;
	ret = kstrtoul(page, 10, &val);
	if (ret)
		return -EINVAL;
	ad->global_latency_window = val;
	return count;
}

static ssize_t adios_bq_refill_below_ratio_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%d\n", ad->bq_refill_below_ratio);
}

static ssize_t adios_bq_refill_below_ratio_store(struct elevator_queue *e,
		const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	int val;
	int ret;
	ret = kstrtoint(page, 10, &val);
	if (ret || val < 0 || val > 100)
		return -EINVAL;
	ad->bq_refill_below_ratio = val;
	return count;
}

static ssize_t adios_read_priority_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	return sprintf(page, "%d\n", ad->dl_prio[0]);
}

static ssize_t adios_read_priority_store(struct elevator_queue *e,
		const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long flags;
	int prio;
	int ret;

	ret = kstrtoint(page, 10, &prio);
	if (ret || prio < -20 || prio > 19)
		return -EINVAL;

	spin_lock_irqsave(&ad->lock, flags);
	ad->dl_prio[0] = prio;
	ad->dl_bias = 0;
	spin_unlock_irqrestore(&ad->lock, flags);

	return count;
}

static ssize_t adios_reset_bq_stats_store(struct elevator_queue *e,
		const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	u8 i;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	for (i = 0; i < ADIOS_OPTYPES; i++)
		ad->batch_actual_max_size[i] = 0;
	ad->batch_actual_max_total = 0;

	return count;
}

static ssize_t adios_reset_lat_model_store(struct elevator_queue *e,
		const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	u8 i;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	for (i = 0; i < ADIOS_OPTYPES; i++) {
		struct latency_model *model = &ad->latency_model[i];
		unsigned long flags;

		spin_lock_irqsave(&model->update_lock, flags);
		model->base = 0ULL;
		model->slope = 0ULL;
		model->small_sum_delay = 0ULL;
		model->small_count = 0ULL;
		model->large_sum_delay = 0ULL;
		model->large_sum_bsize = 0ULL;
		lm_reset_pcpu_buckets(model);
		spin_unlock_irqrestore(&model->update_lock, flags);
	}
	reset_buckets(ad->aggr_buckets);

	return count;
}

static ssize_t adios_version_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%s\n", ADIOS_VERSION);
}

#define AD_ATTR(name, show_func, store_func) \
	__ATTR(name, 0644, show_func, store_func)
#define AD_ATTR_RW(name) \
	__ATTR(name, 0644, adios_##name##_show, adios_##name##_store)
#define AD_ATTR_RO(name) \
	__ATTR(name, 0444, adios_##name##_show, NULL)
#define AD_ATTR_WO(name) \
	__ATTR(name, 0200, NULL, adios_##name##_store)

static struct elv_fs_entry adios_sched_attrs[] = {
	AD_ATTR_RO(batch_actual_max),
	AD_ATTR_RW(bq_refill_below_ratio),
	AD_ATTR_RW(global_latency_window),
	AD_ATTR_RW(batch_limit_read),
	AD_ATTR_RW(batch_limit_write),
	AD_ATTR_RW(batch_limit_discard),
	AD_ATTR_RO(lat_model_read),
	AD_ATTR_RO(lat_model_write),
	AD_ATTR_RO(lat_model_discard),
	AD_ATTR_RW(lat_target_read),
	AD_ATTR_RW(lat_target_write),
	AD_ATTR_RW(lat_target_discard),
	AD_ATTR_RW(read_priority),
	AD_ATTR_WO(reset_bq_stats),
	AD_ATTR_WO(reset_lat_model),
	AD_ATTR(adios_version, adios_version_show, NULL),
	__ATTR_NULL
};

static struct elevator_type mq_adios = {
	.ops.mq = {
		.init_sched		= adios_init_sched,
		.exit_sched		= adios_exit_sched,
		.init_hctx		= adios_init_hctx,
		.bio_merge		= adios_bio_merge,
		.request_merged		= adios_request_merged,
		.requests_merged	= adios_merged_requests,
		.limit_depth		= adios_limit_depth,
		.prepare_request	= adios_prepare_request,
		.finish_request		= adios_finish_request,
		.insert_requests	= adios_insert_requests,
		.dispatch_request	= adios_dispatch_request,
		.completed_request	= adios_completed_request,
		.has_work		= adios_has_work,
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
	},
	.uses_mq		= true,
	.elevator_attrs		= adios_sched_attrs,
	.elevator_name		= "adios",
	.elevator_owner		= THIS_MODULE,
};
MODULE_ALIAS("mq-adios-iosched");

#define ADIOS_PROGNAME "Adaptive Deadline I/O Scheduler"
#define ADIOS_AUTHOR   "Masahito Suzuki"

static int __init adios_init(void)
{
	pr_info("%s %s by %s (backported to 4.14)\n",
		ADIOS_PROGNAME, ADIOS_VERSION, ADIOS_AUTHOR);
	return elv_register(&mq_adios);
}

static void __exit adios_exit(void)
{
	elv_unregister(&mq_adios);
}

module_init(adios_init);
module_exit(adios_exit);

MODULE_AUTHOR(ADIOS_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(ADIOS_PROGNAME);
