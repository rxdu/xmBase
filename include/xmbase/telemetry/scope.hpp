/*
 * telemetry/scope.hpp
 *
 * The scope() verb: RAII causal timing spans. A Scope mints a child span of
 * the calling thread's current context, installs itself as current for its
 * lifetime (so nested scopes parent correctly and events inside it carry its
 * span id), and emits ONE record at destruction ({name, ctx, parent, begin,
 * end}) — end-reporting halves ring traffic vs begin+end pairs and matches
 * OTel span semantics.
 *
 * RT cost: two Now() reads, one id generation, one ring push at end; no
 * allocation. `name` must be a string literal. Unbound: context bookkeeping
 * still runs (so nesting behaves identically), no record is emitted.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmbase/telemetry/binding.hpp"
#include "xmbase/telemetry/context.hpp"
#include "xmbase/telemetry/time.hpp"

namespace xmotion {
namespace telemetry {

class Scope {
 public:
  explicit Scope(const char* name) noexcept
      : name_(name), begin_(Now()), saved_(detail::t_current_context) {
    Context child = saved_;
    if (!child.trace.valid()) {
      // No enclosing trace: become a root so orphan scopes still record.
      child.trace.hi = detail::NextId();
      child.trace.lo = detail::NextId();
      child.span.value = 0;  // no parent
    }
    parent_ = child.span;
    child.span.value = detail::NextId();
    detail::t_current_context = child;
  }

  ~Scope() {
    const Context ctx = detail::t_current_context;
    detail::t_current_context = saved_;  // restore FIRST (exception-free path)
    const Binding* b = ActiveBinding();
    if (b != nullptr) b->emit_span(name_, ctx, parent_, begin_, Now());
  }

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  const char* name_;
  Timestamp begin_;
  Context saved_;
  SpanId parent_;
};

}  // namespace telemetry
}  // namespace xmotion

// Unique-named RAII scope for a block: XM_SCOPE("s1.ctrl.cycle");
#ifndef XM_TELEMETRY_LEVEL
#define XM_TELEMETRY_LEVEL 0
#endif
#define XM_TELEMETRY_DETAIL_CONCAT2(a, b) a##b
#define XM_TELEMETRY_DETAIL_CONCAT(a, b) XM_TELEMETRY_DETAIL_CONCAT2(a, b)
#if XM_TELEMETRY_LEVEL <= 5
#define XM_SCOPE(name)                                        \
  ::xmotion::telemetry::Scope XM_TELEMETRY_DETAIL_CONCAT(     \
      xm_telemetry_scope_, __COUNTER__)(name)
#else
#define XM_SCOPE(name) \
  do {                 \
  } while (false)
#endif
