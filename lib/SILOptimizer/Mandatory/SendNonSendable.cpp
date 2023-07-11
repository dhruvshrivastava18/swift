#include "../../Sema/TypeCheckConcurrency.h"
#include "../../Sema/TypeChecker.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Type.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/PartitionUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "send-non-sendable"

using namespace swift;

namespace {

static const char *SEP_STR = "╾──────────────────────────────╼\n";

// SILApplyCrossesIsolation determines if a SIL instruction is an isolation
// crossing apply expressiong. This is done by checking its correspondence
// to an ApplyExpr AST node, and then checking the internal flags of that
// AST node to see if the ActorIsolationChecker determined it crossed isolation.
// It's possible this is brittle and a more nuanced check is needed, but this
// suffices for all cases tested so far.
static bool SILApplyCrossesIsolation(const SILInstruction *inst) {
  ApplyExpr *apply = inst->getLoc().getAsASTNode<ApplyExpr>();
  // if the instruction doesn't correspond to an ApplyExpr, then it can't
  // cross an isolation domain
  if (!apply)
    return false;
  return apply->getIsolationCrossing().has_value();
}

// PartitionOpTranslator is responsible for performing the translation from
// SILInstructions to PartitionOps. Not all SILInstructions have an effect on
// the region partition, and some have multiple effects - such as an application
// pairwise merging its arguments - so the core functions like
// translateSILBasicBlock map SILInstructions to std::vectors of PartitionOps.
// No more than a single instance of PartitionOpTranslator should be used for
// each SILFunction, as SILValues are assigned unique IDs through the nodeIDMap.
// Some special correspondences between SIL values are also tracked statefully
// by instances of this class, such as the "projection" relationship:
// instructions like begin_borrow and begin_access create effectively temporary
// values used for alternative access to base "projected" values. These are
// tracked to implement "write-through" semantics for assignments to projections
// when they're addresses.
//
// TODO: when translating basic blocks, optimizations might be possible
//       that reduce lists of PartitionOps to smaller, equivalent lists
class PartitionOpTranslator {
  SILFunction *function;
  ProtocolDecl *sendableProtocol;

  // nodeIDMap stores unique IDs for all SILNodes corresponding to
  // non-Sendable values. Implicit conversion from SILValue used pervasively.
  // ensure simplifyVal is called on SILValues before entering into this map
  llvm::DenseMap<const SILNode *, unsigned> nodeIDMap;
  unsigned nextNodeID = 0;

  // some values that AccessStorage claims are uniquely identified are still
  // captured (e.g. in a closure). This set is initialized upon
  // PartitionOpTranslator construction to store those values.
  // ensure simplifyVal is called on SILValues before entering into this map
  //
  // TODO: we could remember not just which values fit this description,
  //       but at what points in function flow they do, this would be more
  //       permissive, but I'm avoiding implementing it in case existing
  //       utilities would make it easier than handrolling
  std::set<SILValue> capturedUIValues;

  void initCapturedUIValues() {
    for (const SILBasicBlock &block: *function) {
      for (const SILInstruction &inst: block) {
        if (isApplyInst(inst)) {
          // add all nonsendable, uniquely identified arguments to applications
          // to capturedUIValues, because applications capture them
          for (SILValue val : inst.getOperandValues()) {
            if (isNonSendable(val) && isUniquelyIdentified(val))
              capturedUIValues.insert(simplifyVal(val));
          }
        }
      }
    }
  }

public:
  // create a new PartitionOpTranslator, all that's needed is the underlying
  // SIL function
  PartitionOpTranslator(SILFunction *function) :
                                                 function(function),
                                                 sendableProtocol(function->getASTContext()
                                                                      .getProtocol(KnownProtocolKind::Sendable)) {
    assert(sendableProtocol && "PartitionOpTranslators should only be created "
                               "in contexts in which the availability of the "
                               "Sendable protocol has already been checked.");
    initCapturedUIValues();
    LLVM_DEBUG(
      llvm::dbgs() << "Captured Uniquely Identified addresses for "
                     << function->getName() << ":\n";
      for (SILValue val : capturedUIValues)
            val->dump();

    );
  }

private:
  static inline bool isAddress(SILValue val) {
    return val->getType().isAddress();
  }

  static bool isApplyInst(const SILInstruction &inst) {
    return isa<ApplyInst, TryApplyInst, PartialApplyInst, BuiltinInst>(inst);
  }

  AccessStorage getAccessStorageFromAddr(SILValue val) {
    assert(isAddress(val));
    auto accessStorage = AccessStorage::compute(val);
    if (accessStorage) {
      auto definingInst = accessStorage.getRoot().getDefiningInstruction();
      if (definingInst &&
          isa<InitExistentialAddrInst, CopyValueInst>(definingInst))
        // look through these because AccessStorage does not
        return getAccessStorageFromAddr(definingInst->getOperand(0));
    }
    return accessStorage;
  }

  bool isUniquelyIdentified(SILValue val) {
    val = simplifyVal(val);
    if (!isAddress(val))
      return false;
    if (auto accessStorage = getAccessStorageFromAddr(val))
      return accessStorage.isUniquelyIdentified() &&
             !capturedUIValues.count(simplifyVal(val));
    return false;
  }

  // simplifyVal reduces an address-typed SILValue to the root SILValue
  // that it was derived from, reducing the set of values that must be
  // reasoned about by rendering two values that are projections/aliases the
  // same.
  // TODO: make usage of this more principled with a wrapper type SimplSILValue
  SILValue simplifyVal(SILValue val) {
    if (!isAddress(val))
      return getUnderlyingObject(val);
    if (auto accessStorage = getAccessStorageFromAddr(val)) {
      return accessStorage.getRoot();
    }
    return val;
  }

