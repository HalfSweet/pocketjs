// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint64_t last;
  bool exhausted;
} pocketjs_net_token_gate_t;

typedef struct {
  size_t capacity;
  size_t reserved;
  size_t queued;
  size_t delivering;
} pocketjs_net_terminal_credits_t;

typedef enum {
  POCKETJS_NET_OPERATION_FREE = 0,
  POCKETJS_NET_OPERATION_ACTIVE = 1,
  POCKETJS_NET_OPERATION_TERMINAL_QUEUED = 2,
  POCKETJS_NET_OPERATION_DELIVERING = 3,
} pocketjs_net_operation_lifecycle_t;

typedef enum {
  POCKETJS_NET_TRANSPORT_OPERATION_NONE = 0,
  POCKETJS_NET_TRANSPORT_OPERATION_RESOLVE = 1,
  POCKETJS_NET_TRANSPORT_OPERATION_CONNECT = 2,
  POCKETJS_NET_TRANSPORT_OPERATION_READ = 3,
  POCKETJS_NET_TRANSPORT_OPERATION_WRITE = 4,
  POCKETJS_NET_TRANSPORT_OPERATION_CLOSE = 5,
} pocketjs_net_transport_operation_kind_t;

typedef enum {
  POCKETJS_NET_OPERATION_ADMISSION_ACCEPTED = 0,
  POCKETJS_NET_OPERATION_ADMISSION_CLOSING,
  POCKETJS_NET_OPERATION_ADMISSION_INVALID_TOKEN,
  POCKETJS_NET_OPERATION_ADMISSION_NO_CAPACITY,
} pocketjs_net_operation_admission_t;

typedef enum {
  POCKETJS_NET_OPERATION_TERMINAL_NONE = 0,
  POCKETJS_NET_OPERATION_TERMINAL_ABORTED,
  POCKETJS_NET_OPERATION_TERMINAL_TIMED_OUT,
} pocketjs_net_operation_terminal_reason_t;

bool pocketjs_net_token_gate_can_accept(const pocketjs_net_token_gate_t *gate,
                                        uint64_t token);
void pocketjs_net_token_gate_consume(pocketjs_net_token_gate_t *gate,
                                     uint64_t token);

bool pocketjs_net_terminal_credit_reserve(
    pocketjs_net_terminal_credits_t *credits);
bool pocketjs_net_terminal_credit_enqueue(
    pocketjs_net_terminal_credits_t *credits);
bool pocketjs_net_terminal_credit_take(
    pocketjs_net_terminal_credits_t *credits);
bool pocketjs_net_terminal_credit_retire(
    pocketjs_net_terminal_credits_t *credits);
bool pocketjs_net_terminal_credit_invariant(
    const pocketjs_net_terminal_credits_t *credits);

bool pocketjs_net_operation_claim_terminal(
    pocketjs_net_operation_lifecycle_t *lifecycle);
/** Caller must serialize this with shutdown and cancellation. */
pocketjs_net_operation_admission_t
pocketjs_net_operation_admit(bool closing, pocketjs_net_token_gate_t *gate,
                             pocketjs_net_terminal_credits_t *credits,
                             pocketjs_net_operation_lifecycle_t *lifecycle,
                             uint64_t *current_token, uint64_t requested_token);
/** Caller must serialize this with cancellation. */
bool pocketjs_net_operation_claim_native_terminal(
    pocketjs_net_operation_lifecycle_t *lifecycle, bool cancel_requested,
    bool deadline_expired);
/** Cancellation wins over a simultaneous deadline. Caller must serialize. */
pocketjs_net_operation_terminal_reason_t
pocketjs_net_operation_claim_cancel_or_timeout(
    pocketjs_net_operation_lifecycle_t *lifecycle, bool cancel_requested,
    bool deadline_expired);
bool pocketjs_net_operation_begin_delivery(
    pocketjs_net_operation_lifecycle_t *lifecycle);
bool pocketjs_net_operation_retire(
    pocketjs_net_operation_lifecycle_t *lifecycle);
bool pocketjs_net_operation_ticket_matches(
    pocketjs_net_operation_lifecycle_t lifecycle, uint64_t current_token,
    uint64_t requested_token);
bool pocketjs_net_operation_cancel_closes_connection(
    pocketjs_net_transport_operation_kind_t kind);

size_t pocketjs_net_round_robin_next(size_t current, size_t capacity);

/** Never wraps. False permanently retires a slot at UINT64_MAX. */
bool pocketjs_net_generation_advance(uint64_t *generation);
