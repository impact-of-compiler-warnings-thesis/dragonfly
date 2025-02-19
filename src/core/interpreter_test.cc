// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "core/interpreter.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <absl/strings/str_cat.h>
#include <gmock/gmock.h>

#include "base/gtest.h"
#include "base/logging.h"

namespace dfly {
using namespace std;

class TestSerializer : public ObjectExplorer {
 public:
  string res;

  void OnBool(bool b) final {
    absl::StrAppend(&res, "bool(", b, ") ");
  }

  void OnString(std::string_view str) final {
    absl::StrAppend(&res, "str(", str, ") ");
  }

  void OnDouble(double d) final {
    absl::StrAppend(&res, "d(", d, ") ");
  }

  void OnInt(int64_t val) final {
    absl::StrAppend(&res, "i(", val, ") ");
  }

  void OnArrayStart(unsigned len) final {
    absl::StrAppend(&res, "[");
  }

  void OnArrayEnd() final {
    if (res.back() == ' ')
      res.pop_back();

    absl::StrAppend(&res, "] ");
  }

  void OnNil() final {
    absl::StrAppend(&res, "nil ");
  }

  void OnStatus(std::string_view str) {
    absl::StrAppend(&res, "status(", str, ") ");
  }

  void OnError(std::string_view str) {
    absl::StrAppend(&res, "err(", str, ") ");
  }
};

class InterpreterTest : public ::testing::Test {
 protected:
  InterpreterTest() {
  }

  lua_State* lua() {
    return intptr_.lua();
  }

  void RunInline(string_view buf, const char* name, unsigned num_results = 0) {
    CHECK_EQ(0, luaL_loadbuffer(lua(), buf.data(), buf.size(), name));
    CHECK_EQ(0, lua_pcall(lua(), 0, num_results, 0));
  }

  void SetGlobalArray(const char* name, vector<string> vec);

  bool Execute(string_view script);

  Interpreter intptr_;
  TestSerializer ser_;
  string error_;
};

void InterpreterTest::SetGlobalArray(const char* name, vector<string> vec) {
  vector<MutableSlice> slices(vec.size());
  for (size_t i = 0; i < vec.size(); ++i) {
    slices[i] = MutableSlice{vec[i]};
  }
  intptr_.SetGlobalArray(name, MutSliceSpan{slices});
}

bool InterpreterTest::Execute(string_view script) {
  string result;
  Interpreter::AddResult add_res = intptr_.AddFunction(script, &result);
  if (add_res == Interpreter::COMPILE_ERR) {
    error_ = result;
    return false;
  }

  Interpreter::RunResult run_res = intptr_.RunFunction(result, &error_);
  if (run_res != Interpreter::RUN_OK) {
    return false;
  }

  ser_.res.clear();
  intptr_.SerializeResult(&ser_);
  ser_.res.pop_back();

  return true;
}

TEST_F(InterpreterTest, Basic) {
  RunInline(R"(
    function foo(n)
      return n,n+1
    end)",
            "code1");

  int type = lua_getglobal(lua(), "foo");
  ASSERT_EQ(LUA_TFUNCTION, type);
  lua_pushnumber(lua(), 42);
  lua_pcall(lua(), 1, 2, 0);
  int val1 = lua_tointeger(lua(), -1);
  int val2 = lua_tointeger(lua(), -2);
  lua_pop(lua(), 2);

  EXPECT_EQ(43, val1);
  EXPECT_EQ(42, val2);
  EXPECT_EQ(0, lua_gettop(lua()));

  lua_pushstring(lua(), "foo");
  EXPECT_EQ(3, lua_rawlen(lua(), 1));
  lua_pop(lua(), 1);

  RunInline("return {nil, 'b'}", "code2", 1);
  ASSERT_EQ(1, lua_gettop(lua()));
  LOG(INFO) << lua_typename(lua(), lua_type(lua(), -1));

  ASSERT_TRUE(lua_istable(lua(), -1));
  ASSERT_EQ(2, lua_rawlen(lua(), -1));
  lua_len(lua(), -1);
  ASSERT_EQ(2, lua_tointeger(lua(), -1));
  lua_pop(lua(), 1);

  lua_pushnil(lua());
  while (lua_next(lua(), -2)) {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    int kt = lua_type(lua(), -2);
    int vt = lua_type(lua(), -1);
    LOG(INFO) << "k/v : " << lua_typename(lua(), kt) << "/" << lua_tonumber(lua(), -2) << " "
              << lua_typename(lua(), vt);
    lua_pop(lua(), 1);
  }
}

TEST_F(InterpreterTest, UnknownFunc) {
  string_view code(R"(
    function foo(n)
      return myunknownfunc(1, n)
    end)");

  CHECK_EQ(0, luaL_loadbuffer(lua(), code.data(), code.size(), "code1"));
  CHECK_EQ(0, lua_pcall(lua(), 0, 0, 0));
  int type = lua_getglobal(lua(), "myunknownfunc");
  ASSERT_EQ(LUA_TNIL, type);
}

TEST_F(InterpreterTest, Stack) {
  RunInline(R"(
local x = {}
for i=1,127 do
   x = {x}
end
return x
)",
            "code1", 1);

  ASSERT_EQ(1, lua_gettop(lua()));
  ASSERT_TRUE(intptr_.IsResultSafe());
  lua_pop(lua(), 1);

  RunInline(R"(
local x = {}
for i=1,128 do
   x = {x}
end
return x
)",
            "code1", 1);

  ASSERT_EQ(1, lua_gettop(lua()));
  ASSERT_FALSE(intptr_.IsResultSafe());
}

