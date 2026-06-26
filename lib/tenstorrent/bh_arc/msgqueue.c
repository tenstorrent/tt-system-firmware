/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include "status_reg.h"
#include "reg.h"
#include "irqnum.h"

#define MSGHANDLER_COMPAT_MASK 0x1

#define MSG_ERROR_REPLY 0xff

/* this should probably be pulled from Devicetree */
#define POST_CODE_REG_ADDR 0x0060

/* this should probably be pulled from Devicetree */
#define APB_BASE_ADDR 0x80000000

/* this should probably be pulled from Devicetree */
#define RESET_UNIT_START_ADDR ((volatile uint32_t *)(APB_BASE_ADDR + RESET_UNIT_OFFSET_ADDR))

#define RESET_UNIT_OFFSET_ADDR            0x30000
#define RESET_UNIT_ARC_MISC_CNTL_REG_ADDR 0x80030100

typedef struct {
	uint32_t run: 4;
	uint32_t halt: 4;
	uint32_t rsvd_0: 4;
	uint32_t soft_reset: 1;
	uint32_t dbg_cache_rst: 1;
	uint32_t mbus_clkdis: 1;
	uint32_t dbus_clkdis: 1;
	uint32_t irq0_trig: 4;
	uint32_t rsvd_1: 11;
	uint32_t self_reset: 1;
} RESET_UNIT_ARC_MISC_CNTL_reg_t;

typedef union {
	uint32_t val;
	RESET_UNIT_ARC_MISC_CNTL_reg_t f;
} RESET_UNIT_ARC_MISC_CNTL_reg_u;

#define RESET_UNIT_ARC_MISC_CNTL_REG_DEFAULT (0x00000000)

#define MSI_CATCHER_BASE (APB_BASE_ADDR + 0xB0000)

/* Read/Write to FIFO pops/pushes on the FIFO. 64 entries. APB SLVERR on overflow/underflow. */
#define MSI_CATCHER_FIFO_OFFSET   0
#define MSI_CATCHER_FIFO_REG_ADDR (MSI_CATCHER_BASE + MSI_CATCHER_FIFO_OFFSET)

/* Read to flush clears the FIFO. */
#define MSI_CATCHER_FLUSH_OFFSET   4
#define MSI_CATCHER_FLUSH_REG_ADDR (MSI_CATCHER_BASE + MSI_CATCHER_FLUSH_OFFSET)

#define MSI_CATCHER_STATUS_OFFSET   8
#define MSI_CATCHER_STATUS_REG_ADDR (MSI_CATCHER_BASE + MSI_CATCHER_STATUS_OFFSET)

BUILD_ASSERT(sizeof(union request) <= (sizeof(uint32_t) * REQUEST_MSG_LEN));
typedef struct {
	uint32_t msi_ready: 1; /* [0:0] -- FIFO can accept a push. (Out of reset and not full.) */
	uint32_t unused: 7;
	uint32_t msi_intr: 1; /* [8:8] -- FIFO is nonempty */
	uint32_t msi_ovfl: 1; /* [9:9] -- FIFO above high water mark. (Full, by default.) */
} MSI_CATCHER_STATUS_reg_t;

typedef union {
	uint32_t val;
	MSI_CATCHER_STATUS_reg_t f;
} MSI_CATCHER_STATUS_reg_u;

/* Describes a single message queue. */
struct message_queue {
	struct message_queue_header header;
	union request request_queue[MSG_QUEUE_SIZE];
	struct response response_queue[MSG_QUEUE_SIZE];
};

/* All the message queues in the system. */
static struct message_queue message_queues[NUM_MSG_QUEUES];

/* All message handlers */
static void *message_handlers[CONFIG_TT_BH_ARC_NUM_MSG_CODES];

__attribute__((used)) static const uintptr_t message_queue_info[] = {
	(uintptr_t)&message_queues, MSG_QUEUE_SIZE | (NUM_MSG_QUEUES << 8), 0, 0};