  bool nodeHasID(SILValue value) {
    value = simplifyVal(value);
    assert(isNonSendable(value) &&
           "only non-Sendable values should be entered in the map");
    return nodeIDMap.count(value);
  }

  // lookup the internally assigned unique ID of a SILValue, or create one
  unsigned lookupNodeID(SILValue value) {
    value = simplifyVal(value);
    assert(isNonSendable(value) &&
           "only non-Sendable values should be entered in the map");
    if (nodeIDMap.count(value)) {
      return nodeIDMap[value];
    }
    nodeIDMap[value] = nextNodeID;
    return nextNodeID++;
  }

  // check the passed type for sendability, special casing the type used for
  // raw pointers to ensure it is treated as non-Sendable and strict checking
  // is applied to it
  bool isNonSendableType(SILType type) {
    if (type.getASTType()->getKind() == TypeKind::BuiltinNativeObject) {
      // these are very unsafe... definitely not Sendable
      return true;
    }
    // consider caching this if it's a bottleneck
    return TypeChecker::conformsToProtocol(
        type.getASTType(), sendableProtocol, function->getParentModule())
        .hasMissingConformance(function->getParentModule());
  }

  // check the passed value for sendability, special casing for values known
  // to be functions or class methods because these can safely be treated as
  // Sendable despite not having true Sendable type
  bool isNonSendable(SILValue value) {
    value = simplifyVal(value);
    SILInstruction *defInst = value.getDefiningInstruction();
    if (defInst && isa<ClassMethodInst, FunctionRefInst>(defInst)) {
      // though these values are technically non-Sendable, we can safely
      // and consistently treat them as Sendable
      return false;
    }

    //consider caching this if it's a bottleneck
    return isNonSendableType(value->getType());
  }

  // used to statefully track the instruction currently being translated,
  // for insertion into generated PartitionOps
  SILInstruction *currentInstruction;

  // ===========================================================================
  // The following section of functions create fresh PartitionOps referencing
  // the current value of currentInstruction for ease of programming.

  std::vector<PartitionOp> AssignFresh(SILValue value) {
    return {PartitionOp::AssignFresh(lookupNodeID(value),
        currentInstruction)};
  }

  std::vector<PartitionOp> Assign(SILValue tgt, SILValue src) {
    assert(nodeHasID(src) &&
           "source value of assignment should already have been encountered");

    if (lookupNodeID(tgt) == lookupNodeID(src))
      return {}; //noop

    return {PartitionOp::Assign(lookupNodeID(tgt), lookupNodeID(src),
                                currentInstruction)};
  }

  std::vector<PartitionOp> Consume(SILValue value) {
    assert(nodeHasID(value) &&
           "consumed value should already have been encountered");

    return {PartitionOp::Consume(lookupNodeID(value),
                                currentInstruction)};
  }

  std::vector<PartitionOp> Merge(SILValue fst, SILValue snd) {
    assert(nodeHasID(fst) && nodeHasID(snd) &&
           "merged values should already have been encountered");

    if (lookupNodeID(fst) == lookupNodeID(snd))
      return {}; //noop

    return {PartitionOp::Merge(lookupNodeID(fst), lookupNodeID(snd),
                              currentInstruction)};
  }

  std::vector<PartitionOp> Require(SILValue value) {
    assert(nodeHasID(value) &&
           "required value should already have been encountered");
    return {PartitionOp::Require(lookupNodeID(value),
                                currentInstruction)};
  }
  // ===========================================================================

  // Get the vector of IDs corresponding to the arguments to the underlying
  // function, and the self parameter if there is one.
  std::vector<unsigned> getArgIDs() {
    std::vector<unsigned> argIDs;
    for (SILArgument *arg : function->getArguments()) {
      if (isNonSendableType(arg->getType())) {
        argIDs.push_back(lookupNodeID(arg));
      }
    }
    if (function->hasSelfParam() &&
        isNonSendableType(function->getSelfArgument()->getType())) {
      argIDs.push_back(lookupNodeID(function->getSelfArgument()));
    }
    return argIDs;
  }

public:
  // Create a partition that places all arguments from this function,
  // including self if available, into the same region, ensuring those
  // arguments get IDs in doing so. This Partition will be used as the
  // entry point for the full partition analysis.
  Partition getEntryPartition() {
    return Partition::singleRegion(getArgIDs());
  }

  // Get the vector of IDs that cannot be legally consumed at any point in
  // this function. Since we place all args and self in a single region right
  // now, it is only necessary to choose a single representative of the set.
  std::vector<unsigned> getNonConsumables() {
    if (const auto &argIDs = getArgIDs(); !argIDs.empty()) {
      return {argIDs.front()};
    }
    return {};
  }

  // ===========================================================================
  // The following section of functions wrap the more primitive Assign, Require,
  // Merge, etc functions that generate PartitionOps with more logic common to
  // the translations from source-level SILInstructions.

