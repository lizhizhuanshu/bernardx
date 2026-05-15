#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "decorator.h"
#include "sensor.h"
#include "types.h"

class Blackboard;
class BtEventQueue;

class Node {
public:
    virtual ~Node() = default;

    virtual NodeStatus Tick(Blackboard& bb, BtEventQueue& events) = 0;
    virtual void Reset();

    // Called when this node (or an ancestor's abort) interrupts execution
    virtual void OnAborted();

    // Tree structure
    Node* parent() const { return parent_; }
    void set_parent(Node* p) { parent_ = p; }

    uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& type() const { return type_; }

    void AddDecorator(std::unique_ptr<Decorator> dec);

    const std::vector<std::unique_ptr<Decorator>>& decorators() const { return decorators_; }

    // Sensor specs
    const std::vector<SensorSpec>& sensor_specs() const { return sensor_specs_; }
    void AddSensorSpec(SensorSpec spec) { sensor_specs_.push_back(std::move(spec)); }

    // Previous decorator evaluation results (keyed by decorator pointer)
    std::unordered_map<Decorator*, bool> prev_decorator_results_;

protected:
    Node(uint32_t id, std::string type, std::string name);

    Node* parent_ = nullptr;
    uint32_t id_;
    std::string type_;
    std::string name_;
    std::vector<std::unique_ptr<Decorator>> decorators_;
    std::vector<SensorSpec> sensor_specs_;
};
