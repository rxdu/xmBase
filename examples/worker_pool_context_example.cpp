/*
 * worker_pool_context_example.cpp
 *
 * REFERENCE USAGE — trace-context discipline on REUSED threads (a worker
 * pool): the classic way telemetry goes wrong in task-based systems is a
 * context leaking from one task into the next unrelated task that happens to
 * run on the same pool thread ("trace-ID pollution" — the failure mode
 * scenario S2-A5 guards). The rule this example demonstrates:
 *
 *     EVERY task body scopes its context with a ContextGuard built from the
 *     identity carried IN THE TASK (envelope bytes) — never relies on
 *     whatever the pool thread had before.
 *
 * Also shows fan-out/fan-in: one iteration's trace fans out to N pool tasks
 * and the joiner LINKS each completed task's context (D7 span links).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <array>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;

namespace {

// A minimal thread pool. Tasks carry their trace identity as ENVELOPE BYTES —
// the pool knows nothing about telemetry; the discipline lives in the task.
class Pool {
 public:
  explicit Pool(int n) {
    for (int i = 0; i < n; ++i)
      workers_.emplace_back([this] { Run(); });
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) w.join();
  }
  void Submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lk(m_);
      q_.push(std::move(task));
    }
    cv_.notify_one();
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return;
        task = std::move(q_.front());
        q_.pop();
      }
      task();
    }
  }
  std::mutex m_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> q_;
  std::vector<std::thread> workers_;
  bool stop_ = false;
};

struct TileResult {
  std::array<std::uint8_t, tel::kContextWireSize> ctx;  // the task's identity
};

}  // namespace

int main() {
  XM_INFO("worker pool demo: 2 iterations x 4 tiles on a 3-thread pool");
  Pool pool(3);

  for (int iter = 1; iter <= 2; ++iter) {
    const tel::Context root = tel::NewTrace();  // one trace per iteration
    tel::ContextGuard g(root);
    tel::Scope iteration("demo.pool.iteration");

    std::mutex done_m;
    std::condition_variable done_cv;
    std::vector<TileResult> results;
    constexpr int kTiles = 4;

    for (int t = 0; t < kTiles; ++t) {
      // The identity travels WITH the task, captured at submission time.
      const auto envelope = tel::Inject(tel::CurrentContext());
      pool.Submit([&, envelope, t] {
        // THE RULE: guard the task's own context — set-and-RESTORE, so this
        // pool thread carries nothing into whatever task runs next.
        tel::ContextGuard task_guard(
            tel::Extract(envelope.data(), envelope.size()));
        XM_SCOPE("demo.pool.tile");
        std::this_thread::sleep_for(std::chrono::milliseconds(2 + t));
        XM_INFO("tile {} of iteration {} done", t, iter);
        std::lock_guard<std::mutex> lk(done_m);
        results.push_back(TileResult{tel::Inject(tel::CurrentContext())});
        done_cv.notify_one();
      });
    }

    // Fan-in: wait for all tiles, LINK each completed tile's context to the
    // joining span (association, not reparenting — D7).
    tel::Scope join("demo.pool.join");
    std::unique_lock<std::mutex> lk(done_m);
    done_cv.wait(lk, [&] { return results.size() == kTiles; });
    for (const auto& r : results)
      join.AddLink(tel::Extract(r.ctx.data(), r.ctx.size()));
    XM_INFO("iteration {} joined {} tiles", iter, kTiles);
  }

  XM_INFO("done — with the SDK bound, each iteration is one trace: tiles as "
          "child spans (correctly parented despite thread reuse), and the "
          "join span linking all 4 tiles");
  return 0;
}
