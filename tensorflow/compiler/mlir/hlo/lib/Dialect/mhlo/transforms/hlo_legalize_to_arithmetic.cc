/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file implements logic for lowering HLO dialect to LHLO dialect.

#include <memory>
#include <utility>

#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir-hlo/Dialect/mhlo/transforms/PassDetail.h"
#include "mlir-hlo/Dialect/mhlo/transforms/passes.h"
#include "mlir-hlo/Dialect/mhlo/transforms/rewriters.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
namespace mhlo {
namespace {

struct RngGetAndUpdateStatePattern
    : public OpConversionPattern<mhlo::XlaRngGetAndUpdateStateOp> {
  using OpConversionPattern<
      mhlo::XlaRngGetAndUpdateStateOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::XlaRngGetAndUpdateStateOp op,
      XlaRngGetAndUpdateStateOpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    // Get various type related information
    auto loc = op->getLoc();

    const auto globalName = rewriter.getStringAttr("rng_state");
    constexpr auto initialSeed = 0x7012395ull;
    auto seedType = rewriter.getIntegerType(128);
    auto memrefType = MemRefType::get({}, seedType);

    auto resultType = op.getType();
    auto wordSize = resultType.getElementType().getIntOrFloatBitWidth();
    auto smallerIntType = rewriter.getIntegerType(wordSize);
    auto numElements = resultType.getNumElements();

    // Get or define the global variable
    auto* globalOp = mlir::SymbolTable::lookupNearestSymbolFrom(op, globalName);
    if (!globalOp) {
      auto* parent = mlir::SymbolTable::getNearestSymbolTable(op);
      OpBuilder::InsertionGuard g(rewriter);
      rewriter.setInsertionPointToStart(&parent->getRegions().front().front());

      const auto priv = rewriter.getStringAttr("private");
      auto initialValue = mlir::DenseElementsAttr::get(
          mlir::RankedTensorType::get({}, seedType),
          rewriter.getIntegerAttr(seedType, initialSeed));
      globalOp = rewriter.create<memref::GlobalOp>(
          loc, globalName, priv, memrefType, initialValue, /*constant=*/false,
          /*alignment=*/IntegerAttr());
    }
    assert(isa<memref::GlobalOp>(globalOp) &&
           "rng_state was defined somewhere else, not as a global op");

    // Get and update
    Value rngState =
        rewriter.create<memref::GetGlobalOp>(loc, memrefType, globalName);
    Value oldVal = rewriter.create<memref::LoadOp>(loc, rngState);
    Value delta = rewriter.create<arith::ConstantOp>(
        loc, rewriter.getIntegerAttr(seedType,
                                     static_cast<int64_t>(adaptor.delta())));
    Value newVal = rewriter.create<arith::AddIOp>(loc, oldVal, delta);
    (void)rewriter.create<memref::StoreOp>(loc, newVal, rngState);

    // Create the proper return type by packing the old seed into a tensor
    SmallVector<Value> pieces;
    for (int i = (numElements - 1) * wordSize; i >= 0; i -= wordSize) {
      Value shiftDistance = rewriter.create<arith::ConstantOp>(
          loc, rewriter.getIntegerAttr(seedType, i));
      pieces.push_back(rewriter.create<arith::TruncIOp>(
          loc, smallerIntType,
          rewriter.create<arith::ShRUIOp>(loc, oldVal, shiftDistance)));
    }

    // Obtain a tensor with the correct shape and bit widths but the incorrect
    // integer signedness, then cast the tensor to the correct signedness to
    // ensure that unrealized casts will successfully lower later.
    Value resultTensor = rewriter.create<tensor::FromElementsOp>(
        loc, mlir::RankedTensorType::get(resultType.getShape(), smallerIntType),
        pieces);
    rewriter.replaceOpWithNewOp<UnrealizedConversionCastOp>(op, resultType,
                                                            resultTensor);
    return success();
  }
};

struct HloLegalizeToArithmeticPass
    : public HloLegalizeToArithmeticPassBase<HloLegalizeToArithmeticPass> {
  void getDependentDialects(DialectRegistry& registry) const override {
    registry.insert<arith::ArithmeticDialect, memref::MemRefDialect,
                    tensor::TensorDialect>();
  }

 public:
  void runOnOperation() override {
    auto& context = getContext();
    RewritePatternSet patterns(&context);
    ConversionTarget target(context);

    populateHLOToArithmeticConversionPatterns(&patterns);

    target.addIllegalOp<XlaRngGetAndUpdateStateOp>();
    target.addLegalDialect<arith::ArithmeticDialect, BuiltinDialect,
                           memref::MemRefDialect, tensor::TensorDialect>();

    auto module = getOperation();
    if (failed(applyPartialConversion(module, target, std::move(patterns))))
      signalPassFailure();
  }
};

}  // namespace

void populateHLOToArithmeticConversionPatterns(RewritePatternSet* patterns) {
  patterns->add<RngGetAndUpdateStatePattern>(patterns->getContext());
}

std::unique_ptr<OperationPass<ModuleOp>> createLegalizeToArithmeticPass() {
  return std::make_unique<HloLegalizeToArithmeticPass>();
}

}  // namespace mhlo
}  // namespace mlir
