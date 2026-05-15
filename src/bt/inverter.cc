#include "inverter.h"

Inverter::Inverter(AbortMode abort_mode)
    : Decorator("Inverter", abort_mode) {}

void Inverter::set_child(std::unique_ptr<Decorator> child) {
    child_ = std::move(child);
}

bool Inverter::Evaluate(Blackboard& bb) {
    if (!child_) return true;
    return !child_->Evaluate(bb);
}
