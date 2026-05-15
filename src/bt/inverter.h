#pragma once

#include <memory>

#include "decorator.h"

class Inverter : public Decorator {
public:
    Inverter(AbortMode abort_mode = AbortMode::kNone);

    void set_child(std::unique_ptr<Decorator> child);
    Decorator* child() const { return child_.get(); }

    bool Evaluate(Blackboard& bb) override;

private:
    std::unique_ptr<Decorator> child_;
};