TEST_F(InterpreterTest, Add) {
  string res1, res2;

  EXPECT_EQ(Interpreter::ADD_OK, intptr_.AddFunction("return 0", &res1));
  EXPECT_EQ(0, lua_gettop(lua()));
  EXPECT_EQ(Interpreter::COMPILE_ERR, intptr_.AddFunction("foobar", &res2));
  EXPECT_THAT(res2, testing::HasSubstr("syntax error"));
  EXPECT_EQ(0, lua_gettop(lua()));
  EXPECT_TRUE(intptr_.Exists(res1));
}

// Test cases taken from scripting.tcl
TEST_F(InterpreterTest, Execute) {
  ASSERT_TRUE(Execute("return 42"));
  EXPECT_EQ("i(42)", ser_.res);

  EXPECT_TRUE(Execute("return 'hello'"));
  EXPECT_EQ("str(hello)", ser_.res);

  // Breaks compatibility.
  EXPECT_TRUE(Execute("return 100.5"));
  EXPECT_EQ("d(100.5)", ser_.res);

  EXPECT_TRUE(Execute("return true"));
  EXPECT_EQ("bool(1)", ser_.res);

  EXPECT_TRUE(Execute("return false"));
  EXPECT_EQ("bool(0)", ser_.res);

  EXPECT_TRUE(Execute("return {ok='fine'}"));
  EXPECT_EQ("status(fine)", ser_.res);

  EXPECT_TRUE(Execute("return {err= 'bla'}"));
  EXPECT_EQ("err(bla)", ser_.res);

  EXPECT_TRUE(Execute("return {1, 2, nil, 3}"));
  EXPECT_EQ("[i(1) i(2) nil i(3)]", ser_.res);

  EXPECT_TRUE(Execute("return {1,2,3,'ciao', {1,2}}"));
  EXPECT_EQ("[i(1) i(2) i(3) str(ciao) [i(1) i(2)]]", ser_.res);
}

TEST_F(InterpreterTest, Call) {
  auto cb = [](MutSliceSpan span, ObjectExplorer* reply) {
    CHECK_GE(span.size(), 1u);
    string_view cmd{span[0].data(), span[0].size()};
    if (cmd == "string") {
      reply->OnString("foo");
    } else if (cmd == "double") {
      reply->OnDouble(3.1415);
    } else if (cmd == "int") {
      reply->OnInt(42);
    } else if (cmd == "err") {
      reply->OnError("myerr");
    } else if (cmd == "status") {
      reply->OnStatus("mystatus");
    } else {
      LOG(FATAL) << "Invalid param";
    }
  };

  intptr_.SetRedisFunc(cb);
  ASSERT_TRUE(Execute("local var = redis.call('string'); return {type(var), var}"));
  EXPECT_EQ("[str(string) str(foo)]", ser_.res);

  EXPECT_TRUE(Execute("local var = redis.call('double'); return {type(var), var}"));
  EXPECT_EQ("[str(number) d(3.1415)]", ser_.res);

  EXPECT_TRUE(Execute("local var = redis.call('int'); return {type(var), var}"));
  EXPECT_EQ("[str(number) i(42)]", ser_.res);

  EXPECT_TRUE(Execute("local var = redis.call('err'); return {type(var), var}"));
  EXPECT_EQ("[str(table) err(myerr)]", ser_.res);

  EXPECT_TRUE(Execute("local var = redis.call('status'); return {type(var), var}"));
  EXPECT_EQ("[str(table) status(mystatus)]", ser_.res);
}

TEST_F(InterpreterTest, CallArray) {
  auto cb = [](MutSliceSpan span, ObjectExplorer* reply) {
    reply->OnArrayStart(2);
    reply->OnArrayStart(1);
    reply->OnArrayStart(2);
    reply->OnNil();
    reply->OnString("s2");
    reply->OnArrayEnd();
    reply->OnArrayEnd();
    reply->OnInt(42);
    reply->OnArrayEnd();
  };

  intptr_.SetRedisFunc(cb);
  EXPECT_TRUE(Execute("local var = redis.call(''); return {type(var), var}"));
  EXPECT_EQ("[str(table) [[[bool(0) str(s2)]] i(42)]]", ser_.res);
}

TEST_F(InterpreterTest, ArgKeys) {
  vector<string> vec_arr{};
  vector<MutableSlice> slices;
  SetGlobalArray("ARGV", {"foo", "bar"});
  SetGlobalArray("KEYS", {"key1", "key2"});
  EXPECT_TRUE(Execute("return {ARGV[1], KEYS[1], KEYS[2]}"));
  EXPECT_EQ("[str(foo) str(key1) str(key2)]", ser_.res);
}

TEST_F(InterpreterTest, Modules) {
  // cjson module
  EXPECT_TRUE(Execute("return cjson.encode({1, 2, 3})"));
  EXPECT_EQ("str([1,2,3])", ser_.res);
  EXPECT_TRUE(Execute("return cjson.decode('{\"a\": 1}')['a']"));
  EXPECT_EQ("d(1)", ser_.res);

  // cmsgpack module
  EXPECT_TRUE(Execute("return cmsgpack.pack('ok', true)"));
  EXPECT_EQ("str(\xA2ok\xC3)", ser_.res);

  // bit module
  EXPECT_TRUE(Execute("return bit.bor(8, 4, 5)"));
  EXPECT_EQ("i(13)", ser_.res);

  // struct module
  EXPECT_TRUE(Execute("return struct.pack('bbc4', 1, 2, 'test')"));
  EXPECT_EQ("str(\x1\x2test)", ser_.res);
}

}  // namespace dfly