  std::vector<PartitionOp> translateSILApply(const SILInstruction *applyInst) {
    // accumulates the non-Sendable operands to this apply, include self and
    // the callee
    std::vector<SILValue> nonSendableOperands;

    for (SILValue operand : applyInst->getOperandValues())
      if (isNonSendable(operand))
        nonSendableOperands.push_back(operand);

    // check whether the result is non-Sendable
    bool nonSendableResult = isNonSendable(applyInst->getResult(0));

    std::vector<PartitionOp> translated;
    auto add_to_translation = [&](std::vector<PartitionOp> ops) {
      for (auto op : ops) translated.push_back(op);
    };

    if (SILApplyCrossesIsolation(applyInst)) {
      // for calls that cross isolation domains, consume all operands
      for (SILValue operand : nonSendableOperands)
        add_to_translation(Consume(operand));

      if (nonSendableResult) {
        // returning non-Sendable values from a cross isolation call will always
        // be an error, but doesn't need to be diagnosed here, so let's pretend
        // it gets a fresh region
        add_to_translation(AssignFresh(applyInst->getResult(0)));
      }
      return translated;
    }

    // for calls that do not cross isolation domains, merge all non-Sendable
    // operands and assign the result to the region of the operands

    if (nonSendableOperands.empty()) {
      // if no operands, a non-Sendable result gets a fresh region
      if (nonSendableResult) {
        add_to_translation(AssignFresh(applyInst->getResult(0)));
      }
      return translated;
    }

    if (nonSendableOperands.size() == 1) {
      // only one operand, so no merges required; just a `Require`
      add_to_translation(Require(nonSendableOperands.front()));
    } else {
      // merge all operands
      for (unsigned i = 1; i < nonSendableOperands.size(); i++) {
        add_to_translation(Merge(nonSendableOperands.at(i-1),
                                   nonSendableOperands.at(i)));
      }
    }

    // if the result is non-Sendable, assign it to the region of the operands
    if (nonSendableResult) {
      add_to_translation(
          Assign(applyInst->getResult(0), nonSendableOperands.front()));
    }

    return translated;
  }

  std::vector<PartitionOp> translateSILAssign(SILValue tgt, SILValue src) {
    // no work to be done if assignment is to a Sendable target
    if (!isNonSendable(tgt))
      return {};

    if (isNonSendable(src)) {
      // non-Sendable source and target of assignment, so just perform the assign
      return Assign(tgt, src);
    }

    // a non-Sendable value is extracted from a Sendable value,
    // seems to only occur when performing unchecked casts like
    // `unchecked_ref_cast`
    return AssignFresh(tgt);
  }

  // If the passed SILValue is NonSendable, then create a fresh region for it,
  // otherwise do nothing.
  std::vector<PartitionOp> translateSILAssignFresh(SILValue fresh) {
    if (isNonSendable(fresh)) {
      return AssignFresh(fresh);
    }
    return {};
  }

  std::vector<PartitionOp> translateSILMerge(SILValue fst, SILValue snd) {
    if (isNonSendable(fst) && isNonSendable(snd)) {
      return Merge(fst, snd);
    }
    return {};
  }

  std::vector<PartitionOp> translateSILStore(SILValue tgt, SILValue src) {
    if (isUniquelyIdentified(tgt)) {
      return translateSILAssign(tgt, src);
    }
    return translateSILMerge(tgt, src);
  }

  std::vector<PartitionOp> translateSILRequire(SILValue val) {
    if (isNonSendable(val)) {
      return Require(val);
    }
    return {};
  }
  // ===========================================================================

  // used to index the translations of SILInstructions performed
  int translationIndex = 0;

  // Some SILInstructions contribute to the partition of non-Sendable values
  // being analyzed. translateSILInstruction translate a SILInstruction
  // to its effect on the non-Sendable partition, if it has one.
  //
  // The current pattern of
  std::vector<PartitionOp> translateSILInstruction(SILInstruction *instruction) {
    translationIndex++;
    currentInstruction = instruction;

    // The following instructions are treated as assigning their result to a
    // fresh region.
    if (isa<AllocBoxInst,
            AllocRefInst,
            AllocStackInst,
            LiteralInst>(instruction)) {
      return translateSILAssignFresh(instruction->getResult(0));
    }

    // The following instructions are treated as assignments that are NOT
    // projections - this means that stores and other writes to their result
    // don't need to be written through to their operand. This could be because
    // the result is fundamentally a different value than the operand
    // (e.g. CopyValueInst, LoadInst, IndexAddrInst) or because the operand
    // is unusable once the result is defined (e.g. the unchecked casts)
    if (isa<AddressToPointerInst,
            BeginAccessInst,
            BeginBorrowInst,
            CopyValueInst,
            ConvertEscapeToNoEscapeInst,
            ConvertFunctionInst,
            IndexAddrInst,
            InitExistentialAddrInst,
            LoadInst,
            LoadBorrowInst,
            LoadWeakInst,
            PointerToAddressInst,
            RefElementAddrInst,
            StrongCopyUnownedValueInst,
            TailAddrInst,
            UncheckedAddrCastInst,
            UncheckedOwnershipConversionInst,
            UncheckedRefCastInst>(instruction)) {
      return translateSILAssign(
          instruction->getResult(0),
          instruction->getOperand(0));
    }

    // The following instructions are treated as non-projecting assignments,
    // but between their two operands instead of their operand and result.
    if (isa<CopyAddrInst,
            ExplicitCopyAddrInst,
            StoreInst,
            StoreBorrowInst,
            StoreWeakInst>(instruction)) {
      return translateSILStore(
          instruction->getOperand(1),
          instruction->getOperand(0));
    }

    // Handle applications
    if (isApplyInst(*instruction)) {
      return translateSILApply(instruction);
    }

    // Treat tuple destruction as a series of individual assignments
    if (auto destructTupleInst = dyn_cast<DestructureTupleInst>(instruction)) {
      std::vector<PartitionOp> translated;
      for (SILValue result : destructTupleInst->getResults())
        for (auto op : translateSILAssign(result, instruction->getOperand(0)))
          translated.push_back(op);
      return translated;
    }

    // Handle returns - require the operand to be non-consumed
    if (auto *returnInst = dyn_cast<ReturnInst>(instruction)) {
      return translateSILRequire(returnInst->getOperand());
    }

    if (isa<ClassMethodInst,
            DeallocBoxInst,
            DebugValueInst,
            DestroyAddrInst,
            DestroyValueInst,
            EndAccessInst,
            EndBorrowInst,
            EndLifetimeInst,
            HopToExecutorInst,
            MetatypeInst,
            DeallocStackInst>(instruction)) {
      //ignored instructions
      return {};
    }

    LLVM_DEBUG(
        llvm::errs().changeColor(llvm::raw_ostream::MAGENTA, true);
        llvm::errs() << "warning: ";
        llvm::errs().resetColor();
        llvm::errs() << "unhandled instruction kind "
                     << getSILInstructionName(instruction->getKind())
                     << "\n";
    );

    return {};
  }