static inline void *mask_voidp(void *x, uintptr_t mask)
{
	uintptr_t y = (uintptr_t)x | mask;

	return (void *)y;
}

static inline void *unmask_voidp(void *x, uintptr_t mask)
{
	uintptr_t y = (uintptr_t)x & ~mask;

	return (void *)y;
}

static union request *request_entry(struct message_queue *queue, uint32_t ptr)
{
	return &queue->request_queue[ptr % MSG_QUEUE_SIZE];
}

static struct response *response_entry(struct message_queue *queue, uint32_t ptr)
{
	return &queue->response_queue[ptr % MSG_QUEUE_SIZE];
}

int msgqueue_request_push(uint32_t msgqueue_id, const union request *request)
{
	struct message_queue *queue = &message_queues[msgqueue_id];

	if (msgqueue_id >= NUM_MSG_QUEUES) {
		return -1;
	}

	if (request == NULL) {
		return -1;
	}

	*request_entry(queue, queue->header.request_queue_wptr) = *request;
	atomic_thread_fence(memory_order_acquire);
	queue->header.request_queue_wptr += 1;
	queue->header.request_queue_wptr %= MSG_QUEUE_POINTER_WRAP;

	return 0;
}

int msgqueue_request_pop(uint32_t msgqueue_id, union request *request)
{
	struct message_queue *queue = &message_queues[msgqueue_id];

	if (msgqueue_id >= NUM_MSG_QUEUES) {
		return -1;
	}

	if (request == NULL) {
		return -1;
	}

	*request = *request_entry(queue, queue->header.request_queue_rptr);
	atomic_thread_fence(memory_order_seq_cst);
	queue->header.request_queue_rptr += 1;
	queue->header.request_queue_rptr %= MSG_QUEUE_POINTER_WRAP;

	return 0;
}

int msgqueue_response_push(uint32_t msgqueue_id, const struct response *response)
{
	struct message_queue *queue = &message_queues[msgqueue_id];

	if (msgqueue_id >= NUM_MSG_QUEUES) {
		return -1;
	}

	if (response == NULL) {
		return -1;
	}

	*response_entry(queue, queue->header.response_queue_wptr) = *response;
	atomic_thread_fence(memory_order_acquire);
	queue->header.response_queue_wptr += 1;
	queue->header.response_queue_wptr %= MSG_QUEUE_POINTER_WRAP;

	return 0;
}

int msgqueue_response_pop(uint32_t msgqueue_id, struct response *response)
{
	struct message_queue *queue = &message_queues[msgqueue_id];

	if (msgqueue_id >= NUM_MSG_QUEUES) {
		return -1;
	}

	if (response == NULL) {
		return -1;
	}

	*response = *response_entry(queue, queue->header.response_queue_rptr);
	atomic_thread_fence(memory_order_seq_cst);
	queue->header.response_queue_rptr += 1;
	queue->header.response_queue_rptr %= MSG_QUEUE_POINTER_WRAP;

	return 0;
}

static bool start_next_message(struct message_queue *queue, uint32_t *request_rptr_out,
			       uint32_t *response_wptr_out)
{
	/* Queue pointers are double-wrapped so equal means empty, differ by size means full. */

	uint32_t request_wptr = queue->header.request_queue_wptr;
	uint32_t request_rptr = queue->header.request_queue_rptr;

	if (request_wptr == request_rptr) {
		return false;
	}

	/* Don't accept a request unless there's a response queue slot. */
	/* We must not block and we don't want to hold onto the response. */
	uint32_t response_wptr = queue->header.response_queue_wptr;
	uint32_t response_rptr = queue->header.response_queue_rptr;

	if ((response_wptr - response_rptr) % MSG_QUEUE_POINTER_WRAP == MSG_QUEUE_SIZE) {
		return false;
	}

	if (request_wptr >= MSG_QUEUE_POINTER_WRAP || request_rptr >= MSG_QUEUE_POINTER_WRAP ||
	    response_wptr >= MSG_QUEUE_POINTER_WRAP || request_rptr >= MSG_QUEUE_POINTER_WRAP) {
		return false;
	}

	atomic_thread_fence(memory_order_acquire);

	*request_rptr_out = request_rptr;
	*response_wptr_out = response_wptr;

	return true;
}

