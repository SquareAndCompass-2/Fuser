// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on

#include <csrc/exceptions.h>
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include <test/utils.h>

#include <expr_evaluator.h>
#include <fusion.h>
#include <ops/all_ops.h>

namespace nvfuser {

class ExprEvalTest : public NVFuserTest {};

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::ThrowsMessage;

namespace {

inline void checkIntValue(
    ExpressionEvaluator& evaluator,
    Val* val,
    int64_t expected_value) {
  EXPECT_TRUE(val->isIntegralScalar());
  const auto actual_value = evaluator.evaluate(val);
  EXPECT_TRUE(actual_value.hasValue());
  EXPECT_EQ(actual_value, expected_value);
}

inline void checkConstEvaluate(
    const ExpressionEvaluator& evaluator,
    Val* val,
    at::Tensor expected_value) {
  auto actual_value = evaluator.evaluate(val);
  EXPECT_TRUE(expected_value.equal(actual_value.as<at::Tensor>()));
}

} // namespace

// Evaluate basic scalar operations with constant values
TEST_F(ExprEvalTest, Constants) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  ExpressionEvaluator evaluator;

  auto* a = IrBuilder::create<Val>(7L);
  auto* b = IrBuilder::create<Val>(3L);

  // Avoid div operation because it casts int operands to float
  checkIntValue(evaluator, neg(a), -7);
  checkIntValue(evaluator, add(a, b), 10);
  checkIntValue(evaluator, neg(mul(sub(a, b), add(a, b))), -40);
  checkIntValue(evaluator, mod(a, b), 1);
  checkIntValue(evaluator, ceilDiv(a, b), 3);
}

TEST_F(ExprEvalTest, Double) {
  auto fusion = std::make_unique<Fusion>();
  FusionGuard fg(fusion.get());
  auto ten = IrBuilder::create<Val>(10.0);
  auto two = IrBuilder::create<Val>(2.0);
  auto three = IrBuilder::create<Val>(3.0);
  auto val = castOp(DataType::Int, ceilDiv(sub(ten, two), three));
  auto reference = static_cast<int64_t>(std::ceil((10.0 - 2.0) / 3.0));
  EXPECT_EQ(reference, val->evaluate());
}

// Evaluate basic scalar operations with bound values
TEST_F(ExprEvalTest, Bindings) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  ExpressionEvaluator evaluator;

  auto* a = IrBuilder::create<Val>(DataType::Int);
  auto* b = IrBuilder::create<Val>(DataType::Int);
  auto* c = add(a, b);
  auto* d = neg(ceilDiv(c, b));
  auto* e = IrBuilder::create<Val>(0L);

  // trying to evaluate before binding should give empty results
  EXPECT_FALSE(evaluator.evaluate(a).hasValue());
  EXPECT_FALSE(evaluator.evaluate(d).hasValue());

  evaluator.bind(a, 7L);
  evaluator.bind(b, 3L);

  // can't bind to concrete values
  ASSERT_ANY_THROW(evaluator.bind(e, 100L));

  checkIntValue(evaluator, c, 10);
  checkIntValue(evaluator, sub(a, b), 4);
  checkIntValue(evaluator, mod(a, b), 1);
  checkIntValue(evaluator, ceilDiv(a, b), 3);
  checkIntValue(evaluator, d, -4);

  // Reset evaluation context
  evaluator = ExpressionEvaluator();

  evaluator.bind(a, 2L);
  evaluator.bind(b, 5L);

  checkIntValue(evaluator, c, 7);
  checkIntValue(evaluator, sub(a, b), -3);
  checkIntValue(evaluator, mod(a, b), 2);
  checkIntValue(evaluator, ceilDiv(a, b), 1);
  checkIntValue(evaluator, d, -2);
}