  // translateSILBasicBlock reduces a SIL basic block to the vector of
  // transformations to the non-Sendable partition that it induces.
  // it accomplished this by sequentially calling translateSILInstruction
  std::vector<PartitionOp> translateSILBasicBlock(SILBasicBlock *basicBlock) {
    LLVM_DEBUG(
        llvm::dbgs() << SEP_STR
                     << "Compiling basic block for function "
                     << basicBlock->getFunction()->getName()
                     << ": ";
        basicBlock->dumpID();
        llvm::dbgs() << SEP_STR;
        basicBlock->dump();
        llvm::dbgs() << SEP_STR << "Results:\n";
    );

    //translate each SIL instruction to a PartitionOp, if necessary
    std::vector<PartitionOp> partitionOps;
    for (SILInstruction &instruction : *basicBlock) {
      auto ops = translateSILInstruction(&instruction);
      for (PartitionOp &op : ops) {
        partitionOps.push_back(op);

        LLVM_DEBUG(
            llvm::dbgs() << " ┌─┬─╼";
            instruction.dump();
            llvm::dbgs() << " │ └─╼  ";
            instruction.getLoc().getSourceLoc().printLineAndColumn(llvm::dbgs(), function->getASTContext().SourceMgr);
            llvm::dbgs() << " │ translation #" << translationIndex;
            llvm::dbgs() << "\n └─────╼ ";
            op.dump();
        );
      }
    }

    return partitionOps;
  }
};

// Instances of BlockPartitionState record all relevant state about a
// SILBasicBlock for the region-based Sendable checking fixpoint analysis.
// In particular, it records flags such as whether the block has been
// reached by the analysis, whether the prior round indicated that this block
// needs to be updated; it recorsd aux data such as the underlying basic block
// and associated PartitionOpTranslator; and most importantly of all it includes
// region partitions at entry and exit to this block - these are the stateful
// component of the fixpoint analysis.
class BlockPartitionState {
  friend class PartitionAnalysis;

  bool needsUpdate = false;
  bool reached = false;

  Partition entryPartition;
  Partition exitPartition;

  SILBasicBlock *basicBlock;
  PartitionOpTranslator &translator;

  bool blockPartitionOpsPopulated = false;
  std::vector<PartitionOp> blockPartitionOps = {};

  BlockPartitionState(SILBasicBlock *basicBlock,
                      PartitionOpTranslator &translator)
      : basicBlock(basicBlock), translator(translator) {}

  void ensureBlockPartitionOpsPopulated() {
    if (blockPartitionOpsPopulated) return;
    blockPartitionOpsPopulated = true;
    blockPartitionOps = translator.translateSILBasicBlock(basicBlock);
  }

  // recomputes the exit partition from the entry partition,
  // and returns whether this changed the exit partition.
  // Note that this method ignored errors that arise.
  bool recomputeExitFromEntry() {
    ensureBlockPartitionOpsPopulated();

    Partition workingPartition = entryPartition;
    for (auto partitionOp : blockPartitionOps) {
      // by calling apply without providing a `handleFailure` closure,
      // errors will be suppressed
      workingPartition.apply(partitionOp);
    }
    bool exitUpdated = !Partition::equals(exitPartition, workingPartition);
    exitPartition = workingPartition;
    return exitUpdated;
  }

  // apply each PartitionOP in this block to the entry partition,
  // but this time pass in a handleFailure closure that can be used
  // to diagnose any failures
  void diagnoseFailures(
      llvm::function_ref<void(const PartitionOp&, unsigned)>
          handleFailure,
      llvm::function_ref<void(const PartitionOp&, unsigned)>
          handleConsumeNonConsumable) {
    Partition workingPartition = entryPartition;
    for (auto &partitionOp : blockPartitionOps) {
      workingPartition.apply(partitionOp, handleFailure,
                             translator.getNonConsumables(),
                             handleConsumeNonConsumable);
    }
  }

public:
  // run the passed action on each partitionOp in this block. Action should
  // return true iff iteration should continue
  void forEachPartitionOp(llvm::function_ref<bool (const PartitionOp&)> action) const {
    for (const PartitionOp &partitionOp : blockPartitionOps)
      if (!action(partitionOp)) break;
  }

  const Partition& getEntryPartition() const {
    return entryPartition;
  }

  const Partition& getExitPartition() const {
    return exitPartition;
  }

