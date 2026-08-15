// SPDX-License-Identifier: MIT

#include "transport_state.h"

#include <limits.h>

bool pocketjs_net_token_gate_can_accept(const pocketjs_net_token_gate_t *gate,
                                        uint64_t token) {
  return gate != NULL && !gate->exhausted && token != 0U && token > gate->last;
}

void pocketjs_net_token_gate_consume(pocketjs_net_token_gate_t *gate,
                                     uint64_t token) {
  gate->last = token;
  if (token == UINT64_MAX) {
    gate->exhausted = true;
  }
}

bool pocketjs_net_terminal_credit_invariant(
    const pocketjs_net_terminal_credits_t *credits) {
  if (credits == NULL) {
    return false;
  }
  return credits->reserved <= credits->capacity &&
         credits->queued <= credits->capacity &&
         credits->delivering <= credits->capacity &&
         credits->reserved + credits->queued + credits->delivering <=
             credits->capacity;
}

bool pocketjs_net_terminal_credit_reserve(
    pocketjs_net_terminal_credits_t *credits) {
  if (!pocketjs_net_terminal_credit_invariant(credits) ||
      credits->reserved + credits->queued + credits->delivering ==
          credits->capacity) {
    return false;
  }
  ++credits->reserved;
  return true;
}

bool pocketjs_net_terminal_credit_enqueue(
    pocketjs_net_terminal_credits_t *credits) {
  if (!pocketjs_net_terminal_credit_invariant(credits) ||
      credits->reserved == 0U) {
    return false;
  }
  --credits->reserved;
  ++credits->queued;
  return pocketjs_net_terminal_credit_invariant(credits);
}

bool pocketjs_net_terminal_credit_take(
    pocketjs_net_terminal_credits_t *credits) {
  if (!pocketjs_net_terminal_credit_invariant(credits) ||
      credits->queued == 0U) {
    return false;
  }
  --credits->queued;
  ++credits->delivering;
  return pocketjs_net_terminal_credit_invariant(credits);
}

bool pocketjs_net_terminal_credit_retire(
    pocketjs_net_terminal_credits_t *credits) {
  if (!pocketjs_net_terminal_credit_invariant(credits) ||
      credits->delivering == 0U) {
    return false;
  }
  --credits->delivering;
  return pocketjs_net_terminal_credit_invariant(credits);
}

bool pocketjs_net_operation_claim_terminal(
    pocketjs_net_operation_lifecycle_t *lifecycle) {
  if (lifecycle == NULL || *lifecycle != POCKETJS_NET_OPERATION_ACTIVE) {
    return false;
  }
  *lifecycle = POCKETJS_NET_OPERATION_TERMINAL_QUEUED;
  return true;
}

pocketjs_net_operation_admission_t
pocketjs_net_operation_admit(bool closing, pocketjs_net_token_gate_t *gate,
                             pocketjs_net_terminal_credits_t *credits,
                             pocketjs_net_operation_lifecycle_t *lifecycle,
                             uint64_t *current_token,
                             uint64_t requested_token) {
  if (closing) {
    return POCKETJS_NET_OPERATION_ADMISSION_CLOSING;
  }
  if (!pocketjs_net_token_gate_can_accept(gate, requested_token)) {
    return POCKETJS_NET_OPERATION_ADMISSION_INVALID_TOKEN;
  }
  if (lifecycle == NULL || current_token == NULL ||
      *lifecycle != POCKETJS_NET_OPERATION_FREE ||
      !pocketjs_net_terminal_credit_reserve(credits)) {
    return POCKETJS_NET_OPERATION_ADMISSION_NO_CAPACITY;
  }
  *current_token = requested_token;
  *lifecycle = POCKETJS_NET_OPERATION_ACTIVE;
  pocketjs_net_token_gate_consume(gate, requested_token);
  return POCKETJS_NET_OPERATION_ADMISSION_ACCEPTED;
}

bool pocketjs_net_operation_claim_native_terminal(
    pocketjs_net_operation_lifecycle_t *lifecycle, bool cancel_requested,
    bool deadline_expired) {
  return !cancel_requested && !deadline_expired &&
         pocketjs_net_operation_claim_terminal(lifecycle);
}

pocketjs_net_operation_terminal_reason_t
pocketjs_net_operation_claim_cancel_or_timeout(
    pocketjs_net_operation_lifecycle_t *lifecycle, bool cancel_requested,
    bool deadline_expired) {
  if ((!cancel_requested && !deadline_expired) ||
      !pocketjs_net_operation_claim_terminal(lifecycle)) {
    return POCKETJS_NET_OPERATION_TERMINAL_NONE;
  }
  return cancel_requested ? POCKETJS_NET_OPERATION_TERMINAL_ABORTED
                          : POCKETJS_NET_OPERATION_TERMINAL_TIMED_OUT;
}

bool pocketjs_net_operation_begin_delivery(
    pocketjs_net_operation_lifecycle_t *lifecycle) {
  if (lifecycle == NULL ||
      *lifecycle != POCKETJS_NET_OPERATION_TERMINAL_QUEUED) {
    return false;
  }
  *lifecycle = POCKETJS_NET_OPERATION_DELIVERING;
  return true;
}

bool pocketjs_net_operation_retire(
    pocketjs_net_operation_lifecycle_t *lifecycle) {
  if (lifecycle == NULL || *lifecycle != POCKETJS_NET_OPERATION_DELIVERING) {
    return false;
  }
  *lifecycle = POCKETJS_NET_OPERATION_FREE;
  return true;
}

bool pocketjs_net_operation_ticket_matches(
    pocketjs_net_operation_lifecycle_t lifecycle, uint64_t current_token,
    uint64_t requested_token) {
  return lifecycle != POCKETJS_NET_OPERATION_FREE && requested_token != 0U &&
         current_token == requested_token;
}

bool pocketjs_net_operation_cancel_closes_connection(
    pocketjs_net_transport_operation_kind_t kind) {
  return kind == POCKETJS_NET_TRANSPORT_OPERATION_CONNECT ||
         kind == POCKETJS_NET_TRANSPORT_OPERATION_READ ||
         kind == POCKETJS_NET_TRANSPORT_OPERATION_WRITE;
}

size_t pocketjs_net_round_robin_next(size_t current, size_t capacity) {
  return capacity == 0U || current >= capacity - 1U ? 0U : current + 1U;
}

bool pocketjs_net_generation_advance(uint64_t *generation) {
  if (generation == NULL || *generation == UINT64_MAX) {
    return false;
  }
  ++*generation;
  if (*generation == 0U) {
    return false;
  }
  return true;
}
