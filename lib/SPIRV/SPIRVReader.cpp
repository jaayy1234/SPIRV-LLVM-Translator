//===- SPIRVReader.cpp � Converts SPIR-V to LLVM -----------------*- C++ -*-===//
//
//                     The LLVM/SPIR-V Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements conversion of SPIR-V binary to LLVM IR.
///
//===----------------------------------------------------------------------===//
#include "SPIRVUtil.h"
#include "SPIRVType.h"
#include "SPIRVValue.h"
#include "SPIRVModule.h"
#include "SPIRVFunction.h"
#include "SPIRVBasicBlock.h"
#include "SPIRVInstruction.h"
#include "SPIRVExtInst.h"
#include "SPIRVInternal.h"
#include "SPIRVMDBuilder.h"
#include "OCLUtil.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"
#include "llvm/PassManager.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>

#define DEBUG_TYPE "spirv"

using namespace std;
using namespace llvm;
using namespace SPIRV;
using namespace OCLUtil;

namespace SPIRV{

cl::opt<bool> SPIRVEnableStepExpansion("spirv-expand-step", cl::init(true),
  cl::desc("Enable expansion of OpenCL step and smoothstep function"));

cl::opt<bool> SPIRVGenKernelArgNameMD("spirv-gen-kernel-arg-name-md",
    cl::init(false), cl::desc("Enable generating OpenCL kernel argument name "
    "metadata"));

// Prefix for placeholder global variable name.
const char* kPlaceholderPrefix = "placeholder.";

// Save the translated LLVM before validation for debugging purpose.
static bool DbgSaveTmpLLVM = true;
static const char *DbgTmpLLVMFileName = "_tmp_llvmbil.ll";

typedef std::pair < unsigned, AttributeSet > AttributeWithIndex;

static bool
isOpenCLKernel(SPIRVFunction *BF) {
  return BF->getModule()->isEntryPoint(ExecutionModelKernel, BF->getId());
}

static void
dumpLLVM(Module *M, const std::string &FName) {
  std::error_code EC;
  raw_fd_ostream FS(FName, EC, sys::fs::F_None);
  if (EC) {
    FS << *M;
    FS.close();
  }
}

static MDNode*
getMDNodeStringIntVec(LLVMContext *Context, const std::string& Str,
    const std::vector<SPIRVWord>& IntVals) {
  std::vector<Metadata*> ValueVec;
  ValueVec.push_back(MDString::get(*Context, Str));
  for (auto &I:IntVals)
    ValueVec.push_back(ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context), I)));
  return MDNode::get(*Context, ValueVec);
}

static MDNode*
getMDTwoInt(LLVMContext *Context, unsigned Int1, unsigned Int2) {
  std::vector<Metadata*> ValueVec;
  ValueVec.push_back(ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context), Int1)));
  ValueVec.push_back(ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context), Int2)));
  return MDNode::get(*Context, ValueVec);
}

static MDNode*
getMDString(LLVMContext *Context, const std::string& Str) {
  std::vector<Metadata*> ValueVec;
  if (!Str.empty())
    ValueVec.push_back(MDString::get(*Context, Str));
  return MDNode::get(*Context, ValueVec);
}

static void
addOCLVersionMetadata(LLVMContext *Context, Module *M,
    const std::string &MDName, unsigned Major, unsigned Minor) {
  NamedMDNode *NamedMD = M->getOrInsertNamedMetadata(MDName);
  NamedMD->addOperand(getMDTwoInt(Context, Major, Minor));
}

static void
addNamedMetadataString(LLVMContext *Context, Module *M,
    const std::string &MDName, const std::string &Str) {
  NamedMDNode *NamedMD = M->getOrInsertNamedMetadata(MDName);
  NamedMD->addOperand(getMDString(Context, Str));
}

static void
addOCLKernelArgumentMetadata(LLVMContext *Context,
  std::vector<llvm::Metadata*> &KernelMD, const std::string &MDName,
    SPIRVFunction *BF, std::function<Metadata *(SPIRVFunctionParameter *)>Func){
  std::vector<Metadata*> ValueVec;
    ValueVec.push_back(MDString::get(*Context, MDName));
  BF->foreachArgument([&](SPIRVFunctionParameter *Arg) {
    ValueVec.push_back(Func(Arg));
  });
  KernelMD.push_back(MDNode::get(*Context, ValueVec));
}

class SPIRVToLLVMDbgTran {
public:
  SPIRVToLLVMDbgTran(SPIRVModule *TBM, Module *TM)
  :BM(TBM), M(TM), SpDbg(BM), Builder(*M){
    Enable = BM->hasDebugInfo();
  }

  void createCompileUnit() {
    if (!Enable)
      return;
    auto File = SpDbg.getEntryPointFileStr(ExecutionModelKernel, 0);
    std::string BaseName;
    std::string Path;
    splitFileName(File, BaseName, Path);
    Builder.createCompileUnit(dwarf::DW_LANG_C99,
      BaseName, Path, "spirv", false, "", 0, "", DIBuilder::LineTablesOnly);
  }

  void addDbgInfoVersion() {
    if (!Enable)
      return;
    M->addModuleFlag(Module::Warning, "Dwarf Version",
        dwarf::DWARF_VERSION);
    M->addModuleFlag(Module::Warning, "Debug Info Version",
        DEBUG_METADATA_VERSION);
  }

  DIFile getDIFile(const std::string &FileName){
    return getOrInsert(FileMap, FileName, [=](){
      std::string BaseName;
      std::string Path;
      splitFileName(FileName, BaseName, Path);
      if (!BaseName.empty())
        return Builder.createFile(BaseName, Path);
      else
        return DIFile();
    });
  }

  DISubprogram getDISubprogram(SPIRVFunction *SF, Function *F){
    return getOrInsert(FuncMap, F, [=](){
      auto DF = getDIFile(SpDbg.getFunctionFileStr(SF));
      auto FN = F->getName();
      auto LN = SpDbg.getFunctionLineNo(SF);
      Metadata *Args[] = {DIType()};
      return Builder.createFunction(DF, FN, FN, DF, LN,
        Builder.createSubroutineType(DF, Builder.getOrCreateTypeArray(Args)),
        Function::isInternalLinkage(F->getLinkage()),
        true, LN, 0, 0, NULL, NULL, NULL);
    });
  }

  void transDbgInfo(SPIRVValue *SV, Value *V) {
    if (!Enable || !SV->hasLine())
      return;
    if (auto I = dyn_cast<Instruction>(V)) {
      assert(SV->isInst() && "Invalid instruction");
      auto SI = static_cast<SPIRVInstruction *>(SV);
      assert(SI->getParent() &&
             SI->getParent()->getParent() &&
             "Invalid instruction");
      auto Line = SV->getLine();
      I->setDebugLoc(DebugLoc::get(Line->getLine(), Line->getColumn(),
          getDISubprogram(SI->getParent()->getParent(),
              I->getParent()->getParent())));
    }
  }

  void finalize() {
    if (!Enable)
      return;
    Builder.finalize();
  }

private:
  SPIRVModule *BM;
  Module *M;
  SPIRVDbgInfo SpDbg;
  DIBuilder Builder;
  bool Enable;
  std::unordered_map<std::string, DIFile> FileMap;
  std::unordered_map<Function *, DISubprogram> FuncMap;

  void splitFileName(const std::string &FileName,
      std::string &BaseName,
      std::string &Path) {
    auto Loc = FileName.find_last_of("/\\");
    if (Loc != std::string::npos) {
      BaseName = FileName.substr(Loc + 1);
      Path = FileName.substr(0, Loc);
    } else {
      BaseName = FileName;
      Path = ".";
    }
  }
};

class SPIRVToLLVM {
public:
  SPIRVToLLVM(Module *LLVMModule, SPIRVModule *TheSPIRVModule)
    :M(LLVMModule), BM(TheSPIRVModule), DbgTran(BM, M){
    if (M)
      Context = &M->getContext();
  }

  std::string getOCLBuiltinName(SPIRVInstruction* BI);
  std::string getOCLConvertBuiltinName(SPIRVInstruction *BI);
  std::string getOCLGenericCastToPtrName(SPIRVInstruction *BI);

  Type *transType(SPIRVType *BT);
  std::string transTypeToOCLTypeName(SPIRVType *BT, bool IsSigned = true);
  std::vector<Type *> transTypeVector(const std::vector<SPIRVType *>&);
  bool translate();
  bool transAddressingModel();

  Value *transValue(SPIRVValue *, Function *F, BasicBlock *,
      bool CreatePlaceHolder = true);
  Value *transValueWithoutDecoration(SPIRVValue *, Function *F, BasicBlock *,
      bool CreatePlaceHolder = true);
  bool transDecoration(SPIRVValue *, Value *);
  bool transAlign(SPIRVValue *, Value *);
  Instruction *transOCLBuiltinFromExtInst(SPIRVExtInst *BC, BasicBlock *BB);
  std::vector<Value *> transValue(const std::vector<SPIRVValue *>&, Function *F,
      BasicBlock *);
  Function *transFunction(SPIRVFunction *F);
  bool transFPContractMetadata();
  bool transKernelMetadata();
  bool transSourceLanguage();
  bool transSourceExtension();
  void transGeneratorMD();
  Value *transConvertInst(SPIRVValue* BV, Function* F, BasicBlock* BB);
  Instruction *transBuiltinFromInst(const std::string& FuncName,
      SPIRVInstruction* BI, BasicBlock* BB);
  Instruction *transOCLBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transSPIRVBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transOCLBarrierFence(SPIRVInstruction* BI, BasicBlock *BB);
  void transOCLVectorLoadStore(std::string& UnmangledName,
      std::vector<SPIRVWord> &BArgs);

  /// Post-process translated LLVM module for OpenCL.
  bool postProcessOCL();

  /// \brief Post-process OpenCL builtin functions returning struct type.
  ///
  /// Some OpenCL builtin functions are translated to SPIR-V instructions with
  /// struct type result, e.g. NDRange creation functions. Such functions
  /// need to be post-processed to return the struct through sret argument.
  bool postProcessOCLBuiltinReturnStruct(Function *F);

  /// \brief Post-process OpenCL builtin functions having block argument.
  ///
  /// These functions are translated to functions with function pointer type
  /// argument first, then post-processed to have block argument.
  bool postProcessOCLBuiltinWithFuncPointer(Function *F,
      Function::arg_iterator I);

  /// \brief Post-process OpenCL builtin functions having array argument.
  ///
  /// These functions are translated to functions with array type argument
  /// first, then post-processed to have pointer arguments.
  bool postProcessOCLBuiltinWithArrayArguments(Function *F,
      const std::string &DemangledName);

  /// \brief Post-process OpImageSampleExplicitLod.
  ///   sampled_image = __spirv_SampledImage__(image, sampler);
  ///   return __spirv_ImageSampleExplicitLod__(sampled_image, ...);
  /// =>
  ///   read_image(image, sampler, ...)
  /// \return transformed call instruction.
  CallInst *postProcessOCLReadImage(SPIRVInstruction *BI, CallInst *CI,
      const std::string &DemangledName);

  /// \brief Expand OCL builtin functions with scalar argument, e.g.
  /// step, smoothstep.
  /// gentype func (fp edge, gentype x)
  /// =>
  /// gentype func (gentype edge, gentype x)
  /// \return transformed call instruction.
  CallInst *expandOCLBuiltinWithScalarArg(CallInst* CI,
      const std::string &FuncName);

  typedef DenseMap<SPIRVType *, Type *> SPIRVToLLVMTypeMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVToLLVMValueMap;
  typedef DenseMap<SPIRVFunction *, Function *> SPIRVToLLVMFunctionMap;
  typedef DenseMap<GlobalVariable *, SPIRVBuiltinVariableKind> BuiltinVarMap;

  // A SPIRV value may be translated to a load instruction of a placeholder
  // global variable. This map records load instruction of these placeholders
  // which are supposed to be replaced by the real values later.
  typedef std::map<SPIRVValue *, LoadInst*> SPIRVToLLVMPlaceholderMap;
private:
  Module *M;
  BuiltinVarMap BuiltinGVMap;
  LLVMContext *Context;
  SPIRVModule *BM;
  SPIRVToLLVMTypeMap TypeMap;
  SPIRVToLLVMValueMap ValueMap;
  SPIRVToLLVMFunctionMap FuncMap;
  SPIRVToLLVMPlaceholderMap PlaceholderMap;
  SPIRVToLLVMDbgTran DbgTran;

  Type *mapType(SPIRVType *BT, Type *T) {
    SPIRVDBG(dbgs() << *T << '\n';)
    TypeMap[BT] = T;
    return T;
  }

  // If a value is mapped twice, the existing mapped value is a placeholder,
  // which must be a load instruction of a global variable whose name starts
  // with kPlaceholderPrefix.
  Value *mapValue(SPIRVValue *BV, Value *V) {
    auto Loc = ValueMap.find(BV);
    if (Loc != ValueMap.end()) {
      if (Loc->second == V)
        return V;
      auto LD = dyn_cast<LoadInst>(Loc->second);
      auto Placeholder = dyn_cast<GlobalVariable>(LD->getPointerOperand());
      assert (LD && Placeholder &&
          Placeholder->getName().startswith(kPlaceholderPrefix) &&
          "A value is translated twice");
      // Replaces placeholders for PHI nodes
      LD->replaceAllUsesWith(V);
      LD->dropAllReferences();
      LD->removeFromParent();
      Placeholder->dropAllReferences();
      Placeholder->removeFromParent();
    }
    ValueMap[BV] = V;
    return V;
  }

  bool isSPIRVBuiltinVariable(GlobalVariable *GV,
      SPIRVBuiltinVariableKind *Kind = nullptr) {
    auto Loc = BuiltinGVMap.find(GV);
    if (Loc == BuiltinGVMap.end())
      return false;
    if (Kind)
      *Kind = Loc->second;
    return true;
  }
  // OpenCL function always has NoUnwound attribute.
  // Change this if it is no longer true.
  bool isFuncNoUnwind() const { return true;}
  bool isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction *BI) const;
  bool transOCLBuiltinsFromVariables();
  bool transOCLBuiltinFromVariable(GlobalVariable *GV,
      SPIRVBuiltinVariableKind Kind);
  MDString *transOCLKernelArgTypeName(SPIRVFunctionParameter *);

  Value *mapFunction(SPIRVFunction *BF, Function *F) {
    SPIRVDBG(spvdbgs() << "[mapFunction] " << *BF << " -> ";
      dbgs() << *F << '\n';)
    FuncMap[BF] = F;
    return F;
  }

  Value *getTranslatedValue(SPIRVValue *BV);
  Type *getTranslatedType(SPIRVType *BT);

  SPIRVErrorLog &getErrorLog() {
    return BM->getErrorLog();
  }

  void setCallingConv(CallInst *Call) {
    Function *F = Call->getCalledFunction();
    Call->setCallingConv(F->getCallingConv());
  }

  void setAttrByCalledFunc(CallInst *Call);
  Type *transFPType(SPIRVType* T);
  BinaryOperator *transShiftLogicalBitwiseInst(SPIRVValue* BV, BasicBlock* BB,
      Function* F);
  void transFlags(llvm::Value* V);
  Instruction *transCmpInst(SPIRVValue* BV, BasicBlock* BB, Function* F);
  void transOCLBuiltinFromInstPreproc(SPIRVInstruction* BI, Type *&RetTy,
      std::vector<SPIRVValue *> &Args);
  Instruction* transOCLBuiltinPostproc(SPIRVInstruction* BI,
      CallInst* CI, BasicBlock* BB, const std::string &DemangledName);
  std::string transOCLImageTypeName(SPIRV::SPIRVTypeImage* ST);
  std::string transOCLSampledImageTypeName(SPIRV::SPIRVTypeSampledImage* ST);
  std::string transOCLPipeTypeName(SPIRV::SPIRVTypePipe* ST);
  std::string transOCLImageTypeAccessQualifier(SPIRV::SPIRVTypeImage* ST);
  std::string transOCLPipeTypeAccessQualifier(SPIRV::SPIRVTypePipe* ST);

  Value *oclTransConstantSampler(SPIRV::SPIRVConstantSampler* BCS);
  void setName(llvm::Value* V, SPIRVValue* BV);
  template<class Source, class Func>
  bool foreachFuncCtlMask(Source, Func);
};

Type *
SPIRVToLLVM::getTranslatedType(SPIRVType *BV){
  auto Loc = TypeMap.find(BV);
  if (Loc != TypeMap.end())
    return Loc->second;
  return nullptr;
}

Value *
SPIRVToLLVM::getTranslatedValue(SPIRVValue *BV){
  auto Loc = ValueMap.find(BV);
  if (Loc != ValueMap.end())
    return Loc->second;
  return nullptr;
}

void
SPIRVToLLVM::setAttrByCalledFunc(CallInst *Call) {
  Function *F = Call->getCalledFunction();
  if (F->isIntrinsic()) {
    return;
  }
  Call->setCallingConv(F->getCallingConv());
  Call->setAttributes(F->getAttributes());
}

bool
SPIRVToLLVM::transOCLBuiltinsFromVariables(){
  std::vector<GlobalVariable *> WorkList;
  for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
    SPIRVBuiltinVariableKind Kind = BuiltInCount;
    if (!isSPIRVBuiltinVariable(I, &Kind))
      continue;
    if (!transOCLBuiltinFromVariable(I, Kind))
      return false;
    WorkList.push_back(I);
  }
  for (auto &I:WorkList) {
    I->dropAllReferences();
    I->removeFromParent();
  }
  return true;
}

// For integer types shorter than 32 bit, unsigned/signedness can be inferred
// from zext/sext attribute.
MDString *
SPIRVToLLVM::transOCLKernelArgTypeName(SPIRVFunctionParameter *Arg) {
  auto Ty = Arg->isByVal() ? Arg->getType()->getPointerElementType() :
    Arg->getType();
  return MDString::get(*Context, transTypeToOCLTypeName(Ty, !Arg->isZext()));
}

// Variable like GlobalInvolcationId[x] -> get_global_id(x).
// Variable like WorkDim -> get_work_dim().
bool
SPIRVToLLVM::transOCLBuiltinFromVariable(GlobalVariable *GV,
    SPIRVBuiltinVariableKind Kind) {
  std::string FuncName = SPIRSPIRVBuiltinVariableMap::rmap(Kind);
  std::string MangledName;
  Type *ReturnTy =  GV->getType()->getPointerElementType();
  bool IsVec = ReturnTy->isVectorTy();
  if (IsVec)
    ReturnTy = cast<VectorType>(ReturnTy)->getElementType();
  std::vector<Type*> ArgTy;
  if (IsVec)
    ArgTy.push_back(Type::getInt32Ty(*Context));
  MangleOpenCLBuiltin(FuncName, ArgTy, MangledName);
  Function *Func = M->getFunction(MangledName);
  if (!Func) {
    FunctionType *FT = FunctionType::get(ReturnTy, ArgTy, false);
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    Func->addFnAttr(Attribute::NoUnwind);
    Func->addFnAttr(Attribute::ReadNone);
  }
  std::vector<Instruction *> Deletes;
  std::vector<Instruction *> Uses;
  for (auto UI = GV->user_begin(), UE = GV->user_end(); UI != UE; ++UI) {
    assert (isa<LoadInst>(*UI) && "Unsupported use");
    auto LD = dyn_cast<LoadInst>(*UI);
    if (!IsVec) {
      Uses.push_back(LD);
      Deletes.push_back(LD);
      continue;
    }
    for (auto LDUI = LD->user_begin(), LDUE = LD->user_end(); LDUI != LDUE;
        ++LDUI) {
      assert(isa<ExtractElementInst>(*LDUI) && "Unsupported use");
      auto EEI = dyn_cast<ExtractElementInst>(*LDUI);
      Uses.push_back(EEI);
      Deletes.push_back(EEI);
    }
    Deletes.push_back(LD);
  }
  for (auto &I:Uses) {
    std::vector<Value *> Arg;
    if (auto EEI = dyn_cast<ExtractElementInst>(I))
      Arg.push_back(EEI->getIndexOperand());
    auto Call = CallInst::Create(Func, Arg, "", I);
    Call->takeName(I);
    setAttrByCalledFunc(Call);
    SPIRVDBG(dbgs() << "[transOCLBuiltinFromVariable] " << *I << " -> " <<
        *Call << '\n';)
    I->replaceAllUsesWith(Call);
  }
  for (auto &I:Deletes) {
    I->dropAllReferences();
    I->removeFromParent();
  }
  return true;
}

Type *
SPIRVToLLVM::transFPType(SPIRVType* T) {
  switch(T->getFloatBitWidth()) {
  case 16: return Type::getHalfTy(*Context);
  case 32: return Type::getFloatTy(*Context);
  case 64: return Type::getDoubleTy(*Context);
  default:
    llvm_unreachable("Invalid type");
    return nullptr;
  }
}

std::string
SPIRVToLLVM::transOCLImageTypeName(SPIRV::SPIRVTypeImage* ST) {
  return std::string(kSPR2TypeName::OCLPrefix)
       + rmap<std::string>(ST->getDescriptor())
       + kSPR2TypeName::Delimiter
       + rmap<std::string>(ST->getAccessQualifier());
}

std::string
SPIRVToLLVM::transOCLSampledImageTypeName(SPIRV::SPIRVTypeSampledImage* ST) {
  return std::string(kSPIRVTypeName::SampledImg)
       + kSPR2TypeName::Delimiter
       + rmap<std::string>(ST->getImageType()->getDescriptor());
}

std::string
SPIRVToLLVM::transOCLPipeTypeName(SPIRV::SPIRVTypePipe* PT) {
  return SPIR_TYPE_NAME_PIPE_T;
}

Type *
SPIRVToLLVM::transType(SPIRVType *T) {
  auto Loc = TypeMap.find(T);
  if (Loc != TypeMap.end())
    return Loc->second;

  SPIRVDBG(spvdbgs() << "[transType] " << *T << " -> ";)
  T->validate();
  switch(T->getOpCode()) {
  case OpTypeVoid:
    return mapType(T, Type::getVoidTy(*Context));
  case OpTypeBool:
    return mapType(T, Type::getInt1Ty(*Context));
  case OpTypeInt:
    return mapType(T, Type::getIntNTy(*Context, T->getIntegerBitWidth()));
  case OpTypeFloat:
    return mapType(T, transFPType(T));
  case OpTypeArray:
    return mapType(T, ArrayType::get(transType(T->getArrayElementType()),
        T->getArrayLength()));
  case OpTypePointer:
    return mapType(T, PointerType::get(transType(T->getPointerElementType()),
        SPIRSPIRVAddrSpaceMap::rmap(T->getPointerStorageClass())));
  case OpTypeVector:
    return mapType(T, VectorType::get(transType(T->getVectorComponentType()),
        T->getVectorComponentCount()));
  case OpTypeOpaque:
    return mapType(T, StructType::create(*Context, T->getName()));
  case OpTypeFunction: {
    auto FT = static_cast<SPIRVTypeFunction *>(T);
    auto RT = transType(FT->getReturnType());
    std::vector<Type *> PT;
    for (size_t I = 0, E = FT->getNumParameters(); I != E; ++I)
      PT.push_back(transType(FT->getParameterType(I)));
    return mapType(T, FunctionType::get(RT, PT, false));
    }
  case OpTypeImage: {
    auto ST = static_cast<SPIRVTypeImage *>(T);
    if (ST->isOCLImage())
      return mapType(T, getOrCreateOpaquePtrType(M,
          transOCLImageTypeName(ST)));
    else
      llvm_unreachable("Unsupported image type");
    return nullptr;
  }
  case OpTypeSampler:
    return mapType(T, Type::getInt32Ty(*Context));
  case OpTypeSampledImage: {
    auto ST = static_cast<SPIRVTypeSampledImage *>(T);
    return mapType(T, getOrCreateOpaquePtrType(M,
        transOCLSampledImageTypeName(ST)));
  }
  case OpTypeStruct: {
    auto ST = static_cast<SPIRVTypeStruct *>(T);
    std::vector<Type *> MT;
    for (size_t I = 0, E = ST->getMemberCount(); I != E; ++I)
      MT.push_back(transType(ST->getMemberType(I)));
    auto Name = ST->getName();
    if (!Name.empty()) {
      if (auto OldST = M->getTypeByName(Name))
        OldST->setName("");
    }
    return mapType(T, StructType::create(*Context, MT, Name,
      ST->isPacked()));
    }
  case OpTypePipe: {
    auto PT = static_cast<SPIRVTypePipe *>(T);
    return mapType(T, getOrCreateOpaquePtrType(M, transOCLPipeTypeName(PT)));
    }
  default: {
    auto OC = T->getOpCode();
    if (isOpaqueGenericTypeOpCode(OC))
      return mapType(T, getOrCreateOpaquePtrType(M,
          BuiltinOpaqueGenericTypeOpCodeMap::rmap(OC),
          getOCLOpaqueTypeAddrSpace(OC)));
    llvm_unreachable("Not implemented");
    }
  }
  return 0;
}

std::string
SPIRVToLLVM::transTypeToOCLTypeName(SPIRVType *T, bool IsSigned) {
  switch(T->getOpCode()) {
  case OpTypeVoid:
    return "void";
  case OpTypeBool:
    return "bool";
  case OpTypeInt: {
    std::string Prefix = IsSigned ? "" : "u";
    switch(T->getIntegerBitWidth()) {
    case 8:
      return Prefix + "char";
    case 16:
      return Prefix + "short";
    case 32:
      return Prefix + "int";
    case 64:
      return Prefix + "long";
    default:
      llvm_unreachable("invalid integer size");
      return Prefix + std::string("int") + T->getIntegerBitWidth() + "_t";
    }
  }
  break;
  case OpTypeFloat:
    switch(T->getFloatBitWidth()){
    case 16:
      return "half";
    case 32:
      return "float";
    case 64:
      return "double";
    default:
      llvm_unreachable("invalid floating pointer bitwidth");
      return std::string("float") + T->getFloatBitWidth() + "_t";
    }
    break;
  case OpTypeArray:
    return "array";
  case OpTypePointer:
    return transTypeToOCLTypeName(T->getPointerElementType()) + "*";
  case OpTypeVector:
    return transTypeToOCLTypeName(T->getVectorComponentType()) +
        T->getVectorComponentCount();
  case OpTypeOpaque:
      return T->getName();
  case OpTypeFunction:
    llvm_unreachable("Unsupported");
    return "function";
  case OpTypeStruct: {
    auto Name = T->getName();
    if (Name.find("struct.") == 0)
      Name[6] = ' ';
    else if (Name.find("union.") == 0)
      Name[5] = ' ';
    return Name;
  }
  case OpTypePipe:
    return "pipe";
  case OpTypeSampler:
    return "sampler_t";
  case OpTypeImage:
    return rmap<std::string>(static_cast<SPIRVTypeImage *>(T)->getDescriptor());
  default:
      if (isOpaqueGenericTypeOpCode(T->getOpCode())) {
        return BuiltinOpaqueGenericTypeOpCodeMap::rmap(T->getOpCode());
      }
      llvm_unreachable("Not implemented");
      return "unknown";
  }
}

std::vector<Type *>
SPIRVToLLVM::transTypeVector(const std::vector<SPIRVType *> &BT) {
  std::vector<Type *> T;
  for (auto I: BT)
    T.push_back(transType(I));
  return T;
}

std::vector<Value *>
SPIRVToLLVM::transValue(const std::vector<SPIRVValue *> &BV, Function *F,
    BasicBlock *BB) {
  std::vector<Value *> V;
  for (auto I: BV)
    V.push_back(transValue(I, F, BB));
  return V;
}

bool
SPIRVToLLVM::isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction* BI) const {
  auto OC = BI->getOpCode();
  return isCmpOpCode(OC) &&
      !(OC >= OpLessOrGreater && OC <= OpUnordered);
}

void
SPIRVToLLVM::transFlags(llvm::Value* V) {
  if(!isa<Instruction>(V))
    return;
  auto OC = cast<Instruction>(V)->getOpcode();
  if (OC == Instruction::AShr || OC == Instruction::LShr) {
    cast<BinaryOperator>(V)->setIsExact();
    return;
  }
}

void
SPIRVToLLVM::setName(llvm::Value* V, SPIRVValue* BV) {
  auto Name = BV->getName();
  if (!Name.empty() && (!V->hasName() || Name != V->getName()))
    V->setName(Name);
}

Value *
SPIRVToLLVM::transValue(SPIRVValue *BV, Function *F, BasicBlock *BB,
    bool CreatePlaceHolder){
  SPIRVToLLVMValueMap::iterator Loc = ValueMap.find(BV);
  if (Loc != ValueMap.end() && (!PlaceholderMap.count(BV) || CreatePlaceHolder))
    return Loc->second;

  SPIRVDBG(spvdbgs() << "[transValue] " << *BV << " -> ";)
  BV->validate();

  auto V = transValueWithoutDecoration(BV, F, BB, CreatePlaceHolder);
  if (!V) {
    SPIRVDBG(dbgs() << " Warning ! nullptr\n";)
    return nullptr;
  }
  setName(V, BV);
  if (!transDecoration(BV, V)) {
    assert (0 && "trans decoration fail");
    return nullptr;
  }
  transFlags(V);

  SPIRVDBG(dbgs() << *V << '\n';)

  return V;
}

Value *
SPIRVToLLVM::transConvertInst(SPIRVValue* BV, Function* F, BasicBlock* BB) {
  SPIRVUnary* BC = static_cast<SPIRVUnary*>(BV);
  auto Src = transValue(BC->getOperand(0), F, BB, BB ? true : false);
  auto Dst = transType(BC->getType());
  CastInst::CastOps CO = Instruction::BitCast;
  bool IsExt = Dst->getScalarSizeInBits()
      > Src->getType()->getScalarSizeInBits();
  switch (BC->getOpCode()) {
  case OpPtrCastToGeneric:
  case OpGenericCastToPtr:
    CO = Instruction::AddrSpaceCast;
    break;
  case OpSConvert:
    CO = IsExt ? Instruction::SExt : Instruction::Trunc;
    break;
  case OpUConvert:
    CO = IsExt ? Instruction::ZExt : Instruction::Trunc;
    break;
  case OpFConvert:
    CO = IsExt ? Instruction::FPExt : Instruction::FPTrunc;
    break;
  default:
    CO = static_cast<CastInst::CastOps>(OpCodeMap::rmap(BC->getOpCode()));
  }
  assert(CastInst::isCast(CO) && "Invalid cast op code");
  SPIRVDBG(if (!CastInst::castIsValid(CO, Src, Dst)) {
    spvdbgs() << "Invalid cast: " << *BV << " -> ";
    dbgs() << "Op = " << CO << ", Src = " << *Src << " Dst = " << *Dst << '\n';
  })
  if (BB)
    return CastInst::Create(CO, Src, Dst, BV->getName(), BB);
  return ConstantExpr::getCast(CO, dyn_cast<Constant>(Src), Dst);
}

BinaryOperator *SPIRVToLLVM::transShiftLogicalBitwiseInst(SPIRVValue* BV,
    BasicBlock* BB,Function* F) {
  SPIRVBinary* BBN = static_cast<SPIRVBinary*>(BV);
  assert(BB && "Invalid BB");
  Instruction::BinaryOps BO;
  auto OP = BBN->getOpCode();
  if (isLogicalOpCode(OP))
    OP = IntBoolOpMap::rmap(OP);
  BO = static_cast<Instruction::BinaryOps>(OpCodeMap::rmap(OP));
  auto Inst = BinaryOperator::Create(BO,
      transValue(BBN->getOperand(0), F, BB),
      transValue(BBN->getOperand(1), F, BB), BV->getName(), BB);
  return Inst;
}

Instruction *
SPIRVToLLVM::transCmpInst(SPIRVValue* BV, BasicBlock* BB, Function* F) {
  SPIRVCompare* BC = static_cast<SPIRVCompare*>(BV);
  assert(BB && "Invalid BB");
  SPIRVType* BT = BC->getOperand(0)->getType();
  Instruction* Inst = nullptr;
  auto OP = BC->getOpCode();
  if (isLogicalOpCode(OP))
    OP = IntBoolOpMap::rmap(OP);
  if (BT->isTypeVectorOrScalarInt() || BT->isTypeVectorOrScalarBool() ||
      BT->isTypePointer())
    Inst = new ICmpInst(*BB, CmpMap::rmap(OP),
        transValue(BC->getOperand(0), F, BB),
        transValue(BC->getOperand(1), F, BB));
  else if (BT->isTypeVectorOrScalarFloat())
    Inst = new FCmpInst(*BB, CmpMap::rmap(OP),
        transValue(BC->getOperand(0), F, BB),
        transValue(BC->getOperand(1), F, BB));
  assert(Inst && "not implemented");
  return Inst;
}

bool
SPIRVToLLVM::postProcessOCL() {
  std::string DemangledName;
  SPIRVWord SrcLangVer = 0;
  BM->getSourceLanguage(&SrcLangVer);
  for (auto I = M->begin(), E = M->end(); I != E;) {
    auto F = I++;
    if (F->hasName() && F->isDeclaration()) {
      DEBUG(dbgs() << "[postProcessOCL sret] " << *F << '\n');
      if (F->getReturnType()->isStructTy() &&
          oclIsBuiltin(F->getName(), SrcLangVer, &DemangledName)) {
        if (!postProcessOCLBuiltinReturnStruct(F))
          return false;
      }
    }
  }
  for (auto I = M->begin(), E = M->end(); I != E;) {
    auto F = I++;
    if (F->hasName() && F->isDeclaration()) {
      DEBUG(dbgs() << "[postProcessOCL func ptr] " << *F << '\n');
      auto AI = F->arg_begin();
      if (hasFunctionPointerArg(F, AI) && isDecoratedSPIRVFunc(F))
        if (!postProcessOCLBuiltinWithFuncPointer(F, AI))
          return false;
    }
  }
  for (auto I = M->begin(), E = M->end(); I != E;) {
    auto F = I++;
    if (F->hasName() && F->isDeclaration()) {
      DEBUG(dbgs() << "[postProcessOCL array arg] " << *F << '\n');
      if (hasArrayArg(F) &&
          oclIsBuiltin(F->getName(), SrcLangVer, &DemangledName))
        if (!postProcessOCLBuiltinWithArrayArguments(F, DemangledName))
          return false;
    }
  }
  return true;
}

bool
SPIRVToLLVM::postProcessOCLBuiltinReturnStruct(Function *F) {
  std::string Name = F->getName();
  F->setName(Name + ".old");
  for (auto I = F->user_begin(), E = F->user_end(); I != E;) {
    if (auto CI = dyn_cast<CallInst>(*I++)) {
      auto ST = dyn_cast<StoreInst>(*(CI->user_begin()));
      std::vector<Type *> ArgTys;
      getFunctionTypeParameterTypes(F->getFunctionType(), ArgTys);
      ArgTys.insert(ArgTys.begin(), PointerType::get(F->getReturnType(),
          SPIRAS_Private));
      auto newF = getOrCreateFunction(M, Type::getVoidTy(*Context),
          ArgTys, Name);
      newF->setCallingConv(F->getCallingConv());
      auto Args = getArguments(CI);
      Args.insert(Args.begin(), ST->getPointerOperand());
      auto NewCI = CallInst::Create(newF, Args, CI->getName(), CI);
      NewCI->setCallingConv(CI->getCallingConv());
      ST->dropAllReferences();
      ST->removeFromParent();
      CI->dropAllReferences();
      CI->removeFromParent();
    }
  }
  F->dropAllReferences();
  F->removeFromParent();
  return true;
}

bool
SPIRVToLLVM::postProcessOCLBuiltinWithFuncPointer(Function* F,
    Function::arg_iterator I) {
  auto Name = undecorateSPIRVFunction(F->getName());
  std::set<Value *> InvokeFuncPtrs;
  mutateFunctionOCL (F, [=, &InvokeFuncPtrs](
      CallInst *CI, std::vector<Value *> &Args) {
    auto ALoc = Args.begin();
    for (auto E = Args.end(); ALoc != E; ++ALoc) {
      if (isFunctionPointerType((*ALoc)->getType())) {
        assert(isa<Function>(*ALoc) && "Invalid function pointer usage");
        break;
      }
    }
    assert (ALoc != Args.end());
    Value *Ctx = nullptr;
    Value *CtxLen = nullptr;
    Value *CtxAlign = nullptr;
    if (Name == kOCLBuiltinName::EnqueueKernel) {
      assert(Args.end() - ALoc > 3);
      Ctx = ALoc[1];
      CtxLen = ALoc[2];
      CtxAlign = ALoc[3];
      Args.erase(ALoc + 1, ALoc + 4);
    }
    InvokeFuncPtrs.insert(*ALoc);
    *ALoc = addBlockBind(M, cast<Function>(removeCast(*ALoc)),
        Ctx, CtxLen, CtxAlign, CI);
    return Name;
  });
  for (auto &I:InvokeFuncPtrs)
    eraseIfNoUse(I);
  return true;
}

bool
SPIRVToLLVM::postProcessOCLBuiltinWithArrayArguments(Function* F,
    const std::string &DemangledName) {
  DEBUG(dbgs() << "[postProcessOCLBuiltinWithArrayArguments] " << *F << '\n');
  auto Attrs = F->getAttributes();
  auto Name = F->getName();
  mutateFunction(F, [=](CallInst *CI, std::vector<Value *> &Args) {
    auto FBegin = CI->getParent()->getParent()->begin()->getFirstInsertionPt();
    for (auto &I:Args) {
      auto T = I->getType();
      if (!T->isArrayTy())
        continue;
      auto Alloca = new AllocaInst(T, "", FBegin);
      auto Store = new StoreInst(I, Alloca, false, CI);
      auto Zero = ConstantInt::getNullValue(Type::getInt32Ty(T->getContext()));
      Value *Index[] = {Zero, Zero};
      I = GetElementPtrInst::CreateInBounds(Alloca, Index, "", CI);
    }
    return Name;
  }, nullptr, &Attrs);
  return true;
}

// ToDo: Handle unsigned integer return type. May need spec change.
CallInst *
SPIRVToLLVM::postProcessOCLReadImage(SPIRVInstruction *BI, CallInst* CI,
    const std::string &FuncName) {
  AttributeSet Attrs = CI->getCalledFunction()->getAttributes();
  return mutateCallInstOCL(M, CI, [=](CallInst *, std::vector<Value *> &Args){
    CallInst *CallSampledImg = cast<CallInst>(Args[0]);
    auto Img = CallSampledImg->getArgOperand(0);
    assert(isOCLImageType(Img->getType()));
    auto Sampler = CallSampledImg->getArgOperand(1);
    Args[0] = Img;
    Args.insert(Args.begin() + 1, Sampler);
    if (CallSampledImg->hasOneUse()) {
      CallSampledImg->replaceAllUsesWith(
          UndefValue::get(CallSampledImg->getType()));
      CallSampledImg->dropAllReferences();
      CallSampledImg->eraseFromParent();
    }
    Type *T = CI->getType();
    if (auto VT = dyn_cast<VectorType>(T))
      T = VT->getElementType();
    return std::string(kOCLBuiltinName::SampledReadImage)
      + (T->isFloatingPointTy()?'f':'i');
  }, &Attrs);
}

CallInst *
SPIRVToLLVM::expandOCLBuiltinWithScalarArg(CallInst* CI,
    const std::string &FuncName) {
  AttributeSet Attrs = CI->getCalledFunction()->getAttributes();
  if (!CI->getOperand(0)->getType()->isVectorTy() &&
    CI->getOperand(1)->getType()->isVectorTy()) {
    return mutateCallInstOCL(M, CI, [=](CallInst *, std::vector<Value *> &Args){
      unsigned vecSize = CI->getOperand(1)->getType()->getVectorNumElements();
      Value *NewVec = nullptr;
      if (auto CA = dyn_cast<Constant>(Args[0]))
        NewVec = ConstantVector::getSplat(vecSize, CA);
      else {
        NewVec = ConstantVector::getSplat(vecSize,
            Constant::getNullValue(Args[0]->getType()));
        NewVec = InsertElementInst::Create(NewVec, Args[0], getInt32(M, 0), "",
            CI);
        NewVec = new ShuffleVectorInst(NewVec, NewVec,
            ConstantVector::getSplat(vecSize, getInt32(M, 0)), "", CI);
      }
      NewVec->takeName(Args[0]);
      Args[0] = NewVec;
      return FuncName;
    }, &Attrs);
  }
  return CI;
}

std::string
SPIRVToLLVM::transOCLPipeTypeAccessQualifier(SPIRV::SPIRVTypePipe* ST) {
  return SPIRSPIRVAccessQualifierMap::rmap(ST->getAccessQualifier());
}

void
SPIRVToLLVM::transGeneratorMD() {
  SPIRVMDBuilder B(*M);
  B.addNamedMD(kSPIRVMD::Generator)
      .addOp()
        .addU16(BM->getGeneratorId())
        .addU16(BM->getGeneratorVer())
        .done();
}

Value *
SPIRVToLLVM::oclTransConstantSampler(SPIRV::SPIRVConstantSampler* BCS) {
  auto Lit = (BCS->getAddrMode() << 1) |
      BCS->getNormalized() |
      ((BCS->getFilterMode() + 1) << 4);
  auto Ty = IntegerType::getInt32Ty(*Context);
  return ConstantInt::get(Ty, Lit);
}

/// For instructions, this function assumes they are created in order
/// and appended to the given basic block. An instruction may use a
/// instruction from another BB which has not been translated. Such
/// instructions should be translated to place holders at the point
/// of first use, then replaced by real instructions when they are
/// created.
///
/// When CreatePlaceHolder is true, create a load instruction of a
/// global variable as placeholder for SPIRV instruction. Otherwise,
/// create instruction and replace placeholder if there is one.
Value *
SPIRVToLLVM::transValueWithoutDecoration(SPIRVValue *BV, Function *F,
    BasicBlock *BB, bool CreatePlaceHolder){

  auto OC = BV->getOpCode();
  IntBoolOpMap::rfind(OC, &OC);

  // Translation of non-instruction values
  switch(OC) {
  case OpConstant: {
    SPIRVConstant *BConst = static_cast<SPIRVConstant *>(BV);
    SPIRVType *BT = BV->getType();
    Type *LT = transType(BT);
    switch(BT->getOpCode()) {
    case OpTypeBool:
    case OpTypeInt:
      return mapValue(BV, ConstantInt::get(LT, BConst->getZExtIntValue(),
          static_cast<SPIRVTypeInt*>(BT)->isSigned()));
    case OpTypeFloat: {
      const llvm::fltSemantics *FS = nullptr;
      switch (BT->getFloatBitWidth()) {
      case 16:
        FS = &APFloat::IEEEhalf;
        break;
      case 32:
        FS = &APFloat::IEEEsingle;
        break;
      case 64:
        FS = &APFloat::IEEEdouble;
        break;
      default:
        assert (0 && "invalid float type");
      }
      return mapValue(BV, ConstantFP::get(*Context, APFloat(*FS,
          APInt(BT->getFloatBitWidth(), BConst->getZExtIntValue()))));
    }
    default:
      llvm_unreachable("Not implemented");
      return NULL;
    }
  }
  break;

  case OpConstantTrue:
    return mapValue(BV, ConstantInt::getTrue(*Context));

  case OpConstantFalse:
    return mapValue(BV, ConstantInt::getFalse(*Context));

  case OpConstantNull: {
    auto LT = transType(BV->getType());
    if (auto PT = dyn_cast<PointerType>(LT))
      return mapValue(BV, ConstantPointerNull::get(PT));
    return mapValue(BV, ConstantAggregateZero::get(LT));
  }

  case OpConstantComposite: {
    auto BCC = static_cast<SPIRVConstantComposite*>(BV);
    std::vector<Constant *> CV;
    for (auto &I:BCC->getElements())
      CV.push_back(dyn_cast<Constant>(transValue(I, F, BB)));
    switch(BV->getType()->getOpCode()) {
    case OpTypeVector:
      return mapValue(BV, ConstantVector::get(CV));
    case OpTypeArray:
      return mapValue(BV, ConstantArray::get(
          dyn_cast<ArrayType>(transType(BCC->getType())), CV));
    case OpTypeStruct:
      return mapValue(BV, ConstantStruct::get(
          dyn_cast<StructType>(transType(BCC->getType())), CV));
    default:
      llvm_unreachable("not implemented");
      return nullptr;
    }
  }
  break;

  case OpConstantSampler: {
    auto BCS = static_cast<SPIRVConstantSampler*>(BV);
    return mapValue(BV, oclTransConstantSampler(BCS));
  }

  case OpSpecConstantOp: {
    auto BI = createInstFromSpecConstantOp(
        static_cast<SPIRVSpecConstantOp*>(BV));
    return mapValue(BV, transValue(BI, nullptr, nullptr, false));
  }

  case OpUndef:
    return mapValue(BV, UndefValue::get(transType(BV->getType())));

  case OpVariable: {
    auto BVar = static_cast<SPIRVVariable *>(BV);
    auto Initializer = BVar->getInitializer();
    SPIRVStorageClassKind BS = BVar->getStorageClass();
    auto Ty = transType(BVar->getType()->getPointerElementType());

    if (BS == StorageClassFunction && !Initializer) {
        assert (BB && "Invalid BB");
        return mapValue(BV, new AllocaInst(Ty, BV->getName(), BB));
    }
    auto AddrSpace = SPIRSPIRVAddrSpaceMap::rmap(BS);
    bool IsConst = BVar->isConstant();
    auto LVar = new GlobalVariable(*M, Ty, IsConst,
        SPIRSPIRVLinkageTypeMap::rmap(BVar->getLinkageType()),
        Initializer?dyn_cast<Constant>(transValue(Initializer, F, BB, false)):
            nullptr,
        BV->getName(), 0, GlobalVariable::NotThreadLocal, AddrSpace);
    LVar->setUnnamedAddr(IsConst && Ty->isArrayTy() &&
        Ty->getArrayElementType()->isIntegerTy(8));
    SPIRVBuiltinVariableKind BVKind = BuiltInCount;
    if (BVar->isBuiltin(&BVKind))
      BuiltinGVMap[LVar] = BVKind;
    return mapValue(BV, LVar);
  }
  break;

  case OpFunctionParameter: {
    auto BA = static_cast<SPIRVFunctionParameter*>(BV);
    assert (F && "Invalid function");
    unsigned ArgNo = 0;
    for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
        ++I, ++ArgNo) {
      if (ArgNo == BA->getArgNo())
        return mapValue(BV, I);
    }
    assert (0 && "Invalid argument");
    return NULL;
  }
  break;

  case OpFunction:
    return mapValue(BV, transFunction(static_cast<SPIRVFunction *>(BV)));

  case OpLabel:
    return mapValue(BV, BasicBlock::Create(*Context, BV->getName(), F));
    break;
  default:
    // do nothing
    break;
  }

  // Creation of place holder
  if (CreatePlaceHolder) {
    auto GV = new GlobalVariable(*M,
        transType(BV->getType()),
        false,
        GlobalValue::PrivateLinkage,
        nullptr,
        std::string(kPlaceholderPrefix) + BV->getName(),
        0, GlobalVariable::NotThreadLocal, 0);
    auto LD = new LoadInst(GV, BV->getName(), BB);
    PlaceholderMap[BV] = LD;
    return mapValue(BV, LD);
  }

  // Translation of instructions
  switch (BV->getOpCode()) {
  case OpBranch: {
    auto BR = static_cast<SPIRVBranch *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, BranchInst::Create(
      dyn_cast<BasicBlock>(transValue(BR->getTargetLabel(), F, BB)), BB));
    }
    break;

  case OpBranchConditional: {
    auto BR = static_cast<SPIRVBranchConditional *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, BranchInst::Create(
      dyn_cast<BasicBlock>(transValue(BR->getTrueLabel(), F, BB)),
      dyn_cast<BasicBlock>(transValue(BR->getFalseLabel(), F, BB)),
      transValue(BR->getCondition(), F, BB),
      BB));
    }
    break;

  case OpPhi: {
    auto Phi = static_cast<SPIRVPhi *>(BV);
    assert(BB && "Invalid BB");
    auto LPhi = dyn_cast<PHINode>(mapValue(BV, PHINode::Create(
      transType(Phi->getType()),
      Phi->getPairs().size() / 2,
      Phi->getName(),
      BB)));
    Phi->foreachPair([&](SPIRVValue *IncomingV, SPIRVBasicBlock *IncomingBB,
      size_t Index){
      auto Translated = transValue(IncomingV, F, BB);
      LPhi->addIncoming(Translated,
        dyn_cast<BasicBlock>(transValue(IncomingBB, F, BB)));
    });
    return LPhi;
    }
    break;

  case OpReturn:
    assert(BB && "Invalid BB");
    return mapValue(BV, ReturnInst::Create(*Context, BB));
    break;

  case OpReturnValue: {
    auto RV = static_cast<SPIRVReturnValue *>(BV);
    return mapValue(BV, ReturnInst::Create(*Context,
      transValue(RV->getReturnValue(), F, BB), BB));
    }
    break;

  case OpStore: {
    SPIRVStore *BS = static_cast<SPIRVStore*>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, new StoreInst(
      transValue(BS->getSrc(), F, BB),
      transValue(BS->getDst(), F, BB),
      BS->SPIRVMemoryAccess::isVolatile(),
      BS->SPIRVMemoryAccess::getAlignment(),
      BB));
    }
    break;

  case OpLoad: {
    SPIRVLoad *BL = static_cast<SPIRVLoad*>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, new LoadInst(
      transValue(BL->getSrc(), F, BB),
      BV->getName(),
      BL->SPIRVMemoryAccess::isVolatile(),
      BL->SPIRVMemoryAccess::getAlignment(),
      BB));
    }
    break;

  case OpCopyMemorySized: {
    SPIRVCopyMemorySized *BC = static_cast<SPIRVCopyMemorySized *>(BV);
    assert(BB && "Invalid BB");
    std::string FuncName = "llvm.memcpy";
    SPIRVType* BS = BC->getSource()->getType();
    SPIRVType* BT = BC->getTarget()->getType();
    Type *Int1Ty = Type::getInt1Ty(*Context);
    Type* Int32Ty = Type::getInt32Ty(*Context);
    Type* VoidTy = Type::getVoidTy(*Context);
    Type* SrcTy = transType(BS);
    Type* TrgTy = transType(BT);
    Type* SizeTy = transType(BC->getSize()->getType());
    Type* ArgTy[] = { TrgTy, SrcTy, SizeTy, Int32Ty, Int1Ty };

    ostringstream TempName;
    TempName << ".p" << SPIRSPIRVAddrSpaceMap::rmap(BT->getPointerStorageClass()) << "i8";
    TempName << ".p" << SPIRSPIRVAddrSpaceMap::rmap(BS->getPointerStorageClass()) << "i8";
    FuncName += TempName.str();
    if (BC->getSize()->getType()->getBitWidth() == 32)
      FuncName += ".i32";
    else
      FuncName += ".i64";

    FunctionType *FT = FunctionType::get(VoidTy, ArgTy, false);
    Function *Func = dyn_cast<Function>(M->getOrInsertFunction(FuncName, FT));
    assert(Func && Func->getFunctionType() == FT && "Function type mismatch");
    Func->setLinkage(GlobalValue::ExternalLinkage);

    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);

    Value *Arg[] = { transValue(BC->getTarget(), Func, BB),
                     transValue(BC->getSource(), Func, BB),
                     dyn_cast<llvm::ConstantInt>(transValue(BC->getSize(),
                         Func, BB)),
                     ConstantInt::get(Int32Ty,
                         BC->SPIRVMemoryAccess::getAlignment()),
                     ConstantInt::get(Int1Ty,
                         BC->SPIRVMemoryAccess::isVolatile())};
    return mapValue( BV, CallInst::Create(Func, Arg, "", BB));
  }
  break;
  case OpSelect: {
    SPIRVSelect *BS = static_cast<SPIRVSelect*>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, SelectInst::Create(
      transValue(BS->getCondition(), F, BB),
      transValue(BS->getTrueValue(), F, BB),
      transValue(BS->getFalseValue(), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpSwitch: {
    auto BS = static_cast<SPIRVSwitch *>(BV);
    assert(BB && "Invalid BB");
    auto Select = transValue(BS->getSelect(), F, BB);
    auto LS = SwitchInst::Create(Select,
      dyn_cast<BasicBlock>(transValue(BS->getDefault(), F, BB)),
      BS->getNumPairs(), BB);
    BS->foreachPair([&](SPIRVWord Literal, SPIRVBasicBlock *Label, size_t Index){
      LS->addCase(ConstantInt::get(dyn_cast<IntegerType>(Select->getType()),
        Literal), dyn_cast<BasicBlock>(transValue(Label, F, BB)));
    });
    return mapValue(BV, LS);
    }
    break;

  case OpAccessChain:
  case OpInBoundsAccessChain:
  case OpPtrAccessChain:
  case OpInBoundsPtrAccessChain: {
    auto AC = static_cast<SPIRVAccessChainBase *>(BV);
    auto Base = transValue(AC->getBase(), F, BB);
    auto Index = transValue(AC->getIndices(), F, BB);
    if (!AC->hasPtrIndex())
      Index.insert(Index.begin(), getInt32(M, 0));
    auto IsInbound = AC->isInBounds();
    Value *V = nullptr;
    if (BB) {
      auto GEP = GetElementPtrInst::Create(Base, Index, BV->getName(), BB);
      GEP->setIsInBounds(IsInbound);
      V = GEP;
    } else {
      V = ConstantExpr::getGetElementPtr(dyn_cast<Constant>(Base), Index,
          IsInbound);
    }
    return mapValue(BV, V);
    }
    break;

  case OpCompositeExtract: {
    SPIRVCompositeExtract *CE = static_cast<SPIRVCompositeExtract *>(BV);
    assert(BB && "Invalid BB");
    assert(CE->getComposite()->getType()->isTypeVector() && "Invalid type");
    assert(CE->getIndices().size() == 1 && "Invalid index");
    return mapValue(BV, ExtractElementInst::Create(
      transValue(CE->getComposite(), F, BB),
      ConstantInt::get(*Context, APInt(32, CE->getIndices()[0])),
      BV->getName(), BB));
    }
    break;

  case OpVectorExtractDynamic: {
    auto CE = static_cast<SPIRVVectorExtractDynamic *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, ExtractElementInst::Create(
      transValue(CE->getVector(), F, BB),
      transValue(CE->getIndex(), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpCompositeInsert: {
    auto CI = static_cast<SPIRVCompositeInsert *>(BV);
    assert(BB && "Invalid BB");
    assert(CI->getComposite()->getType()->isTypeVector() && "Invalid type");
    assert(CI->getIndices().size() == 1 && "Invalid index");
    return mapValue(BV, InsertElementInst::Create(
      transValue(CI->getComposite(), F, BB),
      transValue(CI->getObject(), F, BB),
      ConstantInt::get(*Context, APInt(32, CI->getIndices()[0])),
      BV->getName(), BB));
    }
    break;

  case OpVectorInsertDynamic: {
    auto CI = static_cast<SPIRVVectorInsertDynamic *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, InsertElementInst::Create(
      transValue(CI->getVector(), F, BB),
      transValue(CI->getComponent(), F, BB),
      transValue(CI->getIndex(), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpVectorShuffle: {
    auto VS = static_cast<SPIRVVectorShuffle *>(BV);
    assert(BB && "Invalid BB");
    std::vector<Constant *> Components;
    IntegerType *Int32Ty = IntegerType::get(*Context, 32);
    for (auto I : VS->getComponents()) {
      if (I == static_cast<SPIRVWord>(-1))
        Components.push_back(UndefValue::get(Int32Ty));
      else
        Components.push_back(ConstantInt::get(Int32Ty, I));
    }
    return mapValue(BV, new ShuffleVectorInst(
      transValue(VS->getVector1(), F, BB),
      transValue(VS->getVector2(), F, BB),
      ConstantVector::get(Components),
      BV->getName(), BB));
    }
    break;

  case OpFunctionCall: {
    SPIRVFunctionCall *BC = static_cast<SPIRVFunctionCall *>(BV);
    assert(BB && "Invalid BB");
    auto Call = CallInst::Create(
      transFunction(BC->getFunction()),
      transValue(BC->getArgumentValues(), F, BB),
      BC->getName(),
      BB);
    setCallingConv(Call);
    setAttrByCalledFunc(Call);
    return mapValue(BV, Call);
    }
    break;

  case OpExtInst:
    return mapValue(BV, transOCLBuiltinFromExtInst(
      static_cast<SPIRVExtInst *>(BV), BB));
    break;

  case OpControlBarrier:
  case OpMemoryBarrier:
    return mapValue(BV, transOCLBarrierFence(
        static_cast<SPIRVInstruction *>(BV), BB));

  case OpSNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary*>(BV);
    return mapValue(BV, BinaryOperator::CreateNSWNeg(
      transValue(BC->getOperand(0), F, BB),
      BV->getName(), BB));
    }

  case OpFNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary*>(BV);
    return mapValue(BV, BinaryOperator::CreateFNeg(
      transValue(BC->getOperand(0), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpNot: {
    SPIRVUnary *BC = static_cast<SPIRVUnary*>(BV);
    return mapValue(BV, BinaryOperator::CreateNot(
      transValue(BC->getOperand(0), F, BB),
      BV->getName(), BB));
    }
    break;

  default: {
    auto OC = BV->getOpCode();
    if (isSPIRVCmpInstTransToLLVMInst(static_cast<SPIRVInstruction*>(BV))) {
      return mapValue(BV, transCmpInst(BV, BB, F));
    } else if (OCLSPIRVBuiltinMap::rfind(OC, nullptr) &&
               !isAtomicOpCode(OC) &&
               !isGroupOpCode(OC) &&
               !isPipeOpCode(OC)) {
      return mapValue(BV, transOCLBuiltinFromInst(
          static_cast<SPIRVInstruction *>(BV), BB));
    } else if (isBinaryShiftLogicalBitwiseOpCode(OC) ||
                isLogicalOpCode(OC)) {
          return mapValue(BV, transShiftLogicalBitwiseInst(BV, BB, F));
    } else if (isCvtOpCode(OC)) {
        auto BI = static_cast<SPIRVInstruction *>(BV);
        Value *Inst = nullptr;
        if (BI->hasFPRoundingMode() || BI->isSaturatedConversion())
          Inst = transOCLBuiltinFromInst(BI, BB);
        else
          Inst = transConvertInst(BV, F, BB);
        return mapValue(BV, Inst);
    }
    return mapValue(BV, transSPIRVBuiltinFromInst(
      static_cast<SPIRVInstruction *>(BV), BB));
  }

  SPIRVDBG(spvdbgs() << "Cannot translate " << *BV << '\n';)
  llvm_unreachable("Translation of SPIRV instruction not implemented");
  return NULL;
  }
}

template<class SourceTy, class FuncTy>
bool
SPIRVToLLVM::foreachFuncCtlMask(SourceTy Source, FuncTy Func) {
  SPIRVWord FCM = Source->getFuncCtlMask();
  SPIRSPIRVFuncCtlMaskMap::foreach([&](Attribute::AttrKind Attr,
      SPIRVFunctionControlMaskKind Mask){
    if (FCM & Mask)
      Func(Attr);
  });
  return true;
}

Function *
SPIRVToLLVM::transFunction(SPIRVFunction *BF) {
  auto Loc = FuncMap.find(BF);
  if (Loc != FuncMap.end())
    return Loc->second;

  auto IsKernel = BM->isEntryPoint(ExecutionModelKernel, BF->getId());
  auto Linkage = IsKernel ? GlobalValue::ExternalLinkage :
      SPIRSPIRVLinkageTypeMap::rmap(BF->getLinkageType());
  FunctionType *FT = dyn_cast<FunctionType>(transType(BF->getFunctionType()));
  Function *F = dyn_cast<Function>(mapValue(BF, Function::Create(FT, Linkage,
      BF->getName(), M)));
  mapFunction(BF, F);
  if (!F->isIntrinsic()) {
    F->setCallingConv(IsKernel ? CallingConv::SPIR_KERNEL :
        CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
    foreachFuncCtlMask(BF, [&](Attribute::AttrKind Attr){
      F->addFnAttr(Attr);
    });
  }

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
      ++I) {
    auto BA = BF->getArgument(I->getArgNo());
    mapValue(BA, I);
    setName(I, BA);
    BA->foreachAttr([&](SPIRVFuncParamAttrKind Kind){
      if (Kind == FunctionParameterAttributeNoWrite)
        return;
      F->addAttribute(I->getArgNo() + 1, SPIRSPIRVFuncParamAttrMap::rmap(Kind));
    });
  }
  BF->foreachReturnValueAttr([&](SPIRVFuncParamAttrKind Kind){
    if (Kind == FunctionParameterAttributeNoWrite)
      return;
    F->addAttribute(AttributeSet::ReturnIndex,
        SPIRSPIRVFuncParamAttrMap::rmap(Kind));
  });

  // Creating all basic blocks before creating instructions.
  for (size_t I = 0, E = BF->getNumBasicBlock(); I != E; ++I) {
    transValue(BF->getBasicBlock(I), F, nullptr);
  }

  for (size_t I = 0, E = BF->getNumBasicBlock(); I != E; ++I) {
    SPIRVBasicBlock *BBB = BF->getBasicBlock(I);
    BasicBlock *BB = dyn_cast<BasicBlock>(transValue(BBB, F, nullptr));
    for (size_t BI = 0, BE = BBB->getNumInst(); BI != BE; ++BI) {
      SPIRVInstruction *BInst = BBB->getInst(BI);
      transValue(BInst, F, BB, false);
    }
  }
  return F;
}

/// LLVM convert builtin functions is translated to two instructions:
/// y = i32 islessgreater(float x, float z) ->
///     y = i32 ZExt(bool LessGreater(float x, float z))
/// When translating back, for simplicity, a trunc instruction is inserted
/// w = bool LessGreater(float x, float z) ->
///     w = bool Trunc(i32 islessgreater(float x, float z))
/// Optimizer should be able to remove the redundant trunc/zext
void
SPIRVToLLVM::transOCLBuiltinFromInstPreproc(SPIRVInstruction* BI, Type *&RetTy,
    std::vector<SPIRVValue *> &Args) {
  if (!BI->hasType())
    return;
  auto BT = BI->getType();
  auto OC = BI->getOpCode();
  if (isCmpOpCode(BI->getOpCode())) {
    if (BT->isTypeBool())
      RetTy = IntegerType::getInt32Ty(*Context);
    else if (BT->isTypeVectorBool())
      RetTy = VectorType::get(IntegerType::get(*Context,
          Args[0]->getType()->getVectorComponentType()->isTypeFloat(64)?64:32),
          BT->getVectorComponentCount());
    else
       llvm_unreachable("invalid compare instruction");
  } else if (OC == OpGenericCastToPtrExplicit)
    Args.pop_back();
}

Instruction*
SPIRVToLLVM::transOCLBuiltinPostproc(SPIRVInstruction* BI,
    CallInst* CI, BasicBlock* BB, const std::string &DemangledName) {
  auto OC = BI->getOpCode();
  if (isCmpOpCode(OC) &&
      BI->getType()->isTypeVectorOrScalarBool()) {
    return CastInst::Create(Instruction::Trunc, CI, transType(BI->getType()),
        "cvt", BB);
  }
  if (OC == OpImageSampleExplicitLod)
    return postProcessOCLReadImage(BI, CI, DemangledName);
  if (SPIRVEnableStepExpansion &&
      (DemangledName == "smoothstep" ||
       DemangledName == "step"))
    return expandOCLBuiltinWithScalarArg(CI, DemangledName);
  return CI;
}

Instruction *
SPIRVToLLVM::transBuiltinFromInst(const std::string& FuncName,
    SPIRVInstruction* BI, BasicBlock* BB) {
  std::string MangledName;
  auto Ops = BI->getOperands();
  Type* RetTy = BI->hasType() ? transType(BI->getType()) :
      Type::getVoidTy(*Context);
  transOCLBuiltinFromInstPreproc(BI, RetTy, Ops);
  std::vector<Type*> ArgTys = transTypeVector(
      SPIRVInstruction::getOperandTypes(Ops));
  bool HasFuncPtrArg = false;
  for (auto& I:ArgTys) {
    if (isa<FunctionType>(I)) {
      I = PointerType::get(I, SPIRAS_Private);
      HasFuncPtrArg = true;
    }
  }
  if (!HasFuncPtrArg)
    MangleOpenCLBuiltin(FuncName, ArgTys, MangledName);
  else
    MangledName = decorateSPIRVFunction(FuncName);
  Function* Func = M->getFunction(MangledName);
  FunctionType* FT = FunctionType::get(RetTy, ArgTys, false);
  // ToDo: Some intermediate functions have duplicate names with
  // different function types. This is OK if the function name
  // is used internally and finally translated to unique function
  // names. However it is better to have a way to differentiate
  // between intermidiate functions and final functions and make
  // sure final functions have unique names.
  SPIRVDBG(
  if (!HasFuncPtrArg && Func && Func->getFunctionType() != FT) {
    dbgs() << "Warning: Function name conflict:\n"
       << *Func << '\n'
       << " => " << *FT << '\n';
  }
  )
  if (!Func || Func->getFunctionType() != FT) {
    DEBUG(for (auto& I:ArgTys) {
      dbgs() << *I << '\n';
    });
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);
  }
  auto Call = CallInst::Create(Func,
      transValue(Ops, BB->getParent(), BB), "", BB);
  setName(Call, BI);
  setAttrByCalledFunc(Call);
  SPIRVDBG(spvdbgs() << "[transInstToBuiltinCall] " << *BI << " -> "; dbgs() <<
      *Call << '\n';)
  Instruction *Inst = Call;
  Inst = transOCLBuiltinPostproc(BI, Call, BB, FuncName);
  return Inst;
}

std::string
SPIRVToLLVM::getOCLBuiltinName(SPIRVInstruction* BI) {
  auto OC = BI->getOpCode();
  if (OC == OpGenericCastToPtrExplicit)
    return getOCLGenericCastToPtrName(BI);
  if (isCvtOpCode(OC))
    return getOCLConvertBuiltinName(BI);
  if (OC == OpBuildNDRange) {
    auto NDRangeInst = static_cast<SPIRVBuildNDRange *>(BI);
    auto EleTy = ((NDRangeInst->getOperands())[0])->getType();
    int Dim = EleTy->isTypeArray() ? EleTy->getArrayLength() : 1;
    // cygwin does not have std::to_string
    ostringstream OS;
    OS << Dim;
    assert((EleTy->isTypeInt() && Dim == 1) ||
        (EleTy->isTypeArray() && Dim >= 2 && Dim <= 3));
    return std::string(kOCLBuiltinName::NDRangePrefix) + OS.str() + "D";
  }
  auto Name = OCLSPIRVBuiltinMap::rmap(OC);

  SPIRVType *T = nullptr;
  switch(OC) {
  case OpImageRead:
    T = BI->getType();
    break;
  case OpImageWrite:
    T = BI->getOperands()[2]->getType();
    break;
  default:
    // do nothing
    break;
  }
  if (T && T->isTypeVector())
    T = T->getVectorComponentType();
  if (T)
    Name += T->isTypeFloat()?'f':'i';

  return Name;
}

Instruction *
SPIRVToLLVM::transOCLBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB) {
  assert(BB && "Invalid BB");
  auto FuncName = getOCLBuiltinName(BI);
  return transBuiltinFromInst(FuncName, BI, BB);
}

Instruction *
SPIRVToLLVM::transSPIRVBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB) {
  assert(BB && "Invalid BB");
  return transBuiltinFromInst(getSPIRVFuncName(BI->getOpCode()), BI, BB);
}

bool
SPIRVToLLVM::translate() {
  if (!transAddressingModel())
    return false;

  DbgTran.createCompileUnit();
  DbgTran.addDbgInfoVersion();

  for (unsigned I = 0, E = BM->getNumVariables(); I != E; ++I) {
    auto BV = BM->getVariable(I);
    if (BV->getStorageClass() != StorageClassFunction)
      transValue(BV, nullptr, nullptr);
  }

  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    transFunction(BM->getFunction(I));
  }
  if (!transKernelMetadata())
    return false;
  if (!transFPContractMetadata())
    return false;
  if (!transSourceLanguage())
    return false;
  if (!transSourceExtension())
    return false;
  transGeneratorMD();
  if (!transOCLBuiltinsFromVariables())
    return false;
  if (!postProcessOCL())
    return false;
  eraseUselessFunctions(M);
  DbgTran.finalize();
  return true;
}

bool
SPIRVToLLVM::transAddressingModel() {
  switch (BM->getAddressingModel()) {
  case AddressingModelPhysical64:
    M->setTargetTriple(SPIR_TARGETTRIPLE64);
    M->setDataLayout(SPIR_DATALAYOUT64);
    break;
  case AddressingModelPhysical32:
    M->setTargetTriple(SPIR_TARGETTRIPLE32);
    M->setDataLayout(SPIR_DATALAYOUT32);
    break;
  case AddressingModelLogical:
    // Do not set target triple and data layout
    break;
  default:
    SPIRVCKRT(0, InvalidAddressingModel, "Actual addressing mode is " +
        (unsigned)BM->getAddressingModel());
  }
  return true;
}

bool
SPIRVToLLVM::transDecoration(SPIRVValue *BV, Value *V) {
  if (!transAlign(BV, V))
    return false;
  DbgTran.transDbgInfo(BV, V);
  return true;
}

bool
SPIRVToLLVM::transFPContractMetadata() {
  bool ContractOff = false;
  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    SPIRVFunction *BF = BM->getFunction(I);
    if (!isOpenCLKernel(BF))
      continue;
    if (BF->getExecutionMode(ExecutionModeContractionOff)) {
      ContractOff = true;
      break;
    }
  }
  if (!ContractOff)
    M->getOrInsertNamedMetadata(kSPIR2MD::FPContract);
  return true;
}

std::string SPIRVToLLVM::transOCLImageTypeAccessQualifier(
    SPIRV::SPIRVTypeImage* ST) {
  return SPIRSPIRVAccessQualifierMap::rmap(ST->getAccessQualifier());
}

bool
SPIRVToLLVM::transKernelMetadata() {
  NamedMDNode *KernelMDs = M->getOrInsertNamedMetadata(SPIR_MD_KERNELS);
  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    SPIRVFunction *BF = BM->getFunction(I);
    Function *F = static_cast<Function *>(getTranslatedValue(BF));
    assert(F && "Invalid translated function");
    if (F->getCallingConv() != CallingConv::SPIR_KERNEL)
      continue;
    std::vector<llvm::Metadata*> KernelMD;
    KernelMD.push_back(ValueAsMetadata::get(F));

    // Generate metadata for kernel_arg_address_spaces
    addOCLKernelArgumentMetadata(Context, KernelMD,
        SPIR_MD_KERNEL_ARG_ADDR_SPACE, BF,
        [=](SPIRVFunctionParameter *Arg){
      SPIRVType *ArgTy = Arg->getType();
      SPIRAddressSpace AS = SPIRAS_Private;
      if (ArgTy->isTypePointer())
        AS = SPIRSPIRVAddrSpaceMap::rmap(ArgTy->getPointerStorageClass());
      else if (ArgTy->isTypeOCLImage() || ArgTy->isTypePipe())
        AS = SPIRAS_Global;
      return ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(*Context), AS));
    });
    // Generate metadata for kernel_arg_access_qual
    addOCLKernelArgumentMetadata(Context, KernelMD,
        SPIR_MD_KERNEL_ARG_ACCESS_QUAL, BF,
        [=](SPIRVFunctionParameter *Arg){
      std::string Qual;
      auto T = Arg->getType();
      if (T->isTypeOCLImage()) {
        auto ST = static_cast<SPIRVTypeImage *>(T);
        Qual = transOCLImageTypeAccessQualifier(ST);
      } else if (T->isTypePipe()){
        auto PT = static_cast<SPIRVTypePipe *>(T);
        Qual = transOCLPipeTypeAccessQualifier(PT);
      } else
        Qual = "none";
      return MDString::get(*Context, Qual);
    });
    // Generate metadata for kernel_arg_type
    addOCLKernelArgumentMetadata(Context, KernelMD,
        SPIR_MD_KERNEL_ARG_TYPE, BF,
        [=](SPIRVFunctionParameter *Arg){
      return transOCLKernelArgTypeName(Arg);
    });
    // Generate metadata for kernel_arg_type_qual
    addOCLKernelArgumentMetadata(Context, KernelMD,
        SPIR_MD_KERNEL_ARG_TYPE_QUAL, BF,
        [=](SPIRVFunctionParameter *Arg){
      std::string Qual;
      if (Arg->hasDecorate(DecorationVolatile))
        Qual = kOCLTypeQualifierName::Volatile;
      Arg->foreachAttr([&](SPIRVFuncParamAttrKind Kind){
        Qual += Qual.empty() ? "" : " ";
        switch(Kind){
        case FunctionParameterAttributeNoAlias:
          Qual += kOCLTypeQualifierName::Restrict;
          break;
        case FunctionParameterAttributeNoWrite:
          Qual += kOCLTypeQualifierName::Const;
          break;
        default:
          // do nothing.
          break;
        }
      });
      if (Arg->getType()->isTypePipe()) {
        Qual += Qual.empty() ? "" : " ";
        Qual += kOCLTypeQualifierName::Pipe;
      }
      return MDString::get(*Context, Qual);
    });
    // Generate metadata for kernel_arg_base_type
    addOCLKernelArgumentMetadata(Context, KernelMD,
        SPIR_MD_KERNEL_ARG_BASE_TYPE, BF,
        [=](SPIRVFunctionParameter *Arg){
      return transOCLKernelArgTypeName(Arg);
    });
    // Generate metadata for kernel_arg_name
    if (SPIRVGenKernelArgNameMD) {
      bool ArgHasName = true;
      BF->foreachArgument([&](SPIRVFunctionParameter *Arg){
        ArgHasName &= !Arg->getName().empty();
      });
      if (ArgHasName)
        addOCLKernelArgumentMetadata(Context, KernelMD,
            SPIR_MD_KERNEL_ARG_NAME, BF,
            [=](SPIRVFunctionParameter *Arg){
          return MDString::get(*Context, Arg->getName());
        });
    }
    // Generate metadata for reqd_work_group_size
    if (auto EM = BF->getExecutionMode(ExecutionModeLocalSize)) {
      KernelMD.push_back(getMDNodeStringIntVec(Context,
          kSPIR2MD::WGSize, EM->getLiterals()));
    }
    // Generate metadata for work_group_size_hint
    if (auto EM = BF->getExecutionMode(ExecutionModeLocalSizeHint)) {
      KernelMD.push_back(getMDNodeStringIntVec(Context,
          kSPIR2MD::WGSizeHint, EM->getLiterals()));
    }
    // Generate metadata for vec_type_hint
    if (auto EM = BF->getExecutionMode(ExecutionModeVecTypeHint)) {
      std::vector<Metadata*> MetadataVec;
      MetadataVec.push_back(MDString::get(*Context, kSPIR2MD::VecTyHint));
      Type *VecHintTy = decodeVecTypeHint(*Context, EM->getLiterals()[0]);
      MetadataVec.push_back(ValueAsMetadata::get(UndefValue::get(VecHintTy)));
      MetadataVec.push_back(
          ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context),
              0)));
      KernelMD.push_back(MDNode::get(*Context, MetadataVec));
    }

    llvm::MDNode *Node = MDNode::get(*Context, KernelMD);
    KernelMDs->addOperand(Node);
  }
  return true;
}

bool
SPIRVToLLVM::transAlign(SPIRVValue *BV, Value *V) {
  if (auto AL = dyn_cast<AllocaInst>(V)) {
    SPIRVWord Align = 0;
    if (BV->hasAlignment(&Align))
      AL->setAlignment(Align);
    return true;
  }
  if (auto GV = dyn_cast<GlobalVariable>(V)) {
    SPIRVWord Align = 0;
    if (BV->hasAlignment(&Align))
      GV->setAlignment(Align);
    return true;
  }
  return true;
}

void
SPIRVToLLVM::transOCLVectorLoadStore(std::string& UnmangledName,
    std::vector<SPIRVWord> &BArgs) {
  if (UnmangledName.find("vload") == 0 &&
      UnmangledName.find("n") != std::string::npos) {
    if (BArgs.back() != 1) {
      std::stringstream SS;
      SS << BArgs.back();
      UnmangledName.replace(UnmangledName.find("n"), 1, SS.str());
    } else {
      UnmangledName.erase(UnmangledName.find("n"), 1);
    }
    BArgs.pop_back();
  } else if (UnmangledName.find("vstore") == 0) {
    if (UnmangledName.find("n") != std::string::npos) {
      auto T = BM->getValueType(BArgs[0]);
      if (T->isTypeVector()) {
        auto W = T->getVectorComponentCount();
        std::stringstream SS;
        SS << W;
        UnmangledName.replace(UnmangledName.find("n"), 1, SS.str());
      } else {
        UnmangledName.erase(UnmangledName.find("n"), 1);
      }
    }
    if (UnmangledName.find("_r") != std::string::npos) {
      UnmangledName.replace(UnmangledName.find("_r"), 2, std::string("_") +
          SPIRSPIRVFPRoundingModeMap::rmap(static_cast<SPIRVFPRoundingModeKind>(
              BArgs.back())));
      BArgs.pop_back();
    }
   }
}

// printf is not mangled. The function type should have just one argument.
// read_image*: the second argument should be mangled as sampler.
Instruction *
SPIRVToLLVM::transOCLBuiltinFromExtInst(SPIRVExtInst *BC, BasicBlock *BB) {
  assert(BB && "Invalid BB");
  std::string MangledName;
  SPIRVWord EntryPoint = BC->getExtOp();
  SPIRVExtInstSetKind Set = BM->getBuiltinSet(BC->getExtSetId());
  bool IsVarArg = false;
  bool IsPrintf = false;
  std::string UnmangledName;
  auto BArgs = BC->getArguments();

  assert (Set == SPIRVEIS_OpenCL && "Not OpenCL extended instruction");
  if (EntryPoint == OpenCLLIB::Printf)
    IsPrintf = true;
  else {
    UnmangledName = OCLExtOpMap::map(static_cast<OCLExtOpKind>(
        EntryPoint));
  }

  SPIRVDBG(spvdbgs() << "[transOCLBuiltinFromExtInst] OrigUnmangledName: " <<
      UnmangledName << '\n');
  transOCLVectorLoadStore(UnmangledName, BArgs);

  std::vector<Type *> ArgTypes = transTypeVector(BC->getValueTypes(BArgs));

  if (IsPrintf) {
    MangledName = "printf";
    IsVarArg = true;
    ArgTypes.resize(1);
  } else if (UnmangledName.find("read_image") == 0) {
    auto ModifiedArgTypes = ArgTypes;
    ModifiedArgTypes[1] = getOrCreateOpaquePtrType(M, "opencl.sampler_t");
    MangleOpenCLBuiltin(UnmangledName, ModifiedArgTypes, MangledName);
  } else {
    MangleOpenCLBuiltin(UnmangledName, ArgTypes, MangledName);
  }
  SPIRVDBG(spvdbgs() << "[transOCLBuiltinFromExtInst] ModifiedUnmangledName: " <<
      UnmangledName << " MangledName: " << MangledName << '\n');

  FunctionType *FT = FunctionType::get(
      transType(BC->getType()),
      ArgTypes,
      IsVarArg);
  Function *F = M->getFunction(MangledName);
  if (!F) {
    F = Function::Create(FT,
      GlobalValue::ExternalLinkage,
      MangledName,
      M);
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
  }
  auto Args = transValue(BC->getValues(BArgs), F, BB);
  SPIRVDBG(dbgs() << "[transOCLBuiltinFromExtInst] Function: " << *F <<
      ", Args: ";
    for (auto &I:Args) dbgs() << *I << ", "; dbgs() << '\n');
  CallInst *Call = CallInst::Create(F,
      Args,
      BC->getName(),
      BB);
  setCallingConv(Call);
  addFnAttr(Context, Call, Attribute::NoUnwind);
  return transOCLBuiltinPostproc(BC, Call, BB, UnmangledName);
}

Instruction *
SPIRVToLLVM::transOCLBarrierFence(SPIRVInstruction* MB, BasicBlock *BB) {
  assert(BB && "Invalid BB");
  std::string FuncName;
  SPIRVWord MemSema = 0;
  if (MB->getOpCode() == OpMemoryBarrier) {
    auto MemB = static_cast<SPIRVMemoryBarrier*>(MB);
    FuncName = "mem_fence";
    MemSema = MemB->getOpWord(1);
  } else if (MB->getOpCode() == OpControlBarrier) {
    auto CtlB = static_cast<SPIRVControlBarrier*>(MB);
    SPIRVWord Ver = 1;
    BM->getSourceLanguage(&Ver);
    FuncName = (Ver <= 12) ? kOCLBuiltinName::Barrier :
        kOCLBuiltinName::WorkGroupBarrier;
    MemSema = CtlB->getMemSemantic();
  } else {
    llvm_unreachable("Invalid instruction");
  }
  std::string MangledName;
  Type* Int32Ty = Type::getInt32Ty(*Context);
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* ArgTy[] = {Int32Ty};
  MangleOpenCLBuiltin(FuncName, ArgTy, MangledName);
  Function *Func = M->getFunction(MangledName);
  if (!Func) {
    FunctionType *FT = FunctionType::get(VoidTy, ArgTy, false);
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);
  }
  Value *Arg[] = {ConstantInt::get(Int32Ty,
      rmapBitMask<OCLMemFenceMap>(MemSema))};
  auto Call = CallInst::Create(Func, Arg, "", BB);
  setName(Call, MB);
  setAttrByCalledFunc(Call);
  SPIRVDBG(spvdbgs() << "[transBarrier] " << *MB << " -> ";
    dbgs() << *Call << '\n';)
  return Call;
}

// SPIR-V only contains language version. Use OpenCL language version as
// SPIR version.
bool
SPIRVToLLVM::transSourceLanguage() {
  SPIRVWord Ver = 0;
  SourceLanguage Lang = BM->getSourceLanguage(&Ver);
  assert((Lang == SourceLanguageOpenCL_C ||
      Lang == SourceLanguageOpenCL_CPP) && "Unsupported source language");
  unsigned short Major = 0;
  unsigned char Minor = 0;
  unsigned char Rev = 0;
  std::tie(Major, Minor, Rev) = decodeOCLVer(Ver);
  SPIRVMDBuilder Builder(*M);
  Builder.addNamedMD(kSPIRVMD::Source)
            .addOp()
              .add(Lang)
              .add(Ver)
              .done();
  // ToDo: Phasing out usage of old SPIR metadata
  if (Ver <= kOCLVer::CL12)
    addOCLVersionMetadata(Context, M, kSPIR2MD::SPIRVer, 1, 2);
  else
    addOCLVersionMetadata(Context, M, kSPIR2MD::SPIRVer, 2, 0);

  addOCLVersionMetadata(Context, M, kSPIR2MD::OCLVer, Major, Minor);
  return true;
}

bool
SPIRVToLLVM::transSourceExtension() {
  auto ExtSet = rmap<OclExt::Kind>(BM->getExtension());
  auto CapSet = rmap<OclExt::Kind>(BM->getCapability());
  for (auto &I:CapSet)
    ExtSet.insert(I);
  auto OCLExtensions = getStr(map<std::string>(ExtSet));
  std::string OCLOptionalCoreFeatures;
  bool First = true;
  static const char *OCLOptCoreFeatureNames[] = {
      "cl_images",
      "cl_doubles",
  };
  for (auto &I:OCLOptCoreFeatureNames) {
    size_t Loc = OCLExtensions.find(I);
    if (Loc != std::string::npos) {
      OCLExtensions.erase(Loc, strlen(I));
      if (First)
        First = false;
      else
        OCLOptionalCoreFeatures += ' ';
      OCLOptionalCoreFeatures += I;
    }
  }
  addNamedMetadataString(Context, M, kSPIR2MD::Extensions, OCLExtensions);
  addNamedMetadataString(Context, M, kSPIR2MD::OptFeatures,
      OCLOptionalCoreFeatures);
  return true;
}

// If the argument is unsigned return uconvert*, otherwise return convert*.
std::string
SPIRVToLLVM::getOCLConvertBuiltinName(SPIRVInstruction* BI) {
  auto OC = BI->getOpCode();
  assert(isCvtOpCode(OC) && "Not convert instruction");
  auto U = static_cast<SPIRVUnary *>(BI);
  std::string Name;
  if (isCvtFromUnsignedOpCode(OC))
    Name = "u";
  Name += "convert_";
  Name += mapSPIRVTypeToOCLType(U->getType(),
      !isCvtToUnsignedOpCode(OC));
  SPIRVFPRoundingModeKind Rounding = FPRoundingModeCount;
  if (U->isSaturatedConversion())
    Name += "_sat";
  if (U->hasFPRoundingMode(&Rounding)) {
    Name += "_";
    Name += SPIRSPIRVFPRoundingModeMap::rmap(Rounding);
  }
  return Name;
}

//Check Address Space of the Pointer Type
std::string
SPIRVToLLVM::getOCLGenericCastToPtrName(SPIRVInstruction* BI) {
  auto GenericCastToPtrInst = BI->getType()->getPointerStorageClass();
  switch (GenericCastToPtrInst) {
    case StorageClassCrossWorkgroup:
      return std::string(kOCLBuiltinName::ToGlobal);
    case StorageClassWorkgroup:
      return std::string(kOCLBuiltinName::ToLocal);
    case StorageClassFunction:
      return std::string(kOCLBuiltinName::ToPrivate);
    default:
      llvm_unreachable("Invalid address space");
      return "";
  }
}

}

bool
llvm::ReadSPIRV(LLVMContext &C, std::istream &IS, Module *&M,
    std::string &ErrMsg) {
  M = new Module("", C);
  std::unique_ptr<SPIRVModule> BM(SPIRVModule::createSPIRVModule());

  BM->setAutoAddCapability(false);
  IS >> *BM;

  SPIRVToLLVM BTL(M, BM.get());
  bool Succeed = true;
  if (!BTL.translate()) {
    BM->getError(ErrMsg);
    Succeed = false;
  }
  PassManager PassMgr;
  PassMgr.add(createSPIRVToOCL20());
  PassMgr.add(createOCL20To12());
  PassMgr.run(*M);

  if (DbgSaveTmpLLVM)
    dumpLLVM(M, DbgTmpLLVMFileName);
  if (!Succeed) {
    delete M;
    M = nullptr;
  }
  return Succeed;
}
