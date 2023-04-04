// Copyright 2021 The Tint Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/tint/reader/wgsl/parser_impl_test_helper.h"

namespace tint::reader::wgsl {
namespace {

using ParserImplReservedKeywordTest = ParserImplTestWithParam<std::string>;
TEST_P(ParserImplReservedKeywordTest, Function) {
    auto name = GetParam();
    auto p = parser("fn " + name + "() {}");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:4: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, ModuleConst) {
    auto name = GetParam();
    auto p = parser("const " + name + " : i32 = 1;");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:7: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, ModuleVar) {
    auto name = GetParam();
    auto p = parser("var " + name + " : i32 = 1;");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:5: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, FunctionLet) {
    auto name = GetParam();
    auto p = parser("fn f() { let " + name + " : i32 = 1; }");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:14: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, FunctionVar) {
    auto name = GetParam();
    auto p = parser("fn f() { var " + name + " : i32 = 1; }");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:14: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, FunctionParam) {
    auto name = GetParam();
    auto p = parser("fn f(" + name + " : i32) {}");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:6: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, Struct) {
    auto name = GetParam();
    auto p = parser("struct " + name + " {};");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:8: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, StructMember) {
    auto name = GetParam();
    auto p = parser("struct S { " + name + " : i32, };");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:12: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
TEST_P(ParserImplReservedKeywordTest, Alias) {
    auto name = GetParam();
    auto p = parser("type " + name + " = i32;");
    EXPECT_TRUE(p->Parse());
    EXPECT_FALSE(p->has_error());
    EXPECT_EQ(p->error(),
              "1:6: use of deprecated language feature: '" + name + "' is a reserved keyword");
}
INSTANTIATE_TEST_SUITE_P(ParserImplReservedKeywordTest,
                         ParserImplReservedKeywordTest,
                         testing::Values("ComputeShader",
                                         "DomainShader",
                                         "GeometryShader",
                                         "Hullshader",
                                         "NULL",
                                         "Self",
                                         "abstract",
                                         "active",
                                         "alignas",
                                         "alignof",
                                         "as",
                                         "asm",
                                         "asm_fragment",
                                         "async",
                                         "attribute",
                                         "auto",
                                         "await",
                                         "become",
                                         "binding_array",
                                         "cast",
                                         "catch",
                                         "class",
                                         "co_await",
                                         "co_return",
                                         "co_yield",
                                         "coherent",
                                         "column_major",
                                         "common",
                                         "compile",
                                         "compile_fragment",
                                         "concept",
                                         "const_cast",
                                         "consteval",
                                         "constexpr",
                                         "constinit",
                                         "crate",
                                         "debugger",
                                         "decltype",
                                         "delete",
                                         "demote",
                                         "demote_to_helper",
                                         "do",
                                         "dynamic_cast",
                                         "enum",
                                         "explicit",
                                         "export",
                                         "extends",
                                         "extern",
                                         "external",
                                         "filter",
                                         "final",
                                         "finally",
                                         "friend",
                                         "from",
                                         "fxgroup",
                                         "get",
                                         "goto",
                                         "groupshared",
                                         "handle",
                                         "highp",
                                         "impl",
                                         "implements",
                                         "import",
                                         "inline",
                                         "inout",
                                         "instanceof",
                                         "interface",
                                         "invariant",
                                         "layout",
                                         "line",
                                         "lineadj",
                                         "lowp",
                                         "macro",
                                         "macro_rules",
                                         "match",
                                         "mediump",
                                         "meta",
                                         "mod",
                                         "module",
                                         "move",
                                         "mut",
                                         "mutable",
                                         "namespace",
                                         "new",
                                         "nil",
                                         "noexcept",
                                         "noinline",
                                         "nointerpolation",
                                         "noperspective",
                                         "null",
                                         "nullptr",
                                         "of",
                                         "operator",
                                         "package",
                                         "packoffset",
                                         "partition",
                                         "pass",
                                         "patch",
                                         "pixelfragment",
                                         "point",
                                         "precise",
                                         "precision",
                                         "premerge",
                                         "priv",
                                         "protected",
                                         "pub",
                                         "public",
                                         "readonly",
                                         "ref",
                                         "regardless",
                                         "register",
                                         "reinterpret_cast",
                                         "requires",
                                         "resource",
                                         "restrict",
                                         "self",
                                         "set",
                                         "shared",
                                         "signed",
                                         "sizeof",
                                         "smooth",
                                         "snorm",
                                         "static",
                                         "static_cast",
                                         "std",
                                         "subroutine",
                                         "super",
                                         "target",
                                         "template",
                                         "this",
                                         "thread_local",
                                         "throw",
                                         "trait",
                                         "try",
                                         "typedef",
                                         "typeid",
                                         "typename",
                                         "typeof",
                                         "union",
                                         "unless",
                                         "unorm",
                                         "unsafe",
                                         "unsized",
                                         "use",
                                         "using",
                                         "varying",
                                         "virtual",
                                         "volatile",
                                         "wgsl",
                                         "where",
                                         "with",
                                         "writeonly",
                                         "yield"

                                         ));

}  // namespace
}  // namespace tint::reader::wgsl
