#include "json_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

// --- JsonLibrary ---

class JsonLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        rt = LuaRuntime::Builder()
            .RegisterLibrary(std::make_shared<JsonLibrary>())
            .Create();
    }

    LuaRuntime::Ptr rt;
};

TEST_F(JsonLibraryTest, RequireReturnsTable) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        assert(type(json) == "table")
        assert(type(json.decode) == "function")
        assert(type(json.encode) == "function")
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, RequireCached) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = require("json")
        local b = require("json")
        assert(a == b)
    )")).status, LUA_OK);
}

// --- decode ---

TEST_F(JsonLibraryTest, DecodeObject) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local data = json.decode('{"name":"alice","age":30}')
        assert(data.name == "alice")
        assert(data.age == 30)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeArray) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local data = json.decode('[1,2,3]')
        assert(#data == 3)
        assert(data[1] == 1)
        assert(data[2] == 2)
        assert(data[3] == 3)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeString) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local data = json.decode('"hello"')
        assert(data == "hello")
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeInteger) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local data = json.decode('42')
        assert(data == 42)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeFloat) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local data = json.decode('3.14')
        assert(math.abs(data - 3.14) < 0.001)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeBoolean) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        assert(json.decode('true') == true)
        assert(json.decode('false') == false)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeNull) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local data = json.decode('null')
        assert(data == nil)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeNested) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local data = json.decode('{"users":[{"name":"a"},{"name":"b"}],"count":2}')
        assert(data.count == 2)
        assert(#data.users == 2)
        assert(data.users[1].name == "a")
        assert(data.users[2].name == "b")
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DecodeInvalidJsonErrors) {
    auto r = AWAIT(rt->RunScript(R"(
        local json = require("json")
        json.decode('{invalid}')
    )"));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_NE(r.error.find("json parse error"), std::string::npos);
}

// --- encode ---

TEST_F(JsonLibraryTest, EncodeObject) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local s = json.encode({name = "alice", age = 30})
        local data = json.decode(s)
        assert(data.name == "alice")
        assert(data.age == 30)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeArray) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local s = json.encode({1, 2, 3})
        local data = json.decode(s)
        assert(#data == 3)
        assert(data[1] == 1)
        assert(data[2] == 2)
        assert(data[3] == 3)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeString) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        assert(json.encode("hello") == '"hello"')
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeInteger) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        assert(json.encode(42) == '42')
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeFloat) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local s = json.encode(3.14)
        assert(math.abs(tonumber(s) - 3.14) < 0.001)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeBoolean) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        assert(json.encode(true) == 'true')
        assert(json.encode(false) == 'false')
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeNil) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        assert(json.encode(nil) == 'null')
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeWithIndent) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local s = json.encode({a = 1}, 2)
        assert(s:find('\n'), "expected newlines in indented output")
        assert(s:find('  '), "expected indentation")
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, EncodeNested) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local s = json.encode({users = {{name = "a"}, {name = "b"}}, count = 2})
        local data = json.decode(s)
        assert(data.count == 2)
        assert(data.users[1].name == "a")
        assert(data.users[2].name == "b")
    )")).status, LUA_OK);
}

// --- round-trip ---

TEST_F(JsonLibraryTest, RoundTripComplex) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")
        local original = {
            name = "test",
            values = {1, 2.5, true, false, "str"},
            nested = { x = { y = { z = 42 } } },
            empty_arr = {},
        }
        local encoded = json.encode(original)
        local decoded = json.decode(encoded)
        assert(decoded.name == "test")
        assert(decoded.values[1] == 1)
        assert(decoded.values[2] == 2.5)
        assert(decoded.values[3] == true)
        assert(decoded.values[4] == false)
        assert(decoded.values[5] == "str")
        assert(decoded.nested.x.y.z == 42)
        assert(#decoded.empty_arr == 0)
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DeepNesting40LevelsEncodeDecode) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")

        -- Build 40-level nested table: { l1 = { l2 = { ... { l40 = "leaf" } ... } } }
        local root = { leaf = true }
        for i = 2, 40 do
            root = { ["l" .. (41 - i)] = root }
        end
        -- root = { l1 = { l2 = { ... { l39 = { leaf = true } } ... } } }

        -- Encode
        local encoded = json.encode(root)
        assert(type(encoded) == "string", "encode should return string")
        assert(#encoded > 0, "encoded string should not be empty")

        -- Decode back
        local decoded = json.decode(encoded)

        -- Walk all 39 levels + final leaf
        local cur = decoded
        for i = 1, 39 do
            local key = "l" .. i
            assert(type(cur) == "table", "level " .. i .. " should be table, got " .. type(cur))
            assert(cur[key] ~= nil, "missing key " .. key .. " at level " .. i)
            cur = cur[key]
        end
        assert(cur.leaf == true, "leaf value should be true at level 40")
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DeepNesting40LevelsArrayEncodeDecode) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")

        -- Build 40-level nested array: {{{{ ... { 42 } ... }}}}
        local root = { 42 }
        for i = 2, 40 do
            root = { root }
        end

        local encoded = json.encode(root)
        local decoded = json.decode(encoded)

        local cur = decoded
        for i = 1, 39 do
            assert(type(cur) == "table", "level " .. i .. " should be table")
            assert(#cur == 1, "level " .. i .. " should have 1 element, got " .. #cur)
            cur = cur[1]
        end
        assert(cur[1] == 42, "innermost value should be 42")
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DeepNesting40LevelsMixedEncodeDecode) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")

        -- Build 40-level alternating table/array:
        -- { a = { { b = { { c = { ... { answer = 42 } ... } } } } } }
        local root = { answer = 42, flag = true, name = "deep" }
        for i = 1, 20 do
            -- Wrap in array of one element, then in object
            root = { root }
            root = { ["k" .. (21 - i)] = root }
        end

        local encoded = json.encode(root)
        local decoded = json.decode(encoded)

        -- Walk back down: each level is object with key "kN" -> array with [1]
        local cur = decoded
        for i = 1, 20 do
            local key = "k" .. i
            assert(type(cur) == "table", "obj level " .. i .. " should be table")
            assert(cur[key] ~= nil, "missing key " .. key)
            cur = cur[key]
            assert(type(cur) == "table", "arr level " .. i .. " should be table")
            assert(#cur == 1, "arr level " .. i .. " should have 1 element")
            cur = cur[1]
        end
        assert(cur.answer == 42, "answer should be 42")
        assert(cur.flag == true, "flag should be true")
        assert(cur.name == "deep", "name should be 'deep'")
    )")).status, LUA_OK);
}

TEST_F(JsonLibraryTest, DeepNesting40LevelsWithIndent) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local json = require("json")

        -- Build 10-level nested table (40 with indent would be huge, use 10 for indented)
        local root = { value = "end" }
        for i = 1, 9 do
            root = { ["n" .. (10 - i)] = root }
        end

        local compact = json.encode(root)
        local indented = json.encode(root, 2)

        -- Indented should be larger due to whitespace
        assert(#indented > #compact, "indented should be larger than compact")

        -- Both should decode to same result
        local d1 = json.decode(compact)
        local d2 = json.decode(indented)

        -- Walk both and compare
        local c1, c2 = d1, d2
        for i = 1, 9 do
            local key = "n" .. i
            c1 = c1[key]
            c2 = c2[key]
        end
        assert(c1.value == "end")
        assert(c2.value == "end")
    )")).status, LUA_OK);
}