// Evaluate known values with const expression evaluator reference
TEST_F(ExprEvalTest, ConstReference) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  ExpressionEvaluator evaluator;
  auto tv0 = makeContigTensor(1);
  auto tv1 = makeContigTensor(1);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  auto t0 = at::randn({3}, options);
  auto t1 = at::randn({3}, options);

  evaluator.bind(tv0, t0);
  evaluator.bind(tv1, t1);

  checkConstEvaluate(evaluator, tv0, t0);
  checkConstEvaluate(evaluator, neg(tv0), -t0);
  checkConstEvaluate(evaluator, add(tv0, tv1), t0 + t1);
  checkConstEvaluate(evaluator, add(tv0, neg(tv1)), t0 - t1);
}

// Evaluate expressions in a simple IR
TEST_F(ExprEvalTest, Basic) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Create a non-trivial IR
  TensorView* tv0 = makeSymbolicTensor(2);
  TensorView* tv1 = makeSymbolicTensor(2);

  fusion.addInput(tv0);
  fusion.addInput(tv1);

  TensorView* tv2 = add(tv1, IrBuilder::create<Val>(2.0));
  TensorView* tv3 = add(tv0, tv2);

  fusion.addOutput(tv3);

  tv3->split(0, 4);

  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  // 1. Create an evaluator
  ExpressionEvaluator evaluator;

  // 2. Bind values
  //
  // IMPORTANT:
  // a. The bindings are only as stable as the Vals are in the fusion graph
  // b. You must use the original (rootDomain) extents
  //  (ex. `tv0->getRootDomain()[0]->extent()`
  //   instead of `tv0->axis(0)->extent()`)
  //
  evaluator.bind(tv0->getRootDomain()[0]->extent(), 6L);
  evaluator.bind(tv0->getRootDomain()[1]->extent(), 128L);
  evaluator.bind(tv1->getRootDomain()[0]->extent(), 6L);
  evaluator.bind(tv1->getRootDomain()[1]->extent(), 128L);

  // 3. Evaluate and check result values
  EXPECT_EQ(tv2->domain()->nDims(), 3);
  checkIntValue(evaluator, tv2->axis(0)->extent(), 2);
  checkIntValue(evaluator, tv2->axis(1)->extent(), 4);
  checkIntValue(evaluator, tv2->axis(2)->extent(), 128);

  EXPECT_EQ(tv3->domain()->nDims(), 3);
  checkIntValue(evaluator, tv3->axis(0)->extent(), 2);
  checkIntValue(evaluator, tv3->axis(1)->extent(), 4);
  checkIntValue(evaluator, tv3->axis(2)->extent(), 128);
}

// Evaluate expressions in a more complex IR
TEST_F(ExprEvalTest, Complex) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeSymbolicTensor(2);
  fusion.addInput(tv0);

  TensorView* tv1 = mul(tv0, IrBuilder::create<Val>(-1.0));
  TensorView* tv2 = add(tv0, IrBuilder::create<Val>(3.0));
  TensorView* tv3 = mul(tv0, IrBuilder::create<Val>(2.0));
  TensorView* tv4 = add(tv2, tv1);
  TensorView* tv5 = add(tv4, tv3);
  TensorView* tv6 = add(tv0, tv3);

  fusion.addOutput(tv5);
  fusion.addOutput(tv6);

  tv5->reorder({{-1, 0}});

  tv6->split(0, 5);
  tv5->merge(0);

  // 1. Create an evaluator
  ExpressionEvaluator evaluator;

  // 2. Bind values
  evaluator.bind(tv0->getRootDomain()[0]->extent(), 129L);
  evaluator.bind(tv0->getRootDomain()[1]->extent(), 127L);

  // Evaluate and check extent values
  EXPECT_EQ(tv0->domain()->nDims(), 2);
  checkIntValue(evaluator, tv0->axis(0)->extent(), 129);
  checkIntValue(evaluator, tv0->axis(1)->extent(), 127);

  EXPECT_EQ(tv3->domain()->nDims(), 2);
  checkIntValue(evaluator, tv3->axis(0)->extent(), 129);
  checkIntValue(evaluator, tv3->axis(1)->extent(), 127);

  EXPECT_EQ(tv4->domain()->nDims(), 2);
  checkIntValue(evaluator, tv4->axis(0)->extent(), 129);
  checkIntValue(evaluator, tv4->axis(1)->extent(), 127);

  EXPECT_EQ(tv5->domain()->nDims(), 1);
  checkIntValue(evaluator, tv5->axis(0)->extent(), 16383);

  EXPECT_EQ(tv6->domain()->nDims(), 3);
  checkIntValue(evaluator, tv6->axis(0)->extent(), 26);
  checkIntValue(evaluator, tv6->axis(1)->extent(), 5);
  checkIntValue(evaluator, tv6->axis(2)->extent(), 127);
}

