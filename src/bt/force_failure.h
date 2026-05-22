#pragma once

#include "decorator.h"

class ForceFailure : public Decorator {
public:
    ForceFailure(AbortMode abort_mode = AbortMode::kNone);

    bool Evaluate(Blackboard& bb) override;
};