  void dump() LLVM_ATTRIBUTE_USED {
    LLVM_DEBUG(
        llvm::dbgs() << SEP_STR
                     << "BlockPartitionState[reached="
                     << reached
                     << ", needsUpdate="
                     << needsUpdate
                     << "]\nid: ";
        basicBlock->dumpID();
        llvm::dbgs() << "entry partition: ";
        entryPartition.dump();
        llvm::dbgs() << "exit partition: ";
        exitPartition.dump();
        llvm::dbgs() << "instructions:\n┌──────────╼\n";
        for (PartitionOp op : blockPartitionOps) {
          llvm::dbgs() << "│ ";
          op.dump();
        }
        llvm::dbgs() << "└──────────╼\nSuccs:\n";
        for (auto succ : basicBlock->getSuccessorBlocks()) {
          llvm::dbgs() << "→";
          succ->dumpID();
        }
        llvm::dbgs() << "Preds:\n";
        for (auto pred : basicBlock->getPredecessorBlocks()) {
          llvm::dbgs() << "←";
          pred->dumpID();
        }
        llvm::dbgs()<< SEP_STR;
    );
  }
};

enum class LocalConsumedReasonKind {
  LocalConsumeInst,
  LocalNonConsumeInst,
  NonLocal
};

// Why was a value consumed, without looking across blocks?
// kind == LocalConsumeInst: a consume instruction in this block
// kind == LocalNonConsumeInst: an instruction besides a consume instruction
//                              in this block
// kind == NonLocal: an instruction outside this block
struct LocalConsumedReason {
  LocalConsumedReasonKind kind;
  llvm::Optional<PartitionOp> localInst;

  static LocalConsumedReason ConsumeInst(PartitionOp localInst) {
    assert(localInst.getKind() == PartitionOpKind::Consume);
    return LocalConsumedReason(LocalConsumedReasonKind::LocalConsumeInst, localInst);
  }

  static LocalConsumedReason NonConsumeInst() {
    return LocalConsumedReason(LocalConsumedReasonKind::LocalNonConsumeInst);
  }

  static LocalConsumedReason NonLocal() {
    return LocalConsumedReason(LocalConsumedReasonKind::NonLocal);
  }

  // 0-ary constructor only used in maps, where it's immediately overridden
  LocalConsumedReason() : kind(LocalConsumedReasonKind::NonLocal) {}

private:
  LocalConsumedReason(LocalConsumedReasonKind kind,
                 llvm::Optional<PartitionOp> localInst = {})
      : kind(kind), localInst(localInst) {}
};

// This class captures all available information about why a value's region was
// consumed. In particular, it contains a map `consumeOps` whose keys are
// "distances" and whose values are Consume PartitionOps that cause the target
// region to be consumed. Distances are (roughly) the number of times
// two different predecessor blocks had to have their exit partitions joined
// together to actually cause the target region to be consumed. If a Consume
// op only causes a target access to be invalid because of merging/joining
// that spans many different blocks worth of control flow, it is less likely
// to be informative, so distance is used as a heuristic to choose which
// access sites to display in diagnostics given a racy consumption.
class ConsumedReason {
  std::map<unsigned, std::vector<PartitionOp>> consumeOps;

  friend class ConsumeRequireAccumulator;

  // used only in asserts
  bool containsOp(const PartitionOp& op) {
    for (auto [_, vec] : consumeOps)
      for (auto vecOp : vec)
        if (op == vecOp)
          return true;
    return false;
  }

public:
  // a ConsumedReason is valid if it contains at least one consume instruction
  bool isValid() {
    for (auto [_, vec] : consumeOps)
      if (!vec.empty())
        return true;
    return false;
  }

  ConsumedReason() {}

  ConsumedReason(LocalConsumedReason localReason) {
    assert(localReason.kind == LocalConsumedReasonKind::LocalConsumeInst);
    consumeOps[0] = {localReason.localInst.value()};
  }

  void addConsumeOp(PartitionOp consumeOp, unsigned distance) {
    assert(consumeOp.getKind() == PartitionOpKind::Consume);
    assert(!containsOp(consumeOp));
    consumeOps[distance].push_back(consumeOp);
  }

  // merge in another consumedReason, adding the specified distane to all its ops
  void addOtherReasonAtDistance(const ConsumedReason &otherReason, unsigned distance) {
    for (auto &[otherDistance, otherConsumeOpsAtDistance] : otherReason.consumeOps)
      for (auto otherConsumeOp : otherConsumeOpsAtDistance)
        addConsumeOp(otherConsumeOp, distance + otherDistance);
  }
};

// This class is the "inverse" of a ConsumedReason: instead of associating
// accessing PartitionOps with their consumption sites, it associates
// consumption site Consume PartitionOps with the corresponding accesses.
// It is built up by repeatedly calling accumulateConsumedReason on
// ConsumedReasons, which "inverts" the contents of that reason and adds it to
// this class's tracking. Instead of a two-level map, we store a set that
// join together distances and access partitionOps so that we can use the
// ordering by lowest diagnostics for prioritized output
class ConsumeRequireAccumulator {
  struct PartitionOpAtDistance {
    PartitionOp partitionOp;
    unsigned distance;

    PartitionOpAtDistance(PartitionOp partitionOp, unsigned distance)
        : partitionOp(partitionOp), distance(distance) {}

    bool operator<(const PartitionOpAtDistance& other) const {
      if (distance != other.distance)
        return distance < other.distance;
      return partitionOp < other.partitionOp;
    }
  };

  // map consumptions to sets of requirements for that consumption, ordered so
  // that requirements at a smaller distance from the consumption come first
  std::map<PartitionOp, std::set<PartitionOpAtDistance>>
      requirementsForConsumptions;

public:
  ConsumeRequireAccumulator() {}

  void accumulateConsumedReason(PartitionOp requireOp, const ConsumedReason &consumedReason) {
    for (auto [distance, consumeOps] : consumedReason.consumeOps)
      for (auto consumeOp : consumeOps)
        requirementsForConsumptions[consumeOp].insert({requireOp, distance});
  }