// Evaluate expressions post lowering
TEST_F(ExprEvalTest, PostLower) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Create a non-trivial IR
  TensorView* tv0 = makeSymbolicTensor(2);
  TensorView* tv1 = makeSymbolicTensor(2);

  fusion.addInput(tv0);
  fusion.addInput(tv1);

  TensorView* tv2 = add(tv1, IrBuilder::create<Val>(2.0));
  TensorView* tv3 = add(tv0, tv2);

  fusion.addOutput(tv3);

  tv3->split(0, 4);

  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  auto* bid_x = add(tv3->axis(0)->extent(), IrBuilder::create<Val>(0L));
  auto* tid_x = add(tv3->axis(-1)->extent(), IrBuilder::create<Val>(0L));

  // Lower
  GpuLower gpulw(&fusion);

  // 1. Create an evaluation context
  ExpressionEvaluator evaluator;

  // 2. Bind values
  evaluator.bind(tv0->getRootDomain()[0]->extent(), 6L);
  evaluator.bind(tv0->getRootDomain()[1]->extent(), 128L);
  evaluator.bind(tv1->getRootDomain()[0]->extent(), 6L);
  evaluator.bind(tv1->getRootDomain()[1]->extent(), 128L);

  // 3. Evaluate and check result values
  EXPECT_EQ(tv2->domain()->nDims(), 3);
  checkIntValue(evaluator, tv2->axis(0)->extent(), 2);
  checkIntValue(evaluator, tv2->axis(1)->extent(), 4);
  checkIntValue(evaluator, tv2->axis(2)->extent(), 128);

  EXPECT_EQ(tv3->domain()->nDims(), 3);
  checkIntValue(evaluator, tv3->axis(0)->extent(), 2);
  checkIntValue(evaluator, tv3->axis(1)->extent(), 4);
  checkIntValue(evaluator, tv3->axis(2)->extent(), 128);

  checkIntValue(evaluator, bid_x, 2);
  checkIntValue(evaluator, tid_x, 128);
}

TEST_F(ExprEvalTest, Array) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  auto* a = IrBuilder::create<Val>(DataType::Int);
  auto* b = IrBuilder::create<Val>(DataType::Int);

  auto arr = IrBuilder::arrayExpr(std::vector<Val*>{a, b});

  auto aa = IrBuilder::getItemExpr(arr, fusion.zeroVal());
  auto bb = IrBuilder::getItemExpr(arr, fusion.oneVal());

  ExpressionEvaluator evaluator;
  evaluator.bind(a, 2L);
  evaluator.bind(b, 5L);

  auto arr_val = evaluator.evaluate(arr);
  std::vector<PolymorphicValue> arr_vec = {2L, 5L};
  EXPECT_EQ(arr_val, arr_vec);

  checkIntValue(evaluator, aa, 2L);
  checkIntValue(evaluator, bb, 5L);
}

TEST_F(ExprEvalTest, EmptyArray) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  EXPECT_THAT(
      [&]() {
        IrBuilder::create<Val>(
            std::vector<int64_t>{},
            ArrayType{std::make_shared<DataType>(DataType::Int), 2});
      },
      ThrowsMessage<nvfuser::nvfError>(HasSubstr("not compatible")));

  auto* a = IrBuilder::create<Val>(
      std::vector<int64_t>{},
      ArrayType{std::make_shared<DataType>(DataType::Int), 0});

  ExpressionEvaluator evaluator;
  auto arr_val = evaluator.evaluate(a);
  EXPECT_EQ(arr_val, std::vector<PolymorphicValue>{});
}