static bool command_writes_serial(const union request *request)
{
	return request->command_code == TT_SMC_MSG_SET_LAST_SERIAL;
}

static void advance_serial(struct message_queue *queue, const union request *request)
{
	if (!command_writes_serial(request)) {
		queue->header.last_serial++; /* This just wraps. */
	}
}

/* Forward to process_l2_message. Nearly every message takes this path. */
static void process_l2_message_queue(const union request *request, struct response *response)
{
	uint32_t msg_code = request->command_code;

	if (msg_code >= CONFIG_TT_BH_ARC_NUM_MSG_CODES || message_handlers[msg_code] == NULL) {
		response->data[0] = MSG_ERROR_REPLY;
		return;
	}

	msgqueue_request_handler_t handler = message_handlers[msg_code];
	uint8_t exit_code = handler(request, response);

	response->data[0] |= exit_code;
}

/**
 * @brief Handler for host request to set the message queue serial number
 * @param[in] queue The message queue whose serial number will be updated
 * @param[in] request The request, of type @ref set_last_serial_rqst, with command code
 *                   @ref TT_SMC_MSG_SET_LAST_SERIAL. The serial number is taken from data[1].
 *
 * This function allows the host to manually set the serial number for message queue
 * synchronization purposes, bypassing the normal auto-increment behavior.
 */
static void handle_set_last_serial(struct message_queue *queue, const union request *request)
{
	queue->header.last_serial = request->data[1];
}

/** @brief Handles the test message request
 * @param[in] queue The message queue processing this request
 * @param[in] request The request, of type @ref test_rqst, with command code @ref TT_SMC_MSG_TEST
 * @param[out] response The response to the host, containing the incremented test value and serial
 */
static void handle_test(struct message_queue *queue, const union request *request,
			struct response *response)
{
	/* MSG_TYPE_TEST is a scratch-style message that we want to extend with extra info. */
	response->data[0] = 0;
	response->data[1] = request->test.test_value + 1;
	response->data[2] = queue->header.last_serial + 1;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_REPORT_SCRATCH_ONLY
 * @see report_scratch_only_rqst
 */
static void report_scratch_only_message(struct response *response)
{
	response->data[0] = MESSAGE_QUEUE_STATUS_SCRATCH_ONLY;
}

/* Run a single message. */
static void process_queued_message(struct message_queue *queue, const union request *request,
				   struct response *response)
{
	switch (request->command_code) {
	case TT_SMC_MSG_SET_LAST_SERIAL:
		handle_set_last_serial(queue, request);
		break;
	case TT_SMC_MSG_TEST:
		handle_test(queue, request, response);
		break;
	case TT_SMC_MSG_REPORT_SCRATCH_ONLY:
		report_scratch_only_message(response);
		break;
	default:
		process_l2_message_queue(request, response);
		break;
	}
}

/* Run all the outstanding messages in a single queue. */
static void process_message_queue(struct message_queue *queue)
{

	uint32_t request_rptr;
	uint32_t response_wptr;

	while (start_next_message(queue, &request_rptr, &response_wptr)) {
		union request request = (union request){0};
		struct response response = (struct response){0};

		msgqueue_request_pop(queue - message_queues, &request);
		process_queued_message(queue, &request, &response);
		msgqueue_response_push(queue - message_queues, &response);

		advance_serial(queue, &request);
	}
}

void clear_msg_irq(void)
{
#ifdef CONFIG_BOARD_TT_BLACKHOLE
	RESET_UNIT_ARC_MISC_CNTL_reg_u arc_misc_cntl = {
		.val = ReadReg(RESET_UNIT_ARC_MISC_CNTL_REG_ADDR)};
	arc_misc_cntl.f.irq0_trig = 0;
	WriteReg(RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, arc_misc_cntl.val);
#endif
}

/* Run all messages in all queues. */
void process_message_queues(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_MSG_HANDLE_START);
	for (unsigned int i = 0; i < NUM_MSG_QUEUES; i++) {
		SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARG_MSG_QUEUE_START + i);
		process_message_queue(&message_queues[i]);
	}
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_MSG_HANDLE_DONE);
}

