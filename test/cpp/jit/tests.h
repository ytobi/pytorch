#pragma once

/**
 * See README.md for instructions on how to add a new test.
 */
#include <c10/macros/Export.h>
#include <torch/csrc/WindowsTorchApiMacro.h>

namespace torch {
namespace jit {
#define TH_FORALL_TESTS(_)             \
  _(ADFormulas)                        \
  _(Attributes)                        \
  _(Blocks)                            \
  _(CallStack)                         \
  _(CallStackCaching)                  \
  _(CodeTemplate)                      \
  _(ControlFlow)                       \
  _(CreateAutodiffSubgraphs)           \
  _(CustomOperators)                   \
  _(CustomOperatorAliasing)            \
  _(IValueKWargs)                      \
  _(CustomFusion)                      \
  _(SchemaMatching)                    \
  _(Differentiate)                     \
  _(DifferentiateWithRequiresGrad)     \
  _(FromQualString)                    \
  _(InternedStrings)                   \
  _(PassManagement)                    \
  _(Proto)                             \
  _(RegisterFusionCachesKernel)        \
  _(SchemaParser)                      \
  _(TopologicalIndex)                  \
  _(TopologicalMove)                   \
  _(SubgraphUtils)                     \
  _(AliasAnalysis)                     \
  _(ContainerAliasing)                 \
  _(AliasRegistration)                 \
  _(WriteTracking)                     \
  _(Wildcards)                         \
  _(MemoryDAG)                         \
  _(IRParser)                          \
  _(ConstantPooling)                   \
  _(CleanUpPasses)                     \
  _(THNNConv)                          \
  _(ATenNativeBatchNorm)               \
  _(NoneSchemaMatch)                   \
  _(ClassParser)                       \
  _(UnifyTypes)                        \
  _(Profiler)                          \
  _(InsertAndEliminateRedundantGuards) \
  _(LoopPeeler)                        \
  _(InsertBailOuts)                    \
  _(PeepholeOptimize)                  \
  _(RecordFunction)                    \
  _(ThreadLocalDebugInfo)              \
  _(SubgraphMatching)                  \
  _(SubgraphRewriter)                  \
  _(ModuleClone)                       \
  _(ModuleConstant)                    \
  _(ModuleParameter)                   \
  _(ModuleCopy)                        \
  _(ModuleDeepcopy)                    \
  _(ModuleDeepcopyString)              \
  _(ModuleDeepcopyAliasing)            \
  _(ModuleDefine)                      \
  _(QualifiedName)                     \
  _(ClassImport)                       \
  _(ScriptObject)                      \
  _(ExtraFilesHookPreference)          \
  _(SaveExtraFilesHook)                \
  _(TypeTags)                          \
  _(DCE)                               \
  _(CustomFusionNestedBlocks)          \
  _(ClassDerive)                       \
  _(SaveLoadTorchbind)                 \
  _(ModuleInterfaceSerialization)      \
  _(ModuleCloneWithModuleInterface)    \
  _(ClassTypeAddRemoveAttr)            \
  _(Inliner)                           \
  _(LiteInterpreterAdd)                \
  _(LiteInterpreterConv)               \
  _(LiteInterpreterInline)             \
  _(LiteInterpreterTuple)              \
  _(LiteInterpreterUpsampleNearest2d)  \
  _(CommonAncestor)                    \
  _(AutogradSymbols)                   \
  _(DefaultArgTypeHinting)             \
  _(Futures)                           \
  _(TLSFutureCallbacks)                \
  _(MobileTypeParser)                  \
  _(LiteInterpreterBuiltinFunction)    \
  _(LiteInterpreterPrim)               \
  _(LiteInterpreterLoadOrigJit)        \
  _(LiteInterpreterWrongMethodName)    \
  _(LiteInterpreterParams)             \
  _(LiteInterpreterSetState)           \
  _(TorchbindIValueAPI)                \
  _(LiteInterpreterDict)               \
  _(MobileNamedParameters)             \
  _(MobileSaveLoadData)                \
  _(LiteSGD)                           \
  _(FusionAliasing)

#if defined(USE_CUDA)
#define TH_FORALL_TESTS_CUDA(_)   \
  _(ArgumentSpec)                 \
  _(CompleteArgumentSpec)         \
  _(Fusion)                       \
  _(GraphExecutor)                \
  _(ModuleConversion)             \
  _(Interp)                       \
  _(GPU_IrGraphGenerator)         \
  _(GPU_FusionDispatch)           \
  _(GPU_FusionClear)              \
  _(GPU_FusionCopy)               \
  _(GPU_FusionMove)               \
  _(GPU_FusionSimpleArith)        \
  _(GPU_FusionExprEvalConstants)  \
  _(GPU_FusionExprEvalBindings)   \
  _(GPU_FusionExprEvalBasic)      \
  _(GPU_FusionExprEvalComplex)    \
  _(GPU_FusionExprEvalPostLower)  \
  _(GPU_FusionSimpleTypePromote)  \
  _(GPU_FusionMutator)            \
  _(GPU_FusionRegister)           \
  _(GPU_FusionTopoSort)           \
  _(GPU_FusionTensor)             \
  _(GPU_FusionTVSplit)            \
  _(GPU_FusionTVMerge)            \
  _(GPU_FusionTVReorder)          \
  _(GPU_FusionEquality)           \
  _(GPU_FusionReplaceAll)         \
  _(GPU_FusionParser)             \
  _(GPU_FusionDependency)         \
  _(GPU_FusionCodeGen)            \
  _(GPU_FusionCodeGen2)           \
  _(GPU_FusionSimplePWise)        \
  _(GPU_FusionExecKernel)         \
  _(GPU_FusionForLoop)            \
  _(GPU_FusionLoopUnroll)         \
  _(GPU_FusionUnaryOps)           \
  _(GPU_FusionBinaryOps)          \
  _(GPU_FusionTernaryOps)         \
  _(GPU_FusionCompoundOps)        \
  _(GPU_FusionCastOps)            \
  _(GPU_FusionAdvancedComputeAt)  \
  _(GPU_FusionScalarInputs)       \
  _(GPU_FusionRFactorReplay)      \
  _(GPU_FusionReduction)          \
  _(GPU_FusionReduction2)         \
  _(GPU_FusionReduction3)         \
  _(GPU_FusionReduction4)         \
  _(GPU_FusionReduction5)         \
  _(GPU_FusionReductionTFT)       \
  _(GPU_FusionSimpleBCast)        \
  _(GPU_FusionSimpleGemm)         \
  _(GPU_FusionSoftmax)            \
  _(GPU_FusionGridReduction1)     \
  _(GPU_FusionGridReduction2)     \
  _(GPU_FusionGridReduction3dim1) \
  _(GPU_FusionGridReduction3dim0) \
  _(GPU_FusionGridReduction4)     \
  _(GPU_FusionGridReduction5)     \
  _(GPU_FusionGridReduction6)     \
  _(GPU_FusionNonRedAxisBind)     \
  _(GPU_FusionBCastInnerDim)      \
  _(GPU_FusionBCastReduce)        \
  _(GPU_FusionSplitBCast)         \
  _(GPU_FusionComputeAtExprOrder) \
  _(GPU_FusionZeroDimComputeAt)   \
  _(GPU_FusionZeroDimBroadcast)   \
  _(GPU_FusionZeroDimReduction)   \
  _(GPU_FusionReductionMultiConsumer)
#else
#define TH_FORALL_TESTS_CUDA(_) \
  _(ArgumentSpec)               \
  _(CompleteArgumentSpec)       \
  _(Fusion)                     \
  _(GraphExecutor)              \
  _(ModuleConversion)           \
  _(Interp)
#endif

#define DECLARE_JIT_TEST(name) void test##name();
TH_FORALL_TESTS(DECLARE_JIT_TEST)
TH_FORALL_TESTS_CUDA(DECLARE_JIT_TEST)
#undef DECLARE_JIT_TEST

// This test is special since it requires prior setup in python.
// So it is not part of the general test list (which is shared between the gtest
// and python test runners), but is instead invoked manually by the
// torch_python_test.cpp
void testEvalModeForLoadedModule();
void testSerializationInterop();
void testTorchSaveError();

} // namespace jit
} // namespace torch