  // for each consumption in this ConsumeRequireAccumulator, call the passed
  // processConsumeOp closure on it, followed immediately by calling the passed
  // processRequireOp closure on the top `numRequiresPerConsume` operations
  // that access ("require") the region consumed. Sorting is by lowest distance
  // first, then arbitrarily. This is used for final diagnostic output.
  void forEachConsumeRequire(
      unsigned numRequiresPerConsume,
      llvm::function_ref<void(const PartitionOp& consumeOp, unsigned numProcessed, unsigned numSkipped)>
          processConsumeOp,
      llvm::function_ref<void(const PartitionOp& requireOp)>
          processRequireOp) const {
    for (auto [consumeOp, requireOps] : requirementsForConsumptions) {
      unsigned numProcessed = std::min({requireOps.size(), (unsigned long) numRequiresPerConsume});
      processConsumeOp(consumeOp, numProcessed, requireOps.size() - numProcessed);
      unsigned numRequiresToProcess = numRequiresPerConsume;
      for (auto [requireOp, _] : requireOps) {
        // ensures at most numRequiresPerConsume requires are processed per consume
        if (numRequiresToProcess-- == 0) break;
        processRequireOp(requireOp);
      }
    }
  }
};

// A RaceTracer is used to accumulate the facts that the main phase of
// PartitionAnalysis generates - that certain values were required at certain
// points but were in consumed regions and thus should yield diagnostics -
// and traces those facts to the Consume operations that could have been
// responsible.
class RaceTracer {
  const BasicBlockData<BlockPartitionState>& blockStates;

  std::map<std::pair<SILBasicBlock *, unsigned>, ConsumedReason>
      consumedAtEntryReasons;

  // caches the reasons why consumedVals were consumed at the exit to basic blocks
  std::map<std::pair<SILBasicBlock *, unsigned>, LocalConsumedReason>
      consumedAtExitReasons;

  ConsumeRequireAccumulator accumulator;

  ConsumedReason findConsumedAtOpReason(unsigned consumedVal, PartitionOp op) {
    ConsumedReason consumedReason;
    findAndAddConsumedReasons(op.getSourceInst(true)->getParent(), consumedVal,
                              consumedReason, 0, op);
    return consumedReason;
  }

  void findAndAddConsumedReasons(
      SILBasicBlock * SILBlock, unsigned consumedVal,
      ConsumedReason &consumedReason, unsigned distance,
      llvm::Optional<PartitionOp> targetOp = {}) {
    assert(blockStates[SILBlock].getExitPartition().isConsumed(consumedVal));
    LocalConsumedReason localReason
        = findLocalConsumedReason(SILBlock, consumedVal, targetOp);
    switch (localReason.kind) {
    case LocalConsumedReasonKind::LocalConsumeInst:
      // there is a local consume in the pred block
      consumedReason.addConsumeOp(localReason.localInst.value(), distance);
      break;
    case LocalConsumedReasonKind::LocalNonConsumeInst:
      // ignore this case, that instruction will initiate its own search
      // for a consume op
      break;
    case LocalConsumedReasonKind::NonLocal:
      consumedReason.addOtherReasonAtDistance(
          // recursive call
          findConsumedAtEntryReason(SILBlock, consumedVal), distance);
    }
  }

  // find the reason why a value was consumed at entry to a block
  const ConsumedReason &findConsumedAtEntryReason(
      SILBasicBlock *SILBlock, unsigned consumedVal) {
    const BlockPartitionState &block = blockStates[SILBlock];
    assert(block.getEntryPartition().isConsumed(consumedVal));

    // check the cache
    if (consumedAtEntryReasons.count({SILBlock, consumedVal}))
      return consumedAtEntryReasons.at({SILBlock, consumedVal});

    // enter a dummy value in the cache to prevent circular call dependencies
    consumedAtEntryReasons[{SILBlock, consumedVal}] = ConsumedReason();

    auto entryTracks = [&](unsigned val) {
      return block.getEntryPartition().isTracked(val);
    };

    // this gets populated with all the tracked values at entry to this block
    // that are consumed at the exit to some predecessor block, associated
    // with the blocks that consume them
    std::map<unsigned, std::vector<SILBasicBlock *>> consumedInSomePred;
    for (SILBasicBlock *pred : SILBlock->getPredecessorBlocks())
      for (unsigned consumedVal
          : blockStates[pred].getExitPartition().getConsumedVals())
        if (entryTracks(consumedVal))
          consumedInSomePred[consumedVal].push_back(pred);

    // this gets populated with all the multi-edges between values tracked
    // at entry to this block that will be merged because of common regionality
    // in the exit partition of some predecessor. It is not transitively closed
    // because we want to count how many steps transitive merges require
    std::map<unsigned, std::set<unsigned>> singleStepJoins;
    for (SILBasicBlock *pred : SILBlock->getPredecessorBlocks())
      for (std::vector<unsigned> region
          : blockStates[pred].getExitPartition().getNonConsumedRegions()) {
        for (unsigned fst : region) for (unsigned snd : region)
            if (fst != snd && entryTracks(fst) && entryTracks(snd))
              singleStepJoins[fst].insert(snd);
      }

    // this gets populated with the distance, in terms of single step joins,
    // from the target consumedVal to other values that will get merged with it
    // because of the join at entry to this basic block
    std::map<unsigned, unsigned> distancesFromTarget;

    //perform BFS
    std::deque<std::pair<unsigned, unsigned>> processValues;
    processValues.push_back({consumedVal, 0});
    while (!processValues.empty()) {
      auto [currentTarget, currentDistance] = processValues.front();
      processValues.pop_front();
      distancesFromTarget[currentTarget] = currentDistance;
      for (unsigned nextTarget : singleStepJoins[currentTarget])
        if (!distancesFromTarget.count(nextTarget))
          processValues.push_back({nextTarget, currentDistance + 1});
    }

    ConsumedReason consumedReason;

    for (auto [predVal, distanceFromTarget] : distancesFromTarget) {
      for (SILBasicBlock *predBlock : consumedInSomePred[predVal]) {
        // one reason that our target consumedVal is consumed is that
        // predConsumedVal was consumed at exit of predBlock, and
        // distanceFromTarget merges had to be performed to make that
        // be a reason. Use this to build a ConsumedReason for consumedVal.
        findAndAddConsumedReasons(predBlock, predVal, consumedReason, distanceFromTarget);
      }
    }

    consumedAtEntryReasons[{SILBlock, consumedVal}] = std::move(consumedReason);

    return consumedAtEntryReasons[{SILBlock, consumedVal}];
  }