void msgqueue_register_handler(uint32_t msg_code, msgqueue_request_handler_t handler)
{
	if (msg_code >= CONFIG_TT_BH_ARC_NUM_MSG_CODES) {
		return;
	}

	message_handlers[msg_code] = handler;
}

static void prepare_msg_queue(void)
{
	/* clear message queue headers */
	for (unsigned int i = 0; i < NUM_MSG_QUEUES; i++) {
		memset(&message_queues[i].header, 0, sizeof(message_queues[i].header));
	}

	/* populate address of message queue info */
	WriteReg(STATUS_MSG_Q_INFO_REG_ADDR, (uint32_t)message_queue_info);
}

#ifndef MSG_QUEUE_TEST
static int register_interrupt_handlers(void)
{
	STRUCT_SECTION_FOREACH(msgqueue_handler, item) {
		msgqueue_register_handler(item->msg_type, item->handler);
	}
	return 0;
}

SYS_INIT_APP(register_interrupt_handlers);
#endif

#ifdef CONFIG_BOARD_TT_BLACKHOLE
static void msgqueue_work_handler(struct k_work *work)
{
	process_message_queues();
}

static K_WORK_DEFINE(msgqueue_work, msgqueue_work_handler);

static void msgqueue_interrupt_handler(void *arg)
{
	(void)(arg);
	clear_msg_irq();
	k_work_submit(&msgqueue_work);
}

static bool msi_catcher_nonempty(void)
{
	MSI_CATCHER_STATUS_reg_u status;

	status.val = ReadReg(MSI_CATCHER_STATUS_REG_ADDR);
	return status.f.msi_intr;
}

static uint32_t msi_catcher_pop(void)
{
	return ReadReg(MSI_CATCHER_FIFO_REG_ADDR);
}

static void msi_catcher_flush(void)
{
	ReadReg(MSI_CATCHER_FLUSH_REG_ADDR);
}

static void msgqueue_msi_interrupt_handler(void *arg)
{
	(void)(arg);

	bool msi_for_msgqueue = false;

	/* MSI catcher generates SLVERR if you read from an empty FIFO. */
	while (msi_catcher_nonempty()) {
		uint32_t msi_data = msi_catcher_pop();

		if (msi_data == 0) {
			msi_for_msgqueue = true;
		}
	}

	if (msi_for_msgqueue) {
		k_work_submit(&msgqueue_work);
	}
}

static void msgqueue_msi_overflow_handler(void *arg)
{
	(void)(arg);

	msi_catcher_flush();
	k_work_submit(&msgqueue_work);
}
#endif

void init_msgqueue(void)
{
	prepare_msg_queue();

#ifdef CONFIG_BOARD_TT_BLACKHOLE
	IRQ_CONNECT(IRQNUM_ARC_MISC_CNTL_IRQ0, 0, msgqueue_interrupt_handler, NULL, 0);
	irq_enable(IRQNUM_ARC_MISC_CNTL_IRQ0);

	IRQ_CONNECT(IRQNUM_MSI_CATCHER_NONEMPTY, 0, msgqueue_msi_interrupt_handler, NULL, 0);
	irq_enable(IRQNUM_MSI_CATCHER_NONEMPTY);

	IRQ_CONNECT(IRQNUM_MSI_CATCHER_OVERFLOW, 0, msgqueue_msi_overflow_handler, NULL, 0);
	irq_enable(IRQNUM_MSI_CATCHER_OVERFLOW);

	volatile STATUS_BOOT_STATUS0_reg_u *boot_status0 =
		(volatile STATUS_BOOT_STATUS0_reg_u *)STATUS_BOOT_STATUS0_REG_ADDR;
	boot_status0->f.msg_queue_ready = 1;
#endif
}