TEST_F(ExprEvalTest, Struct) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  struct A : public Struct {
    int64_t a;
    int64_t b;

    StructType type() const override {
      std::vector<StructType::FieldInfo> fields(2);
      fields.at(0) = {"a", std::make_shared<DataType>(DataType::Int), true};
      fields.at(1) = {"b", std::make_shared<DataType>(DataType::Int), false};
      return StructType::make<A>(fields, "A");
    }

    std::function<PolymorphicValue()> getter(
        const std::string& key) const override {
      if (key == "a") {
        return [this]() { return PolymorphicValue(a); };
      } else if (key == "b") {
        return [this]() { return PolymorphicValue(b); };
      } else {
        NVF_ERROR(false, "Invalid key");
      }
    }

    std::function<void(const PolymorphicValue&)> setter(
        const std::string& key) override {
      if (key == "a") {
        return [this](const PolymorphicValue& value) { a = (int64_t)value; };
      } else if (key == "b") {
        return [this](const PolymorphicValue& value) { b = (int64_t)value; };
      } else {
        NVF_ERROR(false, "Invalid key");
      }
    }
  };

  auto* a = IrBuilder::create<Val>(DataType::Int);
  auto* b = IrBuilder::create<Val>(DataType::Int);

  auto struct_ = IrBuilder::structExpr<A>({{"a", a}, {"b", b}}, "test_struct");

  auto aa = IrBuilder::getAttrExpr(struct_, "a");
  auto bb = IrBuilder::getAttrExpr(struct_, "b");

  ExpressionEvaluator evaluator;
  evaluator.bind(a, 2L);
  evaluator.bind(b, 5L);

  auto eval_struct = evaluator.evaluate(struct_);
  EXPECT_EQ((PolymorphicValue)(eval_struct->*"a"), 2L);
  EXPECT_EQ((PolymorphicValue)(eval_struct->*"b"), 5L);
  EXPECT_EQ(evaluator.evaluate(aa), 2L);
  EXPECT_EQ(evaluator.evaluate(bb), 5L);
}

TEST_F(ExprEvalTest, TensorEagerExecution) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeSymbolicTensor(2);
  TensorView* tv1 = makeSymbolicTensor(2);
  auto tv2 = add(tv0, tv1);

  at::Tensor a = at::rand({6, 128}).cuda();
  at::Tensor b = at::rand({6, 128}).cuda();

  ExpressionEvaluator evaluator;
  evaluator.bind(tv0, a);
  evaluator.bind(tv1, b);

  EXPECT_TRUE(at::allclose(evaluator.evaluate(tv2).as<at::Tensor>(), a + b));
}

TEST_F(ExprEvalTest, TensorMetaData) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv = makeSymbolicTensor(2);
  auto metadata = IrBuilder::metadataExpr(tv);
  auto data = IrBuilder::getAttrExpr(metadata, "data");
  auto sizes = IrBuilder::getAttrExpr(metadata, "logical_size");
  auto strides = IrBuilder::getAttrExpr(metadata, "alloc_stride");
  auto size0 = IrBuilder::getItemExpr(sizes, fusion.zeroVal());
  auto size1 = IrBuilder::getItemExpr(sizes, fusion.oneVal());
  auto stride0 = IrBuilder::getItemExpr(strides, fusion.zeroVal());
  auto stride1 = IrBuilder::getItemExpr(strides, fusion.oneVal());

  at::Tensor a = at::rand({6, 128}).cuda();

  ExpressionEvaluator evaluator;
  evaluator.bind(tv, a);

  std::vector<int64_t> sizes_vec = {6, 128};
  std::vector<int64_t> strides_vec = {128, 1};

  EXPECT_EQ(evaluator.evaluate(data), Pointer(a.data_ptr(), tv->dtype()));
  EXPECT_EQ((std::vector<int64_t>)evaluator.evaluate(sizes), sizes_vec);
  EXPECT_EQ((std::vector<int64_t>)evaluator.evaluate(strides), strides_vec);

  checkIntValue(evaluator, size0, 6L);
  checkIntValue(evaluator, size1, 128L);
  checkIntValue(evaluator, stride0, 128L);
  checkIntValue(evaluator, stride1, 1L);
}

