// Copyright 2022 The Tint Authors.
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

#include <cmath>
#include <tuple>
#include <vector>

#include "src/tint/program_builder.h"
#include "src/tint/utils/compiler_macros.h"

#include "gtest/gtest.h"

using namespace tint::number_suffixes;  // NOLINT

namespace tint {
namespace {

// Next ULP up from kHighestF32 for a float64.
constexpr double kHighestF32NextULP = 0x1.fffffe0000001p+127;

// Highest subnormal value for a float32.
constexpr double kHighestF32Subnormal = 0x0.fffffep-126;

// Next ULP up from kHighestF16 for a float64.
constexpr double kHighestF16NextULP = 0x1.ffc0000000001p+15;

// Highest subnormal value for a float16.
constexpr double kHighestF16Subnormal = 0x0.ffcp-14;

constexpr double kLowestF32NextULP = -kHighestF32NextULP;
constexpr double kLowestF16NextULP = -kHighestF16NextULP;

// MSVC (only in release builds) can grumble about some of the inlined numerical overflow /
// underflow that's done in this file. We like to think we know what we're doing, so silence the
// warning.
TINT_BEGIN_DISABLE_WARNING(CONSTANT_OVERFLOW);

TEST(NumberTest, Equality) {
    EXPECT_TRUE(0_a == 0_a);
    EXPECT_TRUE(10_a == 10_a);
    EXPECT_TRUE(-10_a == -10_a);

    EXPECT_TRUE(0_i == 0_i);
    EXPECT_TRUE(10_i == 10_i);
    EXPECT_TRUE(-10_i == -10_i);

    EXPECT_TRUE(0_u == 0_u);
    EXPECT_TRUE(10_u == 10_u);

    EXPECT_TRUE(0._a == 0._a);
    EXPECT_TRUE(-0._a == -0._a);
    EXPECT_TRUE(10._a == 10._a);
    EXPECT_TRUE(-10._a == -10._a);

    EXPECT_TRUE(0_f == 0_f);
    EXPECT_TRUE(-0_f == -0_f);
    EXPECT_TRUE(10_f == 10_f);
    EXPECT_TRUE(-10_f == -10_f);

    EXPECT_TRUE(0_h == 0_h);
    EXPECT_TRUE(-0_h == -0_h);
    EXPECT_TRUE(10_h == 10_h);
    EXPECT_TRUE(-10_h == -10_h);
}

TEST(NumberTest, Inequality) {
    EXPECT_TRUE(0_a != 1_a);
    EXPECT_TRUE(10_a != 11_a);
    EXPECT_TRUE(11_a != 10_a);
    EXPECT_TRUE(-10_a != -11_a);
    EXPECT_TRUE(-11_a != -10_a);

    EXPECT_TRUE(0_i != 1_i);
    EXPECT_TRUE(1_i != 0_i);
    EXPECT_TRUE(10_i != 11_i);
    EXPECT_TRUE(11_i != 10_i);
    EXPECT_TRUE(-10_i != -11_i);
    EXPECT_TRUE(-11_i != -10_i);

    EXPECT_TRUE(0_u != 1_u);
    EXPECT_TRUE(1_u != 0_u);
    EXPECT_TRUE(10_u != 11_u);
    EXPECT_TRUE(11_u != 10_u);

    EXPECT_TRUE(0._a != -0._a);
    EXPECT_TRUE(-0._a != 0._a);
    EXPECT_TRUE(10._a != 11._a);
    EXPECT_TRUE(11._a != 10._a);
    EXPECT_TRUE(-10._a != -11._a);
    EXPECT_TRUE(-11._a != -10._a);

    EXPECT_TRUE(0_f != -0_f);
    EXPECT_TRUE(-0_f != 0_f);
    EXPECT_TRUE(-0_f != -1_f);
    EXPECT_TRUE(-1_f != -0_f);
    EXPECT_TRUE(10_f != -10_f);
    EXPECT_TRUE(-10_f != 10_f);
    EXPECT_TRUE(10_f != 11_f);
    EXPECT_TRUE(-10_f != -11_f);

    EXPECT_TRUE(0_h != -0_h);
    EXPECT_TRUE(-0_h != 0_h);
    EXPECT_TRUE(-0_h != -1_h);
    EXPECT_TRUE(-1_h != -0_h);
    EXPECT_TRUE(10_h != -10_h);
    EXPECT_TRUE(-10_h != 10_h);
    EXPECT_TRUE(10_h != 11_h);
    EXPECT_TRUE(-10_h != -11_h);
}

TEST(NumberTest, CheckedConvertIdentity) {
    EXPECT_EQ(CheckedConvert<AInt>(0_a), 0_a);
    EXPECT_EQ(CheckedConvert<AFloat>(0_a), 0.0_a);
    EXPECT_EQ(CheckedConvert<i32>(0_i), 0_i);
    EXPECT_EQ(CheckedConvert<u32>(0_u), 0_u);
    EXPECT_EQ(CheckedConvert<f32>(0_f), 0_f);
    EXPECT_EQ(CheckedConvert<f16>(0_h), 0_h);

    EXPECT_EQ(CheckedConvert<AInt>(1_a), 1_a);
    EXPECT_EQ(CheckedConvert<AFloat>(1_a), 1.0_a);
    EXPECT_EQ(CheckedConvert<i32>(1_i), 1_i);
    EXPECT_EQ(CheckedConvert<u32>(1_u), 1_u);
    EXPECT_EQ(CheckedConvert<f32>(1_f), 1_f);
    EXPECT_EQ(CheckedConvert<f16>(1_h), 1_h);
}

TEST(NumberTest, CheckedConvertLargestValue) {
    EXPECT_EQ(CheckedConvert<i32>(AInt(i32::Highest())), i32::Highest());
    EXPECT_EQ(CheckedConvert<u32>(AInt(u32::Highest())), u32::Highest());
    EXPECT_EQ(CheckedConvert<u32>(i32::Highest()), u32(i32::Highest()));
    EXPECT_EQ(CheckedConvert<f32>(AFloat(f32::Highest())), f32::Highest());
    EXPECT_EQ(CheckedConvert<f16>(AFloat(f16::Highest())), f16::Highest());
}

TEST(NumberTest, CheckedConvertLowestValue) {
    EXPECT_EQ(CheckedConvert<i32>(AInt(i32::Lowest())), i32::Lowest());
    EXPECT_EQ(CheckedConvert<u32>(AInt(u32::Lowest())), u32::Lowest());
    EXPECT_EQ(CheckedConvert<f32>(AFloat(f32::Lowest())), f32::Lowest());
    EXPECT_EQ(CheckedConvert<f16>(AFloat(f16::Lowest())), f16::Lowest());
}

TEST(NumberTest, CheckedConvertSmallestValue) {
    EXPECT_EQ(CheckedConvert<i32>(AInt(0)), i32(0));
    EXPECT_EQ(CheckedConvert<u32>(AInt(0)), u32(0));
    EXPECT_EQ(CheckedConvert<f32>(AFloat(f32::Smallest())), f32::Smallest());
    EXPECT_EQ(CheckedConvert<f16>(AFloat(f16::Smallest())), f16::Smallest());
}

TEST(NumberTest, CheckedConvertExceedsPositiveLimit) {
    EXPECT_EQ(CheckedConvert<i32>(AInt(static_cast<int64_t>(i32::Highest()) + 1)),
              ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<u32>(AInt(static_cast<uint64_t>(u32::Highest()) + 1)),
              ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<i32>(u32::Highest()), ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<i32>(u32(0x80000000)), ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<u32>(f32::Highest()), ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<i32>(f32::Highest()), ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<u32>(AFloat::Highest()), ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<i32>(AFloat::Highest()), ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<f32>(AFloat(kHighestF32NextULP)),
              ConversionFailure::kExceedsPositiveLimit);
    EXPECT_EQ(CheckedConvert<f16>(AFloat(kHighestF16NextULP)),
              ConversionFailure::kExceedsPositiveLimit);
}

TEST(NumberTest, CheckedConvertExceedsNegativeLimit) {
    EXPECT_EQ(CheckedConvert<i32>(AInt(static_cast<int64_t>(i32::Lowest()) - 1)),
              ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<u32>(AInt(static_cast<uint64_t>(u32::Lowest()) - 1)),
              ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<u32>(i32(-1)), ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<u32>(i32::Lowest()), ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<u32>(f32::Lowest()), ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<i32>(f32::Lowest()), ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<u32>(AFloat::Lowest()), ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<i32>(AFloat::Lowest()), ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<f32>(AFloat(kLowestF32NextULP)),
              ConversionFailure::kExceedsNegativeLimit);
    EXPECT_EQ(CheckedConvert<f16>(AFloat(kLowestF16NextULP)),
              ConversionFailure::kExceedsNegativeLimit);
}

TEST(NumberTest, CheckedConvertSubnormals) {
    EXPECT_EQ(CheckedConvert<f32>(AFloat(kHighestF32Subnormal)), f32(kHighestF32Subnormal));
    EXPECT_EQ(CheckedConvert<f16>(AFloat(kHighestF16Subnormal)), f16(kHighestF16Subnormal));
    EXPECT_EQ(CheckedConvert<f32>(AFloat(-kHighestF32Subnormal)), f32(-kHighestF32Subnormal));
    EXPECT_EQ(CheckedConvert<f16>(AFloat(-kHighestF16Subnormal)), f16(-kHighestF16Subnormal));
}

// Test cases for f16 subnormal quantization and BitsRepresentation.
// The ULP is based on float rather than double or f16, since F16::Quantize and
// F16::BitsRepresentation take float as input.
constexpr float lowestPositiveNormalF16 = 0x1p-14;
constexpr float lowestPositiveNormalF16PlusULP = 0x1.000002p-14;
constexpr float lowestPositiveNormalF16MinusULP = 0x1.fffffep-15;
constexpr float highestPositiveSubnormalF16 = 0x0.ffcp-14;
constexpr float highestPositiveSubnormalF16PlusULP = 0x1.ff8002p-15;
constexpr float highestPositiveSubnormalF16MinusULP = 0x1.ff7ffep-15;
constexpr float lowestPositiveSubnormalF16 = 0x1.p-24;
constexpr float lowestPositiveSubnormalF16PlusULP = 0x1.000002p-24;
constexpr float lowestPositiveSubnormalF16MinusULP = 0x1.fffffep-25;

constexpr uint16_t lowestPositiveNormalF16Bits = 0x0400u;
constexpr uint16_t highestPositiveSubnormalF16Bits = 0x03ffu;
constexpr uint16_t lowestPositiveSubnormalF16Bits = 0x0001u;

constexpr float highestNegativeNormalF16 = -lowestPositiveNormalF16;
constexpr float highestNegativeNormalF16PlusULP = -lowestPositiveNormalF16MinusULP;
constexpr float highestNegativeNormalF16MinusULP = -lowestPositiveNormalF16PlusULP;
constexpr float lowestNegativeSubnormalF16 = -highestPositiveSubnormalF16;
constexpr float lowestNegativeSubnormalF16PlusULP = -highestPositiveSubnormalF16MinusULP;
constexpr float lowestNegativeSubnormalF16MinusULP = -highestPositiveSubnormalF16PlusULP;
constexpr float highestNegativeSubnormalF16 = -lowestPositiveSubnormalF16;
constexpr float highestNegativeSubnormalF16PlusULP = -lowestPositiveSubnormalF16MinusULP;
constexpr float highestNegativeSubnormalF16MinusULP = -lowestPositiveSubnormalF16PlusULP;

constexpr uint16_t highestNegativeNormalF16Bits = 0x8400u;
constexpr uint16_t lowestNegativeSubnormalF16Bits = 0x83ffu;
constexpr uint16_t highestNegativeSubnormalF16Bits = 0x8001u;

constexpr float f32_nan = std::numeric_limits<float>::quiet_NaN();
constexpr float f32_inf = std::numeric_limits<float>::infinity();

struct F16TestCase {
    float input_value;
    float quantized_value;
    uint16_t f16_bit_pattern;
};

using NumberF16Test = testing::TestWithParam<F16TestCase>;

TEST_P(NumberF16Test, QuantizeF16) {
    float input_value = GetParam().input_value;
    float quantized_value = GetParam().quantized_value;

    std::stringstream ss;
    ss << "input value = " << input_value << ", expected quantized value = " << quantized_value;
    SCOPED_TRACE(ss.str());

    if (std::isnan(quantized_value)) {
        EXPECT_TRUE(std::isnan(f16(input_value)));
    } else {
        EXPECT_EQ(f16(input_value), quantized_value);
    }
}

TEST_P(NumberF16Test, BitsRepresentation) {
    float input_value = GetParam().input_value;
    uint16_t representation = GetParam().f16_bit_pattern;

    std::stringstream ss;
    ss << "input value = " << input_value
       << ", expected binary16 bits representation = " << std::hex << std::showbase
       << representation;
    SCOPED_TRACE(ss.str());

    EXPECT_EQ(f16(input_value).BitsRepresentation(), representation);
}

INSTANTIATE_TEST_SUITE_P(
    NumberF16Test,
    NumberF16Test,
    testing::ValuesIn(std::vector<F16TestCase>{
        // NaN, Inf
        {f32_inf, f32_inf, 0x7c00u},
        {-f32_inf, -f32_inf, 0xfc00u},
        {f32_nan, f32_nan, 0x7e00u},
        {-f32_nan, -f32_nan, 0x7e00u},
        // +/- zero
        {+0.0f, 0.0f, 0x0000u},
        {-0.0f, -0.0f, 0x8000u},
        // Value in normal f16 range
        {1.0f, 1.0f, 0x3c00u},
        {-1.0f, -1.0f, 0xbc00u},
        //   0.00006106496 quantized to 0.000061035156 = 0x1p-14
        {0.00006106496f, 0.000061035156f, 0x0400u},
        {-0.00006106496f, -0.000061035156f, 0x8400u},
        //   1.0004883 quantized to 1.0 = 0x1p0
        {1.0004883f, 1.0f, 0x3c00u},
        {-1.0004883f, -1.0f, 0xbc00u},
        //   8196.0 quantized to 8192.0 = 0x1p13
        {8196.0f, 8192.f, 0x7000u},
        {-8196.0f, -8192.f, 0xf000u},
        // Value in subnormal f16 range
        {0x0.034p-14f, 0x0.034p-14f, 0x000du},
        {-0x0.034p-14f, -0x0.034p-14f, 0x800du},
        {0x0.068p-14f, 0x0.068p-14f, 0x001au},
        {-0x0.068p-14f, -0x0.068p-14f, 0x801au},
        //   0x0.06b7p-14 quantized to 0x0.068p-14
        {0x0.06b7p-14f, 0x0.068p-14f, 0x001au},
        {-0x0.06b7p-14f, -0x0.068p-14, 0x801au},
        // Value out of f16 range
        {65504.003f, f32_inf, 0x7c00u},
        {-65504.003f, -f32_inf, 0xfc00u},
        {0x1.234p56f, f32_inf, 0x7c00u},
        {-0x4.321p65f, -f32_inf, 0xfc00u},

        // Test for subnormal quantization.
        // Value larger than or equal to lowest positive normal f16 will be quantized to normal f16.
        {lowestPositiveNormalF16PlusULP, lowestPositiveNormalF16, lowestPositiveNormalF16Bits},
        {lowestPositiveNormalF16, lowestPositiveNormalF16, lowestPositiveNormalF16Bits},
        // Positive value smaller than lowest positive normal f16 but not smaller than lowest
        // positive
        // subnormal f16 will be quantized to subnormal f16 or zero.
        {lowestPositiveNormalF16MinusULP, highestPositiveSubnormalF16,
         highestPositiveSubnormalF16Bits},
        {highestPositiveSubnormalF16PlusULP, highestPositiveSubnormalF16,
         highestPositiveSubnormalF16Bits},
        {highestPositiveSubnormalF16, highestPositiveSubnormalF16, highestPositiveSubnormalF16Bits},
        {highestPositiveSubnormalF16MinusULP, 0x0.ff8p-14, 0x03feu},
        {lowestPositiveSubnormalF16PlusULP, lowestPositiveSubnormalF16,
         lowestPositiveSubnormalF16Bits},
        {lowestPositiveSubnormalF16, lowestPositiveSubnormalF16, lowestPositiveSubnormalF16Bits},
        // Positive value smaller than lowest positive subnormal f16 will be quantized to zero.
        {lowestPositiveSubnormalF16MinusULP, 0.0, 0x0000u},
        // Test the mantissa discarding, the least significant mantissa bit is 0x1p-24 =
        // 0x0.004p-14.
        {0x0.064p-14f, 0x0.064p-14, 0x0019u},
        {0x0.067fecp-14f, 0x0.064p-14, 0x0019u},
        {0x0.063ffep-14f, 0x0.060p-14, 0x0018u},
        {0x0.008p-14f, 0x0.008p-14, 0x0002u},
        {0x0.00bffep-14f, 0x0.008p-14, 0x0002u},
        {0x0.007ffep-14f, 0x0.004p-14, 0x0001u},

        // Vice versa for negative cases.
        {highestNegativeNormalF16MinusULP, highestNegativeNormalF16, highestNegativeNormalF16Bits},
        {highestNegativeNormalF16, highestNegativeNormalF16, highestNegativeNormalF16Bits},
        {highestNegativeNormalF16PlusULP, lowestNegativeSubnormalF16,
         lowestNegativeSubnormalF16Bits},
        {lowestNegativeSubnormalF16MinusULP, lowestNegativeSubnormalF16,
         lowestNegativeSubnormalF16Bits},
        {lowestNegativeSubnormalF16, lowestNegativeSubnormalF16, lowestNegativeSubnormalF16Bits},
        {lowestNegativeSubnormalF16PlusULP, -0x0.ff8p-14, 0x83feu},
        {highestNegativeSubnormalF16MinusULP, highestNegativeSubnormalF16,
         highestNegativeSubnormalF16Bits},
        {highestNegativeSubnormalF16, highestNegativeSubnormalF16, highestNegativeSubnormalF16Bits},
        {highestNegativeSubnormalF16PlusULP, -0.0, 0x8000u},
        // Test the mantissa discarding.
        {-0x0.064p-14f, -0x0.064p-14, 0x8019u},
        {-0x0.067fecp-14f, -0x0.064p-14, 0x8019u},
        {-0x0.063ffep-14f, -0x0.060p-14, 0x8018u},
        {-0x0.008p-14f, -0x0.008p-14, 0x8002u},
        {-0x0.00bffep-14f, -0x0.008p-14, 0x8002u},
        {-0x0.007ffep-14f, -0x0.004p-14, 0x8001u},
        /////////////////////////////////////
    }));

#ifdef OVERFLOW
#undef OVERFLOW  // corecrt_math.h :(
#endif
#define OVERFLOW \
    {}

using BinaryCheckedCase_AInt = std::tuple<std::optional<AInt>, AInt, AInt>;
using BinaryCheckedCase_AFloat = std::tuple<std::optional<AFloat>, AFloat, AFloat>;

using CheckedAddTest_AInt = testing::TestWithParam<BinaryCheckedCase_AInt>;
TEST_P(CheckedAddTest_AInt, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    EXPECT_EQ(CheckedAdd(a, b), expect) << std::hex << "0x" << a << " + 0x" << b;
    EXPECT_EQ(CheckedAdd(b, a), expect) << std::hex << "0x" << a << " + 0x" << b;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedAddTest_AInt,
    CheckedAddTest_AInt,
    testing::ValuesIn(std::vector<BinaryCheckedCase_AInt>{
        {AInt(0), AInt(0), AInt(0)},
        {AInt(1), AInt(1), AInt(0)},
        {AInt(2), AInt(1), AInt(1)},
        {AInt(0), AInt(-1), AInt(1)},
        {AInt(3), AInt(2), AInt(1)},
        {AInt(-1), AInt(-2), AInt(1)},
        {AInt(0x300), AInt(0x100), AInt(0x200)},
        {AInt(0x100), AInt(-0x100), AInt(0x200)},
        {AInt::Highest(), AInt(1), AInt(AInt::kHighestValue - 1)},
        {AInt::Lowest(), AInt(-1), AInt(AInt::kLowestValue + 1)},
        {AInt::Highest(), AInt(0x7fffffff00000000ll), AInt(0x00000000ffffffffll)},
        {AInt::Highest(), AInt::Highest(), AInt(0)},
        {AInt::Lowest(), AInt::Lowest(), AInt(0)},
        {OVERFLOW, AInt(1), AInt::Highest()},
        {OVERFLOW, AInt(-1), AInt::Lowest()},
        {OVERFLOW, AInt(2), AInt::Highest()},
        {OVERFLOW, AInt(-2), AInt::Lowest()},
        {OVERFLOW, AInt(10000), AInt::Highest()},
        {OVERFLOW, AInt(-10000), AInt::Lowest()},
        {OVERFLOW, AInt::Highest(), AInt::Highest()},
        {OVERFLOW, AInt::Lowest(), AInt::Lowest()},
        ////////////////////////////////////////////////////////////////////////
    }));

using CheckedAddTest_AFloat = testing::TestWithParam<BinaryCheckedCase_AFloat>;
TEST_P(CheckedAddTest_AFloat, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    EXPECT_EQ(CheckedAdd(a, b), expect) << std::hex << "0x" << a << " + 0x" << b;
    EXPECT_EQ(CheckedAdd(b, a), expect) << std::hex << "0x" << a << " + 0x" << b;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedAddTest_AFloat,
    CheckedAddTest_AFloat,
    testing::ValuesIn(std::vector<BinaryCheckedCase_AFloat>{
        {AFloat(0), AFloat(0), AFloat(0)},
        {AFloat(1), AFloat(1), AFloat(0)},
        {AFloat(2), AFloat(1), AFloat(1)},
        {AFloat(0), AFloat(-1), AFloat(1)},
        {AFloat(3), AFloat(2), AFloat(1)},
        {AFloat(-1), AFloat(-2), AFloat(1)},
        {AFloat(0x300), AFloat(0x100), AFloat(0x200)},
        {AFloat(0x100), AFloat(-0x100), AFloat(0x200)},
        {AFloat::Highest(), AFloat(1), AFloat(AFloat::kHighestValue - 1)},
        {AFloat::Lowest(), AFloat(-1), AFloat(AFloat::kLowestValue + 1)},
        {AFloat::Highest(), AFloat::Highest(), AFloat(0)},
        {AFloat::Lowest(), AFloat::Lowest(), AFloat(0)},
        {OVERFLOW, AFloat::Highest(), AFloat::Highest()},
        {OVERFLOW, AFloat::Lowest(), AFloat::Lowest()},
        ////////////////////////////////////////////////////////////////////////
    }));

using CheckedSubTest_AInt = testing::TestWithParam<BinaryCheckedCase_AInt>;
TEST_P(CheckedSubTest_AInt, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    EXPECT_EQ(CheckedSub(a, b), expect) << std::hex << "0x" << a << " - 0x" << b;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedSubTest_AInt,
    CheckedSubTest_AInt,
    testing::ValuesIn(std::vector<BinaryCheckedCase_AInt>{
        {AInt(0), AInt(0), AInt(0)},
        {AInt(1), AInt(1), AInt(0)},
        {AInt(0), AInt(1), AInt(1)},
        {AInt(-2), AInt(-1), AInt(1)},
        {AInt(1), AInt(2), AInt(1)},
        {AInt(-3), AInt(-2), AInt(1)},
        {AInt(0x100), AInt(0x300), AInt(0x200)},
        {AInt(-0x300), AInt(-0x100), AInt(0x200)},
        {AInt::Highest(), AInt(AInt::kHighestValue - 1), AInt(-1)},
        {AInt::Lowest(), AInt(AInt::kLowestValue + 1), AInt(1)},
        {AInt(0x00000000ffffffffll), AInt::Highest(), AInt(0x7fffffff00000000ll)},
        {AInt::Highest(), AInt::Highest(), AInt(0)},
        {AInt::Lowest(), AInt::Lowest(), AInt(0)},
        {OVERFLOW, AInt::Lowest(), AInt(1)},
        {OVERFLOW, AInt::Highest(), AInt(-1)},
        {OVERFLOW, AInt::Lowest(), AInt(2)},
        {OVERFLOW, AInt::Highest(), AInt(-2)},
        {OVERFLOW, AInt::Lowest(), AInt(10000)},
        {OVERFLOW, AInt::Highest(), AInt(-10000)},
        {OVERFLOW, AInt::Lowest(), AInt::Highest()},
        ////////////////////////////////////////////////////////////////////////
    }));

using CheckedSubTest_AFloat = testing::TestWithParam<BinaryCheckedCase_AFloat>;
TEST_P(CheckedSubTest_AFloat, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    EXPECT_EQ(CheckedSub(a, b), expect) << std::hex << "0x" << a << " - 0x" << b;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedSubTest_AFloat,
    CheckedSubTest_AFloat,
    testing::ValuesIn(std::vector<BinaryCheckedCase_AFloat>{
        {AFloat(0), AFloat(0), AFloat(0)},
        {AFloat(1), AFloat(1), AFloat(0)},
        {AFloat(0), AFloat(1), AFloat(1)},
        {AFloat(-2), AFloat(-1), AFloat(1)},
        {AFloat(1), AFloat(2), AFloat(1)},
        {AFloat(-3), AFloat(-2), AFloat(1)},
        {AFloat(0x100), AFloat(0x300), AFloat(0x200)},
        {AFloat(-0x300), AFloat(-0x100), AFloat(0x200)},
        {AFloat::Highest(), AFloat(AFloat::kHighestValue - 1), AFloat(-1)},
        {AFloat::Lowest(), AFloat(AFloat::kLowestValue + 1), AFloat(1)},
        {AFloat::Highest(), AFloat::Highest(), AFloat(0)},
        {AFloat::Lowest(), AFloat::Lowest(), AFloat(0)},
        {OVERFLOW, AFloat::Lowest(), AFloat::Highest()},
        ////////////////////////////////////////////////////////////////////////
    }));

using CheckedMulTest_AInt = testing::TestWithParam<BinaryCheckedCase_AInt>;
TEST_P(CheckedMulTest_AInt, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    EXPECT_EQ(CheckedMul(a, b), expect) << std::hex << "0x" << a << " * 0x" << b;
    EXPECT_EQ(CheckedMul(b, a), expect) << std::hex << "0x" << a << " * 0x" << b;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedMulTest_AInt,
    CheckedMulTest_AInt,
    testing::ValuesIn(std::vector<BinaryCheckedCase_AInt>{
        {AInt(0), AInt(0), AInt(0)},
        {AInt(0), AInt(1), AInt(0)},
        {AInt(1), AInt(1), AInt(1)},
        {AInt(-1), AInt(-1), AInt(1)},
        {AInt(2), AInt(2), AInt(1)},
        {AInt(-2), AInt(-2), AInt(1)},
        {AInt(0x20000), AInt(0x100), AInt(0x200)},
        {AInt(-0x20000), AInt(-0x100), AInt(0x200)},
        {AInt(0x4000000000000000ll), AInt(0x80000000ll), AInt(0x80000000ll)},
        {AInt(0x4000000000000000ll), AInt(-0x80000000ll), AInt(-0x80000000ll)},
        {AInt(0x1000000000000000ll), AInt(0x40000000ll), AInt(0x40000000ll)},
        {AInt(-0x1000000000000000ll), AInt(-0x40000000ll), AInt(0x40000000ll)},
        {AInt(0x100000000000000ll), AInt(0x1000000), AInt(0x100000000ll)},
        {AInt(0x2000000000000000ll), AInt(0x1000000000000000ll), AInt(2)},
        {AInt(-0x2000000000000000ll), AInt(0x1000000000000000ll), AInt(-2)},
        {AInt(-0x2000000000000000ll), AInt(-0x1000000000000000ll), AInt(2)},
        {AInt(-0x2000000000000000ll), AInt(0x1000000000000000ll), AInt(-2)},
        {AInt(0x4000000000000000ll), AInt(0x1000000000000000ll), AInt(4)},
        {AInt(-0x4000000000000000ll), AInt(0x1000000000000000ll), AInt(-4)},
        {AInt(-0x4000000000000000ll), AInt(-0x1000000000000000ll), AInt(4)},
        {AInt(-0x4000000000000000ll), AInt(0x1000000000000000ll), AInt(-4)},
        {AInt(-0x8000000000000000ll), AInt(0x1000000000000000ll), AInt(-8)},
        {AInt(-0x8000000000000000ll), AInt(-0x1000000000000000ll), AInt(8)},
        {AInt(0), AInt::Highest(), AInt(0)},
        {AInt(0), AInt::Lowest(), AInt(0)},
        {OVERFLOW, AInt(0x1000000000000000ll), AInt(8)},
        {OVERFLOW, AInt(-0x1000000000000000ll), AInt(-8)},
        {OVERFLOW, AInt(0x800000000000000ll), AInt(0x10)},
        {OVERFLOW, AInt(0x80000000ll), AInt(0x100000000ll)},
        {OVERFLOW, AInt::Highest(), AInt::Highest()},
        {OVERFLOW, AInt::Highest(), AInt::Lowest()},
        ////////////////////////////////////////////////////////////////////////
    }));

using CheckedDivTest_AInt = testing::TestWithParam<BinaryCheckedCase_AInt>;
TEST_P(CheckedDivTest_AInt, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    EXPECT_EQ(CheckedDiv(a, b), expect) << std::hex << "0x" << a << " - 0x" << b;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedDivTest_AInt,
    CheckedDivTest_AInt,
    testing::ValuesIn(std::vector<BinaryCheckedCase_AInt>{
        {AInt(0), AInt(0), AInt(1)},
        {AInt(1), AInt(1), AInt(1)},
        {AInt(1), AInt(1), AInt(1)},
        {AInt(2), AInt(2), AInt(1)},
        {AInt(2), AInt(4), AInt(2)},
        {AInt::Highest(), AInt::Highest(), AInt(1)},
        {AInt::Lowest(), AInt::Lowest(), AInt(1)},
        {AInt(1), AInt::Highest(), AInt::Highest()},
        {AInt(0), AInt(0), AInt::Highest()},
        {AInt(0), AInt(0), AInt::Lowest()},
        {OVERFLOW, AInt(123), AInt(0)},
        {OVERFLOW, AInt(-123), AInt(0)},
        ////////////////////////////////////////////////////////////////////////
    }));

using CheckedDivTest_AFloat = testing::TestWithParam<BinaryCheckedCase_AFloat>;
TEST_P(CheckedDivTest_AFloat, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    EXPECT_EQ(CheckedDiv(a, b), expect) << std::hex << "0x" << a << " - 0x" << b;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedDivTest_AFloat,
    CheckedDivTest_AFloat,
    testing::ValuesIn(std::vector<BinaryCheckedCase_AFloat>{
        {AFloat(0), AFloat(0), AFloat(1)},
        {AFloat(1), AFloat(1), AFloat(1)},
        {AFloat(1), AFloat(1), AFloat(1)},
        {AFloat(2), AFloat(2), AFloat(1)},
        {AFloat(2), AFloat(4), AFloat(2)},
        {AFloat::Highest(), AFloat::Highest(), AFloat(1)},
        {AFloat::Lowest(), AFloat::Lowest(), AFloat(1)},
        {AFloat(1), AFloat::Highest(), AFloat::Highest()},
        {AFloat(0), AFloat(0), AFloat::Highest()},
        {-AFloat(0), AFloat(0), AFloat::Lowest()},
        {OVERFLOW, AFloat(123), AFloat(0)},
        {OVERFLOW, AFloat(123), AFloat(-0)},
        {OVERFLOW, AFloat(-123), AFloat(0)},
        {OVERFLOW, AFloat(-123), AFloat(-0)},
        ////////////////////////////////////////////////////////////////////////
    }));

using TernaryCheckedCase = std::tuple<std::optional<AInt>, AInt, AInt, AInt>;

using CheckedMaddTest_AInt = testing::TestWithParam<TernaryCheckedCase>;
TEST_P(CheckedMaddTest_AInt, Test) {
    auto expect = std::get<0>(GetParam());
    auto a = std::get<1>(GetParam());
    auto b = std::get<2>(GetParam());
    auto c = std::get<3>(GetParam());
    EXPECT_EQ(CheckedMadd(a, b, c), expect)
        << std::hex << "0x" << a << " * 0x" << b << " + 0x" << c;
    EXPECT_EQ(CheckedMadd(b, a, c), expect)
        << std::hex << "0x" << a << " * 0x" << b << " + 0x" << c;
}
INSTANTIATE_TEST_SUITE_P(
    CheckedMaddTest_AInt,
    CheckedMaddTest_AInt,
    testing::ValuesIn(std::vector<TernaryCheckedCase>{
        {AInt(0), AInt(0), AInt(0), AInt(0)},
        {AInt(0), AInt(1), AInt(0), AInt(0)},
        {AInt(1), AInt(1), AInt(1), AInt(0)},
        {AInt(2), AInt(1), AInt(1), AInt(1)},
        {AInt(0), AInt(1), AInt(-1), AInt(1)},
        {AInt(-1), AInt(1), AInt(-2), AInt(1)},
        {AInt(-1), AInt(-1), AInt(1), AInt(0)},
        {AInt(2), AInt(2), AInt(1), AInt(0)},
        {AInt(-2), AInt(-2), AInt(1), AInt(0)},
        {AInt(0), AInt::Highest(), AInt(0), AInt(0)},
        {AInt(0), AInt::Lowest(), AInt(0), AInt(0)},
        {AInt(3), AInt(1), AInt(2), AInt(1)},
        {AInt(0x300), AInt(1), AInt(0x100), AInt(0x200)},
        {AInt(0x100), AInt(1), AInt(-0x100), AInt(0x200)},
        {AInt(0x20000), AInt(0x100), AInt(0x200), AInt(0)},
        {AInt(-0x20000), AInt(-0x100), AInt(0x200), AInt(0)},
        {AInt(0x4000000000000000ll), AInt(0x80000000ll), AInt(0x80000000ll), AInt(0)},
        {AInt(0x4000000000000000ll), AInt(-0x80000000ll), AInt(-0x80000000ll), AInt(0)},
        {AInt(0x1000000000000000ll), AInt(0x40000000ll), AInt(0x40000000ll), AInt(0)},
        {AInt(-0x1000000000000000ll), AInt(-0x40000000ll), AInt(0x40000000ll), AInt(0)},
        {AInt(0x100000000000000ll), AInt(0x1000000), AInt(0x100000000ll), AInt(0)},
        {AInt(0x2000000000000000ll), AInt(0x1000000000000000ll), AInt(2), AInt(0)},
        {AInt(-0x2000000000000000ll), AInt(0x1000000000000000ll), AInt(-2), AInt(0)},
        {AInt(-0x2000000000000000ll), AInt(-0x1000000000000000ll), AInt(2), AInt(0)},
        {AInt(-0x2000000000000000ll), AInt(0x1000000000000000ll), AInt(-2), AInt(0)},
        {AInt(0x4000000000000000ll), AInt(0x1000000000000000ll), AInt(4), AInt(0)},
        {AInt(-0x4000000000000000ll), AInt(0x1000000000000000ll), AInt(-4), AInt(0)},
        {AInt(-0x4000000000000000ll), AInt(-0x1000000000000000ll), AInt(4), AInt(0)},
        {AInt(-0x4000000000000000ll), AInt(0x1000000000000000ll), AInt(-4), AInt(0)},
        {AInt(-0x8000000000000000ll), AInt(0x1000000000000000ll), AInt(-8), AInt(0)},
        {AInt(-0x8000000000000000ll), AInt(-0x1000000000000000ll), AInt(8), AInt(0)},
        {AInt::Highest(), AInt(1), AInt(1), AInt(AInt::kHighestValue - 1)},
        {AInt::Lowest(), AInt(1), AInt(-1), AInt(AInt::kLowestValue + 1)},
        {AInt::Highest(), AInt(1), AInt(0x7fffffff00000000ll), AInt(0x00000000ffffffffll)},
        {AInt::Highest(), AInt(1), AInt::Highest(), AInt(0)},
        {AInt::Lowest(), AInt(1), AInt::Lowest(), AInt(0)},
        {OVERFLOW, AInt(0x1000000000000000ll), AInt(8), AInt(0)},
        {OVERFLOW, AInt(-0x1000000000000000ll), AInt(-8), AInt(0)},
        {OVERFLOW, AInt(0x800000000000000ll), AInt(0x10), AInt(0)},
        {OVERFLOW, AInt(0x80000000ll), AInt(0x100000000ll), AInt(0)},
        {OVERFLOW, AInt::Highest(), AInt::Highest(), AInt(0)},
        {OVERFLOW, AInt::Highest(), AInt::Lowest(), AInt(0)},
        {OVERFLOW, AInt(1), AInt(1), AInt::Highest()},
        {OVERFLOW, AInt(1), AInt(-1), AInt::Lowest()},
        {OVERFLOW, AInt(1), AInt(2), AInt::Highest()},
        {OVERFLOW, AInt(1), AInt(-2), AInt::Lowest()},
        {OVERFLOW, AInt(1), AInt(10000), AInt::Highest()},
        {OVERFLOW, AInt(1), AInt(-10000), AInt::Lowest()},
        {OVERFLOW, AInt(1), AInt::Highest(), AInt::Highest()},
        {OVERFLOW, AInt(1), AInt::Lowest(), AInt::Lowest()},
        {OVERFLOW, AInt(1), AInt::Highest(), AInt(1)},
        {OVERFLOW, AInt(1), AInt::Lowest(), AInt(-1)},
    }));

TINT_END_DISABLE_WARNING(CONSTANT_OVERFLOW);

}  // namespace
}  // namespace tint