  LocalConsumedReason findLocalConsumedReason(
      SILBasicBlock * SILBlock, unsigned consumedVal,
      llvm::Optional<PartitionOp> targetOp = {}) {
    // if this is a query for consumption reason at block exit, check the cache
    if (!targetOp && consumedAtExitReasons.count({SILBlock, consumedVal}))
      return consumedAtExitReasons.at({SILBlock, consumedVal});

    const BlockPartitionState &block = blockStates[SILBlock];

    // if targetOp is null, we're checking why the value is consumed at exit,
    // so assert that it's actually consumed at exit
    assert(targetOp || block.getExitPartition().isConsumed(consumedVal));

    llvm::Optional<LocalConsumedReason> consumedReason;

    Partition workingPartition = block.getEntryPartition();

    // we're looking for a local reason, so if the value is consumed at entry,
    // revive it for the sake of this search
    if (workingPartition.isConsumed(consumedVal))
      workingPartition.apply(PartitionOp::AssignFresh(consumedVal));

    block.forEachPartitionOp([&](const PartitionOp& partitionOp) {
      if (targetOp && targetOp == partitionOp)
        return false; //break
      workingPartition.apply(partitionOp);
      if (workingPartition.isConsumed(consumedVal) && !consumedReason) {
        // this partitionOp consumes the target value
        if (partitionOp.getKind() == PartitionOpKind::Consume)
          consumedReason = LocalConsumedReason::ConsumeInst(partitionOp);
        else
          // a merge or assignment invalidated this, but that will be a separate
          // failure to diagnose, so we don't worry about it here
          consumedReason = LocalConsumedReason::NonConsumeInst();
      }
      if (!workingPartition.isConsumed(consumedVal) && consumedReason)
        // value is no longer consumed - e.g. reassigned or assigned fresh
        consumedReason = llvm::None;

      // continue walking block
      return true;
    });

    // if we failed to find a local consume reason, but the value was consumed
    // at entry to the block, then the reason is "NonLocal"
    if (!consumedReason && block.getEntryPartition().isConsumed(consumedVal))
      consumedReason = LocalConsumedReason::NonLocal();

    // if consumedReason is none, then consumedVal was not actually consumed
    assert(consumedReason);

    // if this is a query for consumption reason at block exit, update the cache
    if (!targetOp)
      return consumedAtExitReasons[std::pair{SILBlock, consumedVal}]
                 = consumedReason.value();

    return consumedReason.value();
  }

public:
  RaceTracer(const BasicBlockData<BlockPartitionState>& blockStates)
      : blockStates(blockStates) {}

  void traceUseOfConsumedValue(PartitionOp use, unsigned consumedVal) {
    accumulator.accumulateConsumedReason(
        use, findConsumedAtOpReason(consumedVal, use));
  }

  const ConsumeRequireAccumulator &getAccumulator() {
    return accumulator;
  }
};


// Instances of PartitionAnalysis perform the region-based Sendable checking.
// Internally, a PartitionOpTranslator is stored to perform the translation from
// SILInstructions to PartitionOps, then a fixed point iteration is run to
// determine the set of exit and entry partitions to each point satisfying
// the flow equations.
class PartitionAnalysis {
  PartitionOpTranslator translator;

  BasicBlockData<BlockPartitionState> blockStates;

  RaceTracer raceTracer;

  SILFunction *function;

  bool solved;

  const static int NUM_REQUIREMENTS_TO_DIAGNOSE = 5;

  // The constructor initializes each block in the function by compiling it
  // to PartitionOps, then seeds the solve method by setting `needsUpdate` to
  // true for the entry block
  PartitionAnalysis(SILFunction *fn)
      : translator(fn),
        blockStates(fn,
                    [this](SILBasicBlock *block) {
                      return BlockPartitionState(block, translator);
                    }),
        raceTracer(blockStates),
        function(fn),
        solved(false) {
    // initialize the entry block as needing an update, and having a partition
    // that places all its non-sendable args in a single region
    blockStates[fn->getEntryBlock()].needsUpdate = true;
    blockStates[fn->getEntryBlock()].entryPartition =
        translator.getEntryPartition();
  }

  void solve() {
    assert(!solved && "solve should only be called once");
    solved = true;

    bool anyNeedUpdate = true;
    while (anyNeedUpdate) {
      anyNeedUpdate = false;

      for (auto [block, blockState] : blockStates) {
        if (!blockState.needsUpdate) continue;

        // mark this block as no longer needing an update
        blockState.needsUpdate = false;

        // mark this block as reached by the analysis
        blockState.reached = true;

        // compute the new entry partition to this block
        Partition newEntryPartition;
        bool firstPred = true;

        // this loop computes the join of the exit partitions of all
        // predecessors of this block
        for (SILBasicBlock *predBlock : block.getPredecessorBlocks()) {
          BlockPartitionState &predState = blockStates[predBlock];
          // ignore predecessors that haven't been reached by the analysis yet
          if (!predState.reached) continue;

          if (firstPred) {
            firstPred = false;
            newEntryPartition = predState.exitPartition;
            continue;
          }

          newEntryPartition = Partition::join(
              newEntryPartition, predState.exitPartition);
        }

        // if we found predecessor blocks, then attempt to use them to
        // update the entry partition for this block, and abort this block's
        // update if the entry partition was not updated
        if (!firstPred) {
          // if the recomputed entry partition is the same as the current one,
          // perform no update
          if (Partition::equals(newEntryPartition, blockState.entryPartition))
            continue;

          // otherwise update the entry partition
          blockState.entryPartition = newEntryPartition;
        }

        // recompute this block's exit partition from its (updated) entry
        // partition, and if this changed the exit partition notify all
        // successor blocks that they need to update as well
        if (blockState.recomputeExitFromEntry()) {
          for (SILBasicBlock *succBlock : block.getSuccessorBlocks()) {
            anyNeedUpdate = true;
            blockStates[succBlock].needsUpdate = true;
          }
        }
      }
    }
  }