TEST_F(ExprEvalTest, Validation) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  auto a = IrBuilder::create<Val>(DataType::Int);
  auto b = IrBuilder::create<Val>(DataType::Int);
  auto one = fusion.oneVal(DataType::Int);
  auto c = add(a, one);
  auto d = add(c, b);

  ExpressionEvaluator evaluator;
  evaluator.bind(a, 299792458L);
  evaluator.bind(b, 1L);

  EXPECT_THAT(
      [&]() { evaluator.bind(c, 4L, true); },
      ThrowsMessage<nvfuser::nvfError>(
          HasSubstr("Tried to bind to a value: ")));
  EXPECT_EQ(evaluator.evaluate(c), 299792459L);
  evaluator.bind(d, 299792460L, true);
}

TEST_F(ExprEvalTest, ReverseArray) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  auto input = IrBuilder::create<Val>(
      DataType(ArrayType{std::make_shared<DataType>(DataType::Int), 5}));
  auto output = IrBuilder::reverseArrayExpr(input);

  ExpressionEvaluator evaluator;
  evaluator.bind(input, std::vector<int64_t>{1, 2, 3, 4, 5});

  auto expect = std::vector<int64_t>{5, 4, 3, 2, 1};
  EXPECT_EQ((std::vector<int64_t>)evaluator.evaluate(output), expect);
}

//! Test evaluating ternary ops
TEST_F(ExprEvalTest, TernaryOps) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  ExpressionEvaluator evaluator;

  auto* a = IrBuilder::create<Val>(7.0);
  auto* b = IrBuilder::create<Val>(3.8);
  auto* c = IrBuilder::create<Val>(0.8);
  auto* d = IrBuilder::create<Val>(0.2);
  auto* t = IrBuilder::create<Val>(true);
  auto* f = IrBuilder::create<Val>(false);

  // Run once without PrecomputedValues, then once with
  for ([[maybe_unused]] auto i : c10::irange(2)) {
    EXPECT_EQ(evaluator.evaluate(clamp(b, c, a)), b->value());
    EXPECT_EQ(evaluator.evaluate(clamp(a, c, b)), b->value());
    EXPECT_EQ(evaluator.evaluate(clamp(d, c, b)), c->value());

    EXPECT_EQ(
        evaluator.evaluate(lerp(a, b, d)),
        a->value() + d->value() * (b->value() - a->value()));

    EXPECT_EQ(
        evaluator.evaluate(lerp(a, b, c)),
        a->value() + c->value() * (b->value() - a->value()));
    EXPECT_EQ(
        evaluator.evaluate(lerp(a, b, d)),
        a->value() + d->value() * (b->value() - a->value()));

    EXPECT_EQ(evaluator.evaluate(threshold(a, c, b)), a->value());
    EXPECT_EQ(evaluator.evaluate(threshold(d, c, b)), b->value());
    EXPECT_EQ(evaluator.evaluate(threshold(d, d, b)), b->value());

    EXPECT_EQ(evaluator.evaluate(where(t, a, b)), a->value());
    EXPECT_EQ(evaluator.evaluate(where(f, a, b)), b->value());

    // Now bind a PrecomputedValues
    PrecomputedValues pv(&fusion);
    evaluator.bindPrecomputedValues(&pv);
  }
}

TEST_F(ExprEvalTest, Permute) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* in =
      TensorViewBuilder().shape({-1, -1, -1, 6}).dtype(DataType::Float).build();
  fusion.addInput(in);
  TensorView* out = permute(in, {0, 3, 1, 2});
  fusion.addOutput(out);

  at::Tensor in_tensor =
      at::rand({256}).cuda().as_strided({2, 3, 4, 6}, {128, 32, 8, 1});

  ExpressionEvaluator evaluator;
  evaluator.bind(in, in_tensor);
  at::Tensor out_tensor = evaluator.evaluate(out).as<at::Tensor>();
  EXPECT_THAT(out_tensor.sizes(), ElementsAre(2, 6, 3, 4));
  EXPECT_THAT(out_tensor.strides(), ElementsAre(128, 1, 32, 8));
}

