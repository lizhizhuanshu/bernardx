#pragma once

#include "decorator.h"

class ForceSuccess : public Decorator {
public:
    ForceSuccess(AbortMode abort_mode = AbortMode::kNone);

    bool Evaluate(Blackboard& bb) override;
};
