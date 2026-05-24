#include "force_failure.h"

ForceFailure::ForceFailure(AbortMode abort_mode)
    : Decorator("ForceFailure", abort_mode) {}

bool ForceFailure::Evaluate(Blackboard& /*bb*/) {
    return false;
}
