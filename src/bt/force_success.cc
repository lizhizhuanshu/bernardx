#include "force_success.h"

ForceSuccess::ForceSuccess(AbortMode abort_mode)
    : Decorator("ForceSuccess", abort_mode) {}

bool ForceSuccess::Evaluate(Blackboard& /*bb*/) {
    return true;
}
