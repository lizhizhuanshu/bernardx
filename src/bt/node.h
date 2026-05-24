#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "decorator.h"
#include "sensor.h"
#include "types.h"

class Blackboard;
class BtEventQueue;
class LuaRuntime;

class Node {
public:
    virtual ~Node() = default;

    virtual NodeStatus Tick(Blackboard& bb, BtEventQueue& events) = 0;
    virtual void Reset();
    virtual void OnAborted();

    virtual async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx,
                                                 const std::string& base_path);

    // Tree structure
    Node* parent() const { return parent_; }
    void set_parent(Node* p) { parent_ = p; }

    uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& type() const { return type_; }
    const std::string& description() const { return description_; }
    void set_description(std::string desc) { description_ = std::move(desc); }

    const std::string& last_error() const { return last_error_; }
    void set_last_error(std::string err) { last_error_ = std::move(err); }

    void AddDecorator(std::unique_ptr<Decorator> dec);

    const std::vector<std::unique_ptr<Decorator>>& decorators() const { return decorators_; }

    const std::vector<SensorSpec>& sensor_specs() const { return sensor_specs_; }
    void AddSensorSpec(SensorSpec spec) { sensor_specs_.push_back(std::move(spec)); }

protected:
    Node(uint32_t id, std::string type, std::string name);

    Node* parent_ = nullptr;
    uint32_t id_;
    std::string type_;
    std::string name_;
    std::string description_;
    std::string last_error_;
    std::vector<std::unique_ptr<Decorator>> decorators_;
    std::vector<SensorSpec> sensor_specs_;
};