TEST_F(ExprEvalTest, ReshapePermuteReshape) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* in =
      TensorViewBuilder().shape({-1, 6}).dtype(DataType::Float).build();
  fusion.addInput(in);
  TensorView* out = reshape(
      in, {size(in, 0), IrBuilder::create<Val>(2), IrBuilder::create<Val>(3)});
  out = permute(out, {1, 2, 0});
  out = reshape(out, {IrBuilder::create<Val>(6), size(out, 2)});
  fusion.addOutput(out);

  at::Tensor in_tensor = at::rand({72}).cuda().as_strided({9, 6}, {8, 1});

  ExpressionEvaluator evaluator;
  evaluator.bind(in, in_tensor);
  at::Tensor out_tensor = evaluator.evaluate(out).as<at::Tensor>();

  EXPECT_EQ(in_tensor.data_ptr(), out_tensor.data_ptr());
  EXPECT_THAT(out_tensor.sizes(), ElementsAre(6, 9));
  EXPECT_THAT(out_tensor.strides(), ElementsAre(1, 8));
}

TEST_F(ExprEvalTest, Reshape_ForwardBroadcast) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* in = TensorViewBuilder()
                       .shape({-1, 6})
                       .dtype(DataType::Float)
                       .expanded({true, false})
                       .build();
  fusion.addInput(in);
  TensorView* out = reshape(
      in, {size(in, 0), IrBuilder::create<Val>(2), IrBuilder::create<Val>(3)});
  fusion.addOutput(out);

  at::Tensor in_tensor = at::rand({6}).cuda().as_strided({9, 6}, {0, 1});

  ExpressionEvaluator evaluator;
  evaluator.bind(in, in_tensor);
  at::Tensor out_tensor = evaluator.evaluate(out).as<at::Tensor>();

  EXPECT_EQ(in_tensor.data_ptr(), out_tensor.data_ptr());
  EXPECT_THAT(out_tensor.sizes(), ElementsAre(9, 2, 3));
  EXPECT_THAT(out_tensor.strides(), ElementsAre(0, 3, 1));
}

TEST_F(ExprEvalTest, Reshape_SplitBroadcast) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* in = TensorViewBuilder()
                       .shape({-1, 6})
                       .dtype(DataType::Float)
                       .expanded({false, true})
                       .build();
  fusion.addInput(in);
  TensorView* out = reshape(
      in, {size(in, 0), IrBuilder::create<Val>(2), IrBuilder::create<Val>(3)});
  fusion.addOutput(out);

  at::Tensor in_tensor = at::rand({9}).cuda().as_strided({9, 6}, {1, 0});

  ExpressionEvaluator evaluator;
  evaluator.bind(in, in_tensor);
  at::Tensor out_tensor = evaluator.evaluate(out).as<at::Tensor>();

  EXPECT_EQ(in_tensor.data_ptr(), out_tensor.data_ptr());
  EXPECT_THAT(out_tensor.sizes(), ElementsAre(9, 2, 3));
  EXPECT_THAT(out_tensor.strides(), ElementsAre(1, 0, 0));
}

TEST_F(ExprEvalTest, Reshape_MergeBroadcast) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* in = TensorViewBuilder()
                       .shape({-1, 6})
                       .dtype(DataType::Float)
                       .expanded({false, true})
                       .build();
  fusion.addInput(in);
  TensorView* out = flatten(in);
  fusion.addOutput(out);

  at::Tensor in_tensor = at::rand({9}).cuda().as_strided({9, 6}, {1, 0});

  ExpressionEvaluator evaluator;
  evaluator.bind(in, in_tensor);
  at::Tensor out_tensor = evaluator.evaluate(out).as<at::Tensor>();

  EXPECT_THAT(out_tensor.sizes(), ElementsAre(54));
  EXPECT_THAT(out_tensor.strides(), ElementsAre(1));
}

} // namespace nvfuser
