#include "node.h"

#include "lua_runtime.h"

Node::Node(uint32_t id, std::string type, std::string name)
    : id_(id), type_(std::move(type)), name_(std::move(name)) {}

void Node::Reset() {
    last_error_.clear();
    is_running_ = false;
    self_aborted_by_guard_ = false;
    last_tick_was_guard_ = false;
    if (condition_) condition_->Reset();
}

void Node::OnAborted() {
    // A guard-short-circuit abort is self-originated: the very condition that
    // faulted is being preserved (self_aborted_by_guard_ was set by
    // TickAndRecord just before this runs) so the Pipeline's kWaitGuard can
    // re-Eval the SAME condition to detect guard recovery — Reset here would
    // wipe its script counter and the guard would spuriously "recover" every
    // tick. Any other abort (engine preemption, pipeline retry) resets the
    // condition so it starts fresh on the next run. One-shot: a later,
    // unrelated OnAborted goes back to resetting the condition.
    if (self_aborted_by_guard_) {
        self_aborted_by_guard_ = false;
    } else if (condition_) {
        condition_->Reset();
    }
}

async_simple::coro::Lazy<bool> Node::Init(lua_State* /*L*/, LuaRuntime* /*ctx*/) {
    co_return true;
}