  // track the AST exprs that have already had diagnostics emitted about
  llvm::DenseSet<Expr *> emittedExprs;

  // check if a diagnostic has already been emitted about expr, only
  // returns true false for each expr
  bool hasBeenEmitted(Expr *expr) {
    if (auto castExpr = dyn_cast<ImplicitConversionExpr>(expr))
      return hasBeenEmitted(castExpr->getSubExpr());

    if (emittedExprs.contains(expr)) return true;
    emittedExprs.insert(expr);
    return false;
  }

  // used for generating informative diagnostics
  Expr *getExprForPartitionOp(const PartitionOp& op) {
    SILInstruction *sourceInstr = op.getSourceInst(true);
    Expr *expr = sourceInstr->getLoc().getAsASTNode<Expr>();
    assert(expr && "PartitionOp's source location should correspond to"
                   "an AST node");
    return expr;
  }

  // once the fixpoint has been solved for, run one more pass over each basic
  // block, reporting any failures due to requiring consumed regions in the
  // fixpoint state
  void diagnose() {
    assert(solved && "diagnose should not be called before solve");

    //llvm::dbgs() << function->getName() << "\n";
    RaceTracer tracer = blockStates;

    for (auto [_, blockState] : blockStates) {
      blockState.diagnoseFailures(
          /*handleFailure=*/
          [&](const PartitionOp& partitionOp, unsigned consumedVal) {
            raceTracer.traceUseOfConsumedValue(partitionOp, consumedVal);
            /*
             * This handles diagnosing accesses to consumed values at the site
             * of access instead of the site of consumption, as this is less
             * useful it will likely be eliminated, but leaving it for now
            auto expr = getExprForPartitionOp(partitionOp);
            if (hasBeenEmitted(expr)) return;
            function->getASTContext().Diags.diagnose(
                expr->getLoc(), diag::consumed_value_used);
                */
          },
          /*handleConsumeNonConsumable=*/
          [&](const PartitionOp& partitionOp, unsigned consumedVal) {
            auto expr = getExprForPartitionOp(partitionOp);
            function->getASTContext().Diags.diagnose(
                expr->getLoc(), diag::arg_region_consumed);
          });
    }

    raceTracer.getAccumulator().forEachConsumeRequire(
        NUM_REQUIREMENTS_TO_DIAGNOSE,
        /*diagnoseConsume=*/
        [&](const PartitionOp& consumeOp,
            unsigned numDisplayed, unsigned numHidden) {
          auto expr = getExprForPartitionOp(consumeOp);
          function->getASTContext().Diags.diagnose(
              expr->getLoc(), diag::consumption_yields_race,
              numDisplayed, numDisplayed != 1, numHidden > 0, numHidden);
        },
        /*diagnoseRequire=*/
        [&](const PartitionOp& requireOp) {
          auto expr = getExprForPartitionOp(requireOp);
          function->getASTContext().Diags.diagnose(
              expr->getLoc(), diag::possible_racy_access_site);
        });
  }

public:

  void dump() LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "\nPartitionAnalysis[fname=" << function->getName() << "]\n";

    for (auto [_, blockState] : blockStates) {
      blockState.dump();
    }
  }

  static void performForFunction(SILFunction *function) {
    auto analysis = PartitionAnalysis(function);
    analysis.solve();
    LLVM_DEBUG(
        llvm::dbgs() << "SOLVED: ";
        analysis.dump();
    );
    analysis.diagnose();
  }
};

// this class is the entry point to the region-based Sendable analysis,
// after certain checks are performed to ensure the analysis can be completed
// a PartitionAnalysis object is created and used to run the analysis.
class SendNonSendable : public SILFunctionTransform {

  // find any ApplyExprs in this function, and check if any of them make an
  // unsatisfied isolation jump, emitting appropriate diagnostics if so
  void run() override {
    SILFunction *function = getFunction();
    DeclContext *declContext = function->getDeclContext();

    // if this function does not correspond to a syntactic declContext, don't
    // check it
    // TODO: revisit this assumption, in particular perhaps verify no isolation
    // crossing applies occurs within these
    if (!declContext)
      return;

    // if the experimental feature DeferredSendableChecking is not provided,
    // do not perform this pass
    if (!function->getASTContext().LangOpts.hasFeature(
            Feature::DeferredSendableChecking))
      return;

    // if the Sendable protocol is not available, don't perform this checking
    // because we'd have to conservatively treat every value as non-Sendable
    // which would be very expensive
    if (!function->getASTContext().getProtocol(KnownProtocolKind::Sendable))
      return;

    PartitionAnalysis::performForFunction(function);
  }
};
}

/// This pass is known to depend on the following passes having run before it:
/// none so far
SILTransform *swift::createSendNonSendable() {
  return new SendNonSendable();
}
