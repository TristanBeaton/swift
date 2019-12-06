//===--- Differentiation.cpp - SIL Automatic Differentiation --*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// SWIFT_ENABLE_TENSORFLOW
//
// Automatic differentiation utilities.
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "swift/SILOptimizer/Utils/Differentiation/Common.h"

namespace swift {
namespace autodiff {

raw_ostream &getADDebugStream() { return llvm::dbgs() << "[AD] "; }

bool isArrayLiteralIntrinsic(ApplyInst *ai) {
  return ai->hasSemantics("array.uninitialized_intrinsic");
}

ApplyInst *getAllocateUninitializedArrayIntrinsic(SILValue v) {
  if (auto *ai = dyn_cast<ApplyInst>(v))
    if (isArrayLiteralIntrinsic(ai))
      return ai;
  return nullptr;
}

ApplyInst *
getAllocateUninitializedArrayIntrinsicElementAddress(SILValue v) {
  // Find the `pointer_to_address` result, peering through `index_addr`.
  auto *ptai = dyn_cast<PointerToAddressInst>(v);
  if (auto *iai = dyn_cast<IndexAddrInst>(v))
    ptai = dyn_cast<PointerToAddressInst>(iai->getOperand(0));
  if (!ptai)
    return nullptr;
  // Return the `array.uninitialized_intrinsic` application, if it exists.
  if (auto *dti = dyn_cast<DestructureTupleInst>(
          ptai->getOperand()->getDefiningInstruction())) {
    if (auto *ai = getAllocateUninitializedArrayIntrinsic(dti->getOperand()))
      return ai;
  }
  return nullptr;
}

DestructureTupleInst *getSingleDestructureTupleUser(SILValue value) {
  bool foundDestructureTupleUser = false;
  if (!value->getType().is<TupleType>())
    return nullptr;
  DestructureTupleInst *result = nullptr;
  for (auto *use : value->getUses()) {
    if (auto *dti = dyn_cast<DestructureTupleInst>(use->getUser())) {
      assert(!foundDestructureTupleUser &&
             "There should only be one `destructure_tuple` user of a tuple");
      foundDestructureTupleUser = true;
      result = dti;
    }
  }
  return result;
}

void forEachApplyDirectResult(
    ApplyInst *ai, llvm::function_ref<void(SILValue)> resultCallback) {
  if (!ai->getType().is<TupleType>()) {
    resultCallback(ai);
    return;
  }
  if (auto *dti = getSingleDestructureTupleUser(ai))
    for (auto result : dti->getResults())
      resultCallback(result);
}

void collectAllFormalResultsInTypeOrder(SILFunction &function,
                                        SmallVectorImpl<SILValue> &results) {
  SILFunctionConventions convs(function.getLoweredFunctionType(),
                               function.getModule());
  auto indResults = function.getIndirectResults();
  auto *retInst = cast<ReturnInst>(function.findReturnBB()->getTerminator());
  auto retVal = retInst->getOperand();
  SmallVector<SILValue, 8> dirResults;
  if (auto *tupleInst =
          dyn_cast_or_null<TupleInst>(retVal->getDefiningInstruction()))
    dirResults.append(tupleInst->getElements().begin(),
                      tupleInst->getElements().end());
  else
    dirResults.push_back(retVal);
  unsigned indResIdx = 0, dirResIdx = 0;
  for (auto &resInfo : convs.getResults())
    results.push_back(resInfo.isFormalDirect() ? dirResults[dirResIdx++]
                                               : indResults[indResIdx++]);
}

void collectMinimalIndicesForFunctionCall(
    ApplyInst *ai, SILAutoDiffIndices parentIndices,
    const DifferentiableActivityInfo &activityInfo,
    SmallVectorImpl<SILValue> &results, SmallVectorImpl<unsigned> &paramIndices,
    SmallVectorImpl<unsigned> &resultIndices) {
  auto calleeFnTy = ai->getSubstCalleeType();
  auto calleeConvs = ai->getSubstCalleeConv();
  // Parameter indices are indices (in the callee type signature) of parameter
  // arguments that are varied or are arguments.
  // Record all parameter indices in type order.
  unsigned currentParamIdx = 0;
  for (auto applyArg : ai->getArgumentsWithoutIndirectResults()) {
    if (activityInfo.isActive(applyArg, parentIndices))
      paramIndices.push_back(currentParamIdx);
    ++currentParamIdx;
  }
  // Result indices are indices (in the callee type signature) of results that
  // are useful.
  SmallVector<SILValue, 8> directResults;
  forEachApplyDirectResult(ai, [&](SILValue directResult) {
    directResults.push_back(directResult);
  });
  auto indirectResults = ai->getIndirectSILResults();
  // Record all results and result indices in type order.
  results.reserve(calleeFnTy->getNumResults());
  unsigned dirResIdx = 0;
  unsigned indResIdx = calleeConvs.getSILArgIndexOfFirstIndirectResult();
  for (auto &resAndIdx : enumerate(calleeConvs.getResults())) {
    auto &res = resAndIdx.value();
    unsigned idx = resAndIdx.index();
    if (res.isFormalDirect()) {
      results.push_back(directResults[dirResIdx]);
      if (auto dirRes = directResults[dirResIdx])
        if (dirRes && activityInfo.isActive(dirRes, parentIndices))
          resultIndices.push_back(idx);
      ++dirResIdx;
    } else {
      results.push_back(indirectResults[indResIdx]);
      if (activityInfo.isActive(indirectResults[indResIdx], parentIndices))
        resultIndices.push_back(idx);
      ++indResIdx;
    }
  }
  // Make sure the function call has active results.
  assert(results.size() == calleeFnTy->getNumResults());
  assert(llvm::any_of(results, [&](SILValue result) {
    return activityInfo.isActive(result, parentIndices);
  }));
}

} // end namespace autodiff
} // end namespace swift