#pragma once

#include <string>

#include "types.h"

class Blackboard;

class Decorator {
public:
    virtual ~Decorator() = default;

    virtual bool Evaluate(Blackboard& bb) = 0;
    virtual void OnAborted() {}

    AbortMode abort_mode() const { return abort_mode_; }
    void set_abort_mode(AbortMode mode) { abort_mode_ = mode; }

    const std::string& type() const { return type_; }

protected:
    explicit Decorator(std::string type, AbortMode abort_mode = AbortMode::kNone)
        : type_(std::move(type)), abort_mode_(abort_mode) {}

    std::string type_;
    AbortMode abort_mode_ = AbortMode::kNone;
};
