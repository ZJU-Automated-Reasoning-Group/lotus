#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"

#include "boost/algorithm/string.hpp"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

#define BARRIER_CONSTRAINTS "~{memory},~{dirflag},~{fpsr},~{flags}"
#define BARRIER_DATA_CONSTRAINTS "r,~{memory},~{dirflag},~{fpsr},~{flags}"
#define BUILD_U64_CONSTRAINTS "=A,{ax},{dx},~{dirflag},~{fpsr},~{flags}"
#define ARCH_ATOMIC64_XCHG_CONSTRAINTS                                         \
  "=&A,i,{si},{bx},{cx},~{memory},~{dirflag},~{fpsr},~{flags}"
#define OPTIMIZER_HIDE_VAR_CONSTRAINTS "=r,0,~{dirflag},~{fpsr},~{flags}"

#define BIT_TEST_PREFIX "btq $2,$1"
#define BIT_TEST_AND_SET_1_0_PREFIX "btsq $1,$0"
#define BIT_TEST_AND_SET_2_0_PREFIX "btsq $2,$0"
#define BIT_TEST_AND_SET_2_1_PREFIX "btsq $2,$1"
#define BIT_TEST_AND_RESET_1_0_PREFIX "btrq $1,$0"
#define BIT_TEST_AND_RESET_2_0_PREFIX "btrq $2,$0"
#define BIT_TEST_AND_RESET_2_1_PREFIX "btrq $2,$1"

#define INCL "incl $0"
#define DECL_PREFIX "decl $0"
#define ADD_WITH_CARRY "addl $2,$0;adcl $$0,$0"
#define XADDL "xaddl $0,$1"
#define XADDQ "xaddq ${0:q},$1"
#define MOVB_0_1 "movb $0,$1"
#define MOVB_1_0 "movb $1,$0"
#define MOVW_0_1 "movw $0,$1"
#define MOVW_1_0 "movw $1,$0"
#define MOVL_0_1 "movl $0,$1"
#define MOVL_1_0 "movl $1,$0"
#define MOVQ_0_1 "movq $0,$1"
#define MOVQ_1_0 "movq $1,$0"
#define MOVQ_GS_1_0 "movq %gs:$1,$0"
#define MOVQ_POSITION_INDEPENDENT "movq ${1:P},$0"
#define MOVQ_GS_POSITION_INDEPENDENT "movq %gs:${1:P},$0"
#define ADDQ_1_0 "addq $1,$0"
#define ADDQ_2_0_PREFIX "addq $2,$0"
#define ADDL_1_0 "addl $1,$0"
#define ADDL_2_0_PREFIX "addl $2,$0"
#define ANDB_1_0 "andb ${1:b},$0"
#define ANDB_2_1_PREFIX "andb $2,$1"
#define ANDL "andl $1,$0"
#define ANDQ "andq $1,$0"
#define MULL "mull $3"
#define DIVL "divl $2"
#define ORB "orb ${1:b},$0"
#define ORL "orl $1,$0"
#define ORQ "orq $1,$0"
#define CMPXCHGL21 "cmpxchgl $2,$1"
#define CMPXCHGL21_CONSTRAINTS                                                 \
  "={ax},=*m,r,0,*m,~{memory},~{dirflag},~{fpsr},~{flags}"
#define CMPXCHGL31_PREFIX "cmpxchgl $3,$1"
#define CMPXCHGL31_CONSTRAINTS                                                 \
  "={@ccz},=*m,={ax},r,*m,2,~{memory},~{dirflag},~{fpsr},~{flags}"
#define CMPXCHGQ21 "cmpxchgq $2,$1"
#define CMPXCHGQ21_CONSTRAINTS                                                 \
  "={ax},=*m,r,0,*m,~{memory},~{dirflag},~{fpsr},~{flags}"
#define CMPXCHGQ31_PREFIX "cmpxchgq $3,$1"
#define CMPXCHGQ31_CONSTRAINTS                                                 \
  "={@ccz},=*m,={ax},r,*m,2,~{memory},~{dirflag},~{fpsr},~{flags}"
#define CMPXCHG8B "cmpxchg8b $1"
#define XCHGL "xchgl $0,$1"
#define XCHGQ "xchgq ${0:q},$1"
#define FFS "rep; bsf $1,$0"
#define FLSL "bsrl $1,$0"
#define FLSQ "bsrq $1,${0:q}"
#define CLI "cli"
#define STI "sti"
#define RDPMC "rdpmc"
#define CALL0 "call ${0:P}"
#define CALL1 "call ${1:P}"
#define CALL2 "call ${2:P}"
#define ARRAY_INDEX_MASK_NOSPEC "cmp $1,$2; sbb $0,$0;"
#define CPUID "cpuid"
#define INB "inb ${1:w},${0:b}"
#define INW "inw ${1:w},${0:w}"
#define INL "inl ${1:w},$0"
#define OUTB "outb ${0:b},${1:w}"
#define OUTW "outw ${0:w},${1:w}"
#define OUTL "outl $0,${1:w}"
#define OUT_AL_0x80 "outb %al,$$0x80"
#define OUT_AL_0xed "outb %al,$$0xed"

#define UD2 ".byte 0x0f,0x0b"
#define INT3 "int3"
#define SERIALIZE ".byte 0xf,0x1,0xe8"
#define IRET_TO_SELF "pushfl;pushl %cs;pushl $$1f;iret;1:"
#define SET_DEBUG_REGISTER_PREFIX "mov $0,%db"
#define NOP "rep; nop"
#define LFENCE "lfence"
#define SFENCE "sfence"
#define MFENCE "mfence"
#define RDSEED_PREFIX "rdseed $1"
#define RDRAND_PREFIX "rdrand $1"

#define LOAD_CR3 "mov $0,%cr3"
#define LIDT "lidt $0"

#define GET_USER "call __get_user_nocheck_${4:P}"
#define GET_USER_CONSTRAINTS                                                   \
  "={ax},={edx},={esp},0,i,{esp},~{dirflag},~{fpsr},~{flags}"

#define CALL_ON_STACK "xchgl%ebx,%esp;call *$2;movl%ebx,%esp;"

#define HWEIGHT                                                                \
  "# ALT: oldnstr;661:;call __sw_hweight32;662:;# ALT: padding;.skip "         \
  "-(((6651f-6641f)-(662b-661b)) > 0) * "                                      \
  "((6651f-6641f)-(662b-661b)),0x90;663:;.pushsection "                        \
  ".altinstructions,\"a\";.long 661b - .;.long 6641f - .;.4byte ( "            \
  "4*32+23);.byte 663b-661b;.byte 6651f-6641f;.popsection;.pushsection "       \
  ".altinstr_replacement,\"ax\";# ALT: replacement 1;6641:;popcntl "           \
  "$1,$0;6651:;.popsection"
#define NATIVE_READ_MSR_SAFE                                                   \
  "1: rdmsr ; xor $0,$0;2:; .pushsection \"__ex_table\",\"a\"; .balign 4; "    \
  ".long "                                                                     \
  "(1b) - .; .long (2b) - .;.macro extable_type_reg type:req reg:req;.set "    \
  ".Lfound,0;.set .Lregnr,0;.irp "                                             \
  "rs,rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15;.ifc "     \
  "\\reg,%\\rs;.set .Lfound,.Lfound+1;.long \\type + (.Lregnr << "             \
  "8);.endif;.set .Lregnr,.Lregnr+1;.endr;.set .Lregnr,0;.irp "                \
  "rs,eax,ecx,edx,ebx,esp,ebp,esi,edi,r8d,r9d,r10d,r11d,r12d,r13d,r14d,r15d;." \
  "ifc \\reg,%\\rs;.set .Lfound,.Lfound+1;.long \\type + (.Lregnr << "         \
  "8);.endif;.set .Lregnr,.Lregnr+1;.endr;.if (.Lfound != 1);.error "          \
  "\"extable_type_reg: bad register argument\";.endif;.endm;extable_type_reg " \
  "reg=$0,type=11 ;.purgem extable_type_reg; .popsection"
#define NATIVE_WRITE_MSR_SAFE                                                  \
  "1: wrmsr ; xor $0,$0;2:; .pushsection \"__ex_table\",\"a\"; .balign 4; "    \
  ".long "                                                                     \
  "(1b) - .; .long (2b) - .;.macro extable_type_reg type:req reg:req;.set "    \
  ".Lfound,0;.set .Lregnr,0;.irp "                                             \
  "rs,rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15;.ifc "     \
  "\\reg,%\\rs;.set .Lfound,.Lfound+1;.long \\type + (.Lregnr << "             \
  "8);.endif;.set .Lregnr,.Lregnr+1;.endr;.set .Lregnr,0;.irp "                \
  "rs,eax,ecx,edx,ebx,esp,ebp,esi,edi,r8d,r9d,r10d,r11d,r12d,r13d,r14d,r15d;." \
  "ifc \\reg,%\\rs;.set .Lfound,.Lfound+1;.long \\type + (.Lregnr << "         \
  "8);.endif;.set .Lregnr,.Lregnr+1;.endr;.if (.Lfound != 1);.error "          \
  "\"extable_type_reg: bad register argument\";.endif;.endm;extable_type_reg " \
  "reg=$0,type=10 ;.purgem extable_type_reg; .popsection"
#define RDMSR                                                                  \
  "1: rdmsr;2:; .pushsection \"__ex_table\",\"a\"; .balign 4; .long (1b) - "   \
  ".; "                                                                        \
  ".long (2b) - .; .long 9 ; .popsection"
#define WRMSR                                                                  \
  "1: wrmsr;2:; .pushsection \"__ex_table\",\"a\"; .balign 4; .long (1b) - "   \
  ".; "                                                                        \
  ".long (2b) - .; .long 8 ; .popsection"
#define MB                                                                     \
  "# ALT: oldnstr;661:;lock; addl $$0,-4(%esp);662:;# ALT: padding;.skip "     \
  "-(((6651f-6641f)-(662b-661b)) > 0) * "                                      \
  "((6651f-6641f)-(662b-661b)),0x90;663:;.pushsection "                        \
  ".altinstructions,\"a\"; "                                                   \
  ".long 661b - .; .long 6641f - .; .word ( 0*32+26); .byte 663b-661b; .byte " \
  "6651f-6641f;.popsection;.pushsection .altinstr_replacement,\"ax\";# ALT: "  \
  "replacement 1;6641:;mfence;6651:;.popsection"
#define RMB                                                                    \
  "# ALT: oldnstr;661:;lock; addl $$0,-4(%esp);662:;# ALT: padding;.skip "     \
  "-(((6651f-6641f)-(662b-661b)) > 0) * "                                      \
  "((6651f-6641f)-(662b-661b)),0x90;663:;.pushsection "                        \
  ".altinstructions,\"a\"; "                                                   \
  ".long 661b - .; .long 6641f - .; .word ( 0*32+26); .byte 663b-661b; .byte " \
  "6651f-6641f;.popsection;.pushsection .altinstr_replacement,\"ax\";# ALT: "  \
  "replacement 1;6641:;lfence;6651:;.popsection"
#define WMB                                                                    \
  "# ALT: oldnstr;661:;lock; addl $$0,-4(%esp);662:;# ALT: padding;.skip "     \
  "-(((6651f-6641f)-(662b-661b)) > 0) * "                                      \
  "((6651f-6641f)-(662b-661b)),0x90;663:;.pushsection "                        \
  ".altinstructions,\"a\"; "                                                   \
  ".long 661b - .; .long 6641f - .; .word ( 0*32+26); .byte 663b-661b; .byte " \
  "6651f-6641f;.popsection;.pushsection .altinstr_replacement,\"ax\";# ALT: "  \
  "replacement 1;6641:;sfence;6651:;.popsection"
#define LOAD_GS                                                                \
  "1:movl ${0:k},%gs; .pushsection \"__ex_table\",\"a\"; .balign 4; .long "    \
  "(1b) "                                                                      \
  "- .; .long (1b) - .;.macro extable_type_reg type:req reg:req;.set "         \
  ".Lfound,0;.set .Lregnr,0;.irp "                                             \
  "rs,rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15;.ifc "     \
  "\\reg,%\\rs;.set .Lfound,.Lfound+1;.long \\type + (.Lregnr << "             \
  "8);.endif;.set .Lregnr,.Lregnr+1;.endr;.set .Lregnr,0;.irp "                \
  "rs,eax,ecx,edx,ebx,esp,ebp,esi,edi,r8d,r9d,r10d,r11d,r12d,r13d,r14d,r15d;." \
  "ifc \\reg,%\\rs;.set .Lfound,.Lfound+1;.long \\type + (.Lregnr << "         \
  "8);.endif;.set .Lregnr,.Lregnr+1;.endr;.if (.Lfound != 1);.error "          \
  "\"extable_type_reg: bad register argument\";.endif;.endm;extable_type_reg " \
  "reg=${0:k},type=(17 $| ((0) << 16)) ;.purgem extable_type_reg; "            \
  ".popsection;"
#define RDTSC "rdtsc"
#define RDTSC_ORDERED                                                          \
  "# ALT: oldinstr2;661:;rdtsc;662:;# ALT: padding2;.skip -((((6651f-6641f) "  \
  "^ (((6651f-6641f) ^ (6652f-6642f)) & -(-((6651f-6641f) < "                  \
  "(6652f-6642f))))) - (662b-661b)) > 0) * (((6651f-6641f) ^ (((6651f-6641f) " \
  "^ (6652f-6642f)) & -(-((6651f-6641f) < (6652f-6642f))))) - "                \
  "(662b-661b)),0x90;663:;.pushsection .altinstructions,\"a\"; .long 661b "    \
  "- .; .long 6641f - .; .word ( 3*32+18); .byte 663b-661b; .byte "            \
  "6651f-6641f; .long 661b - .; .long 6642f - .; .word ( 1*32+27); .byte "     \
  "663b-661b; .byte 6652f-6642f;.popsection;.pushsection "                     \
  ".altinstr_replacement,\"ax\";# ALT: replacement 1;6641:;lfence; "           \
  "rdtsc;6651:;# ALT: replacement 2;6642:;rdtscp;6652:;.popsection"
#define STATIC_CPU_HAS_BRANCH                                                  \
  "# ALT: oldinstr2;661:;jmp 6f;662:;# ALT: padding2;.skip -((((6651f-6641f) " \
  "^ (((6651f-6641f) ^ (6652f-6642f)) & -(-((6651f-6641f) < "                  \
  "(6652f-6642f))))) - (662b-661b)) > 0) * (((6651f-6641f) ^ (((6651f-6641f) " \
  "^ (6652f-6642f)) & -(-((6651f-6641f) < (6652f-6642f))))) - "                \
  "(662b-661b)),0x90;663:;.pushsection .altinstructions,\"a\";.long 661b - "   \
  ".;.long 6641f - .;.word ( 3*32+21);.byte 663b-661b;.byte "                  \
  "6651f-6641f;.long 661b - .;.long 6642f - .;.word ${0:P};.byte "             \
  "663b-661b;.byte 6652f-6642f;.popsection;.pushsection "                      \
  ".altinstr_replacement,\"ax\";# ALT: replacement 1;6641:;jmp "               \
  "${4:l};6651:;# ALT: replacement 2;6642:;6652:;.popsection;.pushsection "    \
  ".altinstr_aux,\"ax\";6:;testb $1,${2:P} (% rip);jnz ${3:l};jmp "            \
  "${4:l};.popsection"
#define GET_USER_ASM_Q                                                         \
  "1:movq $1,$0;.pushsection \"__ex_table\",\"a\";.balign 4;.long (1b) - "     \
  ".;.long (${2:l}) - .;.long 3;.popsection"
#define GET_USER_ASM_L                                                         \
  "1:movl $1,$0;.pushsection \"__ex_table\",\"a\";.balign 4;.long (1b) - "     \
  ".;.long (${2:l}) - .;.long 3;.popsection"
#define GET_USER_ASM_W                                                         \
  "1:movw $1,$0;.pushsection \"__ex_table\",\"a\";.balign 4;.long (1b) - "     \
  ".;.long (${2:l}) - .;.long 3;.popsection"
#define GET_USER_ASM_B                                                         \
  "1:movb $1,$0;.pushsection \"__ex_table\",\"a\";.balign 4;.long (1b) - "     \
  ".;.long (${2:l}) - .;.long 3;.popsection"
#define CPU_VMX_OFF                                                            \
  "1: vmxoff;.pushsection \"__ex_table\",\"a\";.balign 4;.long (1b) - "        \
  ".;.long (${0:l}) - .;.long 1;.popsection"

#define NATIVE_SAVE_FL "# __raw_save_flags;pushf ; pop $0"

#define ATOMIC64_COUNTER_INDEX 0

namespace seahorn {
std::string splitAndJoin(std::string s, std::string delimiter,
                         std::string joiner) {
  SmallVector<std::string> parts;
  boost::split(parts, s, boost::is_any_of(delimiter), boost::token_compress_on);
  for (std::string &part : parts)
    boost::trim(part);
  return boost::join(parts, joiner);
}

std::string formatInlineAsm(std::string s) {
  boost::trim(s);
  boost::erase_all(s, "\t");
  s = splitAndJoin(s, "\n", ";");
  s = splitAndJoin(s, " ", " ");
  s = splitAndJoin(s, ",", ",");
  return s;
}

SmallVector<CallBrInst *>
getTargetAsmCallBrs(Module &M, const std::string &asmStr, bool isPrefix,
                    const std::string &constraints = "") {
  auto isTargetAsm = [&asmStr, isPrefix,
                      constraints](const CallBrInst *callbr) {
    const CallBase *call = dyn_cast<CallBase>(callbr);
    if (!call)
      return false;
    const InlineAsm *inlineAsm = dyn_cast<InlineAsm>(call->getCalledOperand());
    if (!inlineAsm)
      return false;
    std::string formatted = formatInlineAsm(inlineAsm->getAsmString());
    if (isPrefix)
      return !formatted.rfind(asmStr, 0) &&
             (constraints.empty() ||
              inlineAsm->getConstraintString() == constraints);
    else
      return formatted == asmStr &&
             (constraints.empty() ||
              inlineAsm->getConstraintString() == constraints);
  };

  SmallVector<CallBrInst *> calls;
  for (Function &F : M) {
    for (Instruction &inst : instructions(F)) {
      if (CallBrInst *call = dyn_cast<CallBrInst>(&inst)) {
        if (isTargetAsm(call))
          calls.push_back(call);
      }
    }
  }
  return calls;
}

SmallVector<CallInst *> getTargetAsmCalls(Module &m, const std::string &asmStr,
                                          bool isPrefix,
                                          const std::string &constraints = "") {
  auto isTargetAsm = [&asmStr, isPrefix, constraints](const CallInst *call) {
    const InlineAsm *inlineAsm = dyn_cast<InlineAsm>(call->getCalledOperand());
    if (!inlineAsm)
      return false;
    std::string formatted = formatInlineAsm(inlineAsm->getAsmString());
    if (isPrefix)
      return !formatted.rfind(asmStr, 0) &&
             (constraints.empty() ||
              inlineAsm->getConstraintString() == constraints);
    else
      return formatted == asmStr &&
             (constraints.empty() ||
              inlineAsm->getConstraintString() == constraints);
  };

  SmallVector<CallInst *> calls;
  for (Function &f : m) {
    for (Instruction &inst : instructions(f)) {
      if (CallInst *call = dyn_cast<CallInst>(&inst)) {
        if (isTargetAsm(call))
          calls.push_back(call);
      }
    }
  }
  return calls;
}

SmallVector<CallInst *> getTargetAsmCalls(Module &m,
                                          const std::set<std::string> &asmStr) {
  auto isTargetAsm = [&asmStr](const CallInst *call) {
    const InlineAsm *inlineAsm = dyn_cast<InlineAsm>(call->getCalledOperand());
    if (!inlineAsm)
      return false;
    std::string formatted = formatInlineAsm(inlineAsm->getAsmString());
    return !!asmStr.count(formatted);
  };

  SmallVector<CallInst *> calls;
  for (Function &f : m) {
    for (Instruction &inst : instructions(f)) {
      if (CallInst *call = dyn_cast<CallInst>(&inst)) {
        if (isTargetAsm(call))
          calls.push_back(call);
      }
    }
  }
  return calls;
}

class HandleInlineAsm : public ModulePass {
public:
  static char ID;

  HandleInlineAsm() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    handleBitTest(m);
    handleBitTestAndSet(m);
    handleBitTestAndReset(m);
    handleFFS(m);
    handleFLS(m);
    handleHWeight(m);

    // should be called before handleAddl, since they have identical prefix.
    handleAddWithCarry(m);
    // handleIncl(m);
    handleDecl(m);
    handleXAddq(m);
    handleXAddl(m);
    handleAddq(m);
    handleAddl(m);
    // handlemull(m);
    // handleDivl(m);
    handleAnd(m);
    handleOr(m);
    handleCpuid(m);
    handleIn(m);
    handleOut(m);
    handleMov(m);

    // handleAtomic64Read(m);
    // handleAtomic64Set(m);
    // handleAtomic64AddReturn(m);
    // handleAtomic64SubReturn(m);
    // handleAtomic64Xchg(m);
    handleXchg(m);
    handleCmpxchgl(m);
    handleCmpxchgq(m);
    // handleCmpxchg8b(m);

    handleNativeSaveFL(m);
    handleCLI(m);
    handleSTI(m);
    // handleRDPmC(m);

    // handleNativeReadmSRSafe(m);
    // handleNativeWritemSRSafe(m);
    // handleRDmSR(m);
    // handleWRmSR(m);
    handleRDTSC(m);
    // handleArrayIndexmaskNoSpec(m);

    handleCurrentTask(m);
    handleBarrier(m);
    removeFunctions(m);
    // handleSerialize(m);
    // handleIretToSelf(m);
    // handleDebugRegisters(m);
    // handleLoadGs(m);
    // handleBuildU64(m);
    // handleCallOnStack(m);
    // handleOptimizerHideVar(m);
    // handleGetUser(m);
    handleRandom(m);

    // handleFence(m);
    handleStaticCpuHas(m);
    handleGetUserAsm(m);
    handleCpuVmxOff(m);
    handleBinaryCallBr(m);
    return true;
  }

  virtual StringRef getPassName() const override { return "HandleInlineAsm"; }

private:
  DenseMap<const Type *, FunctionCallee> ndfn;

  void handleCurrentTask(Module &m) {
    LLVMContext &ctx = m.getContext();
    StructType *taskStructType =
        StructType::getTypeByName(ctx, "struct.task_struct");

    auto isCurrentTask = [taskStructType](const CallInst *call) {
      const Value *arg = call->getArgOperand(0);
      if (!equivTypes(arg->getType(),
                      taskStructType->getPointerTo()->getPointerTo())) {
        return false;
      }
      const GlobalVariable *gv =
          dyn_cast<GlobalVariable>(getUnderlyingObject(arg));
      if (!gv || !gv->getName().equals("pcpu_hot"))
        return false;
      // the only user should be inttoptr
      if (!call->hasNUses(1))
        return false;
      const User *user = *call->user_begin();
      return isa<IntToPtrInst>(user);
    };

    for (CallInst *call :
         getTargetAsmCalls(m, {MOVQ_1_0, MOVQ_GS_1_0, MOVQ_POSITION_INDEPENDENT,
                               MOVQ_GS_POSITION_INDEPENDENT})) {
      if (isCurrentTask(call)) {
        IRBuilder<> b(call);
        IntToPtrInst *intToPtr = cast<IntToPtrInst>(*call->user_begin());
        AllocaInst *alloca =
            b.CreateAlloca(intToPtr->getType()->getPointerElementType());
        intToPtr->replaceAllUsesWith(alloca);
        call->eraseFromParent();
        intToPtr->eraseFromParent();
      }
    }
  }

  void handleBarrier(Module &m) {
    for (CallInst *call : getTargetAsmCalls(m, "", false, BARRIER_CONSTRAINTS))
      call->eraseFromParent();
    for (CallInst *call :
         getTargetAsmCalls(m, "", false, BARRIER_DATA_CONSTRAINTS))
      call->eraseFromParent();
  }

  void handleIretToSelf(Module &M) {
    for (CallInst *call : getTargetAsmCalls(M, IRET_TO_SELF, false)) {
      IRBuilder<> b(call);
      // iret_to_self() pops back the stack pointer.
      Value *sp = call->getArgOperand(0);
      call->replaceAllUsesWith(sp);
      call->eraseFromParent();
    }
  }

  void handleDebugRegisters(Module &m) {
    for (CallInst *call : getTargetAsmCalls(m, SET_DEBUG_REGISTER_PREFIX, true))
      call->eraseFromParent();
  }

  void removeFunctions(Module &m) {
    SmallVector<CallInst *> calls =
        getTargetAsmCalls(m, {MB, RMB, WMB, UD2, INT3, SERIALIZE,
                              SET_DEBUG_REGISTER_PREFIX, NOP, LOAD_CR3, LIDT});
    for (CallInst *call : calls)
      call->eraseFromParent();
  }

  void handleLoadGs(Module &m) {
    SmallVector<CallInst *> calls = getTargetAsmCalls(m, LOAD_GS, false);
    Type *i16Ty = Type::getInt16Ty(m.getContext());
    FunctionCallee ndf = getNondetFn(i16Ty, m);
    for (CallInst *call : calls) {
      IRBuilder<> b(call);
      Value *v = b.CreateCall(ndf);
      call->replaceAllUsesWith(v);
      call->eraseFromParent();
    }
  }

  void handleBuildU64(Module &m) {
    Type *i64Ty = Type::getInt64Ty(m.getContext());
    for (CallInst *call :
         getTargetAsmCalls(m, "", false, BUILD_U64_CONSTRAINTS)) {
      IRBuilder<> b(call);
      Value *low = b.CreateZExt(call->getArgOperand(0), i64Ty);
      Value *upper = b.CreateZExt(call->getArgOperand(1), i64Ty);
      Value *v = b.CreateOr(low, b.CreateShl(upper, 32));
      call->replaceAllUsesWith(v);
      call->eraseFromParent();
    }
  }

  void handleBitTest(Module &m) {
    Type *i64Ty = Type::getInt64Ty(m.getContext());
    for (CallInst *call : getTargetAsmCalls(m, BIT_TEST_PREFIX, true)) {
      IRBuilder<> b(call);
      Value *base = call->getArgOperand(0);
      Value *offset = call->getArgOperand(1);

      Value *byteIdx = b.CreateLShr(offset, b.getInt64(5)); // divide by 32
      Value *bitIdx = b.CreateAnd(offset, b.getInt64(31));  // mod 32
      Value *byte = b.CreateGEP(i64Ty, base, byteIdx);
      Value *load = b.CreateLoad(i64Ty, byte);
      Value *mask = b.CreateShl(b.getInt64(1), bitIdx);
      Value *bit = b.CreateAnd(load, mask);
      Value *isSet = b.CreateICmpNE(bit, b.getInt64(0));
      Value *replace = b.CreateZExt(isSet, call->getType());
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  void handleBitTestAndSet(Module &m) {
    Type *i64Ty = Type::getInt64Ty(m.getContext());

    auto replace = [&m, i64Ty](const std::string &targetAsmPrefix) {
      SmallVector<CallInst *> calls =
          getTargetAsmCalls(m, targetAsmPrefix, true);
      for (CallInst *call : calls) {
        IRBuilder<> b(call);
        Value *base = call->getArgOperand(0);
        Value *offset = call->getArgOperand(1);

        Value *byteIdx = b.CreateLShr(offset, b.getInt64(5)); // divide by 32
        Value *bitIdx = b.CreateAnd(offset, b.getInt64(31));  // mod 32
        Value *byte = b.CreateGEP(i64Ty, base, byteIdx);
        Value *load = b.CreateLoad(i64Ty, byte);
        Value *mask = b.CreateShl(b.getInt64(1), bitIdx);
        Value *bit = b.CreateAnd(load, mask);
        Value *isSet = b.CreateICmpNE(bit, b.getInt64(0));
        b.CreateStore(b.CreateOr(load, mask), byte);
        if (!call->getType()->isVoidTy()) {
          Value *replace = b.CreateZExt(isSet, call->getType());
          call->replaceAllUsesWith(replace);
        }
        call->eraseFromParent();
      }
    };

    replace(BIT_TEST_AND_SET_1_0_PREFIX);
    replace(BIT_TEST_AND_SET_2_0_PREFIX);
    replace(BIT_TEST_AND_SET_2_1_PREFIX);
  }

  void handleBitTestAndReset(Module &m) {
    Type *i64Ty = Type::getInt64Ty(m.getContext());
    auto replaceBtrl = [&m, i64Ty](const std::string &targetAsmPrefix,
                                   unsigned addrIdx, unsigned offIdx) {
      SmallVector<CallInst *> calls =
          getTargetAsmCalls(m, targetAsmPrefix, true);
      for (CallInst *call : calls) {
        IRBuilder<> b(call);
        Value *base = call->getArgOperand(0);
        Value *offset = call->getArgOperand(1);

        Value *byteIdx = b.CreateLShr(offset, b.getInt64(5)); // divide by 32
        Value *bitIdx = b.CreateAnd(offset, b.getInt64(31));  // mod 32
        Value *byte = b.CreateGEP(i64Ty, base, byteIdx);
        Value *load = b.CreateLoad(i64Ty, byte);
        Value *mask = b.CreateShl(b.getInt64(1), bitIdx);
        Value *bit = b.CreateAnd(load, mask);
        Value *isSet = b.CreateICmpNE(bit, b.getInt64(0));
        Value *flipped = b.CreateXor(mask, b.getInt64(0xffffffff));
        b.CreateStore(b.CreateAnd(load, flipped), byte);
        if (!call->getType()->isVoidTy()) {
          Value *replace = b.CreateZExt(isSet, call->getType());
          call->replaceAllUsesWith(replace);
        }
        call->eraseFromParent();
      }
    };

    replaceBtrl(BIT_TEST_AND_RESET_1_0_PREFIX, 1, 0);
    replaceBtrl(BIT_TEST_AND_RESET_2_0_PREFIX, 2, 0);
    replaceBtrl(BIT_TEST_AND_RESET_2_1_PREFIX, 2, 1);
  }

  void handleIncl(Module &m) {
    Type *i32Ty = Type::getInt32Ty(m.getContext());
    for (CallInst *call : getTargetAsmCalls(m, INCL, false)) {
      IRBuilder<> b(call);
      Value *val = call->getArgOperand(0);
      if (val->getType()->isPointerTy()) {
        Value *load = b.CreateLoad(i32Ty, val);
        Value *inc = b.CreateAdd(load, b.getInt32(1));
        b.CreateStore(inc, val);
        call->replaceAllUsesWith(inc);
      } else {
        errs() << "TODO: handleIncl\n";
      }
      call->eraseFromParent();
    }
  }

  void handleAddWithCarry(Module &m) {
    SmallVector<CallInst *> calls = getTargetAsmCalls(m, ADD_WITH_CARRY, false);
    LLVMContext &ctx = m.getContext();
    for (CallInst *call : calls) {
      IRBuilder<> b(call);
      Value *v1 = call->getArgOperand(0);
      Value *v2 = call->getArgOperand(1);
      Function *uaddWithOverflow = Intrinsic::getDeclaration(
          &m, Intrinsic::uadd_with_overflow, {Type::getInt32Ty(ctx)});
      CallInst *newCall = b.CreateCall(uaddWithOverflow, {v1, v2});
      Value *sum = b.CreateExtractValue(newCall, 0);
      call->replaceAllUsesWith(sum);
      call->eraseFromParent();
    }
  }

  void handleDecl(Module &m) {
    SmallVector<CallInst *> calls = getTargetAsmCalls(m, DECL_PREFIX, true);
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt32Ty(ctx);
    Type *i8Ty = Type::getInt8Ty(ctx);
    for (CallInst *call : calls) {
      IRBuilder<> b(call);
      Value *val = call->getArgOperand(0);
      if (val->getType()->isPointerTy()) {
        Value *load = b.CreateLoad(i32Ty, val);
        Value *isZero = b.CreateICmpEQ(load, b.getInt32(0));
        Value *isZeroExt = b.CreateZExt(isZero, i8Ty);
        Value *dec = b.CreateSub(load, b.getInt32(1));
        b.CreateStore(dec, val);
        call->replaceAllUsesWith(isZeroExt);
      } else {
        errs() << "TODO: handleDecl\n";
      }
      call->eraseFromParent();
    }
  }

  void handleXAddq(Module &m) {
    SmallVector<CallInst *> calls = getTargetAsmCalls(m, XADDQ, false);
    Type *i64Ty = Type::getInt64Ty(m.getContext());
    for (CallInst *call : calls) {
      IRBuilder<> b(call);
      Value *src = call->getArgOperand(0);
      Value *load = b.CreateLoad(i64Ty, src);
      Value *add = b.CreateAdd(load, call->getArgOperand(1));
      b.CreateStore(add, src);
      call->replaceAllUsesWith(load);
      call->eraseFromParent();
    }
  }

  void handleXAddl(Module &m) {
    SmallVector<CallInst *> calls = getTargetAsmCalls(m, XADDL, false);
    Type *i32Ty = Type::getInt32Ty(m.getContext());
    for (CallInst *call : calls) {
      IRBuilder<> b(call);
      Value *src = call->getArgOperand(0);
      Value *load = b.CreateLoad(i32Ty, src);
      Value *add = b.CreateAdd(load, call->getArgOperand(1));
      b.CreateStore(add, src);
      call->replaceAllUsesWith(load);
      call->eraseFromParent();
    }
  }

  void handleAddq(Module &m) {
    auto replace = [&m](const std::string &targetAsm, bool isPrefix) {
      SmallVector<CallInst *> calls = getTargetAsmCalls(m, targetAsm, isPrefix);
      LLVMContext &ctx = m.getContext();
      for (CallInst *call : calls) {
        IRBuilder<> B(call);
        Value *dst = call->getArgOperand(0);
        Value *src = call->getArgOperand(1);
        Type *dstType = dst->getType();
        Value *load = B.CreateLoad(dstType->getPointerElementType(), dst);
        Value *add = B.CreateAdd(load, src);
        B.CreateStore(add, dst);
        Value *isNeg = B.CreateZExt(B.CreateICmpSLT(add, B.getInt64(0)),
                                    Type::getInt8Ty(ctx));
        call->replaceAllUsesWith(isNeg);
        call->eraseFromParent();
      }
    };

    replace(ADDQ_1_0, false);
    replace(ADDQ_2_0_PREFIX, true);
  }

  void handleAddl(Module &m) {
    auto replace = [&m](const std::string &targetAsm, bool isPrefix) {
      SmallVector<CallInst *> calls = getTargetAsmCalls(m, targetAsm, isPrefix);
      LLVMContext &ctx = m.getContext();
      for (CallInst *call : calls) {
        IRBuilder<> b(call);
        Value *dst = call->getArgOperand(0);
        Value *src = call->getArgOperand(1);
        Type *dstType = dst->getType();
        Value *load = b.CreateLoad(dstType->getPointerElementType(), dst);
        Value *add = b.CreateAdd(load, src);
        b.CreateStore(add, dst);
        Value *isNeg = b.CreateZExt(b.CreateICmpSLT(add, b.getInt32(0)),
                                    Type::getInt8Ty(ctx));
        call->replaceAllUsesWith(isNeg);
        call->eraseFromParent();
      }
    };

    replace(ADDL_1_0, false);
    replace(ADDL_2_0_PREFIX, true);
  }

  void handleMull(Module &m) {
    Type *i32Ty = Type::getInt32Ty(m.getContext());
    for (CallInst *call : getTargetAsmCalls(m, MULL, false)) {
      IRBuilder<> b(call);
      Value *v1 = b.CreateZExt(call->getArgOperand(0), b.getInt64Ty());
      Value *v2 = b.CreateZExt(call->getArgOperand(1), b.getInt64Ty());

      Value *mul = b.CreateMul(v1, v2);
      Value *low = b.CreateTrunc(mul, i32Ty);
      Value *upper = b.CreateTrunc(b.CreateLShr(mul, 32), i32Ty);
      StructType *type = cast<StructType>(call->getType());
      Value *empty = UndefValue::get(type);
      Value *setLow = b.CreateInsertValue(empty, low, {0});
      Value *replace = b.CreateInsertValue(setLow, upper, {1});
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  void handleDivl(Module &m) {
    SmallVector<CallInst *> calls = getTargetAsmCalls(m, DIVL, false);
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt32Ty(ctx);
    Type *i64Ty = Type::getInt64Ty(ctx);
    for (CallInst *call : calls) {
      IRBuilder<> b(call);
      Value *divisor = b.CreateZExt(call->getArgOperand(0), i64Ty);
      Value *low = b.CreateZExt(call->getArgOperand(1), i64Ty);
      Value *upper = b.CreateZExt(call->getArgOperand(2), i64Ty);

      Value *v = b.CreateOr(b.CreateShl(upper, 32), low);
      Value *quotient = b.CreateTrunc(b.CreateUDiv(v, divisor), i32Ty);
      Value *remainder = b.CreateTrunc(b.CreateURem(v, divisor), i32Ty);

      StructType *type = cast<StructType>(call->getType());
      Value *empty = UndefValue::get(type);
      Value *setQuotient = b.CreateInsertValue(empty, quotient, {0});
      Value *replace = b.CreateInsertValue(setQuotient, remainder, {1});
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  void handleAnd(Module &m) {
    auto replace = [&m](const std::string &targetAsm, bool isPrefix) {
      SmallVector<CallInst *> calls = getTargetAsmCalls(m, targetAsm, isPrefix);
      for (CallInst *call : calls) {
        IRBuilder<> b(call);
        Value *dst = call->getArgOperand(0);
        Value *src = call->getArgOperand(1);
        Type *dstType = dst->getType();
        if (dstType->isPointerTy()) {
          Value *load = b.CreateLoad(dstType->getPointerElementType(), dst);
          if (load->getType() != src->getType()) {
            src = b.CreateIntCast(src, load->getType(), true);
          }
          Value *and_ = b.CreateAnd(load, src);
          b.CreateStore(and_, dst);

          call->replaceAllUsesWith(and_);
          call->eraseFromParent();
        } else {
          errs() << "TODO: handleAndl\n";
        }
      }
    };

    replace(ANDB_1_0, false);
    replace(ANDB_2_1_PREFIX, true);
    replace(ANDL, false);
    replace(ANDQ, false);
  }

  void handleOr(Module &m) {
    auto replace = [&m](const std::string &targetAsm) {
      for (CallInst *call : getTargetAsmCalls(m, targetAsm, false)) {
        IRBuilder<> b(call);
        Value *dst = call->getArgOperand(0);
        Value *src = call->getArgOperand(1);
        Type *dstType = dst->getType();

        if (dstType->isPointerTy()) {
          if (src->getType()->isIntegerTy()) {
            // LHS value does not exists.
            Type *innerType = dstType->getPointerElementType();
            Value *cast = b.CreateIntCast(src, innerType, true);
            Value *loaded = b.CreateLoad(innerType, dst);
            Value *or_ = b.CreateOr(loaded, cast);
            b.CreateStore(or_, dst);
          } else {
            Value *load = b.CreateLoad(dstType->getPointerElementType(), dst);
            Value *or_ = b.CreateOr(load, src);
            b.CreateStore(or_, dst);
            call->replaceAllUsesWith(or_);
          }
        } else {
          errs() << "TODO: handleOr\n";
        }
        call->eraseFromParent();
      }
    };

    replace(ORB);
    replace(ORL);
    replace(ORQ);
  }

  void handleCpuid(Module &m) {
    for (CallInst *call : getTargetAsmCalls(m, CPUID, false)) {
      FunctionCallee ndf = getNondetFn(call->getType(), m);
      IRBuilder<> b(call);
      Value *ndval = b.CreateCall(ndf);
      call->replaceAllUsesWith(ndval);
      call->eraseFromParent();
    }
  }

  void handleIn(Module &m) {
    auto replace = [this, &m](const std::string &targetAsm, Type *type) {
      FunctionCallee ndf = getNondetFn(type, m);
      for (CallInst *call : getTargetAsmCalls(m, targetAsm, false)) {
        IRBuilder<> b(call);
        Value *replace = b.CreateCall(ndf);
        call->replaceAllUsesWith(replace);
        call->eraseFromParent();
      }
    };

    LLVMContext &ctx = m.getContext();
    replace(INB, Type::getInt8Ty(ctx));
    replace(INW, Type::getInt16Ty(ctx));
    replace(INL, Type::getInt32Ty(ctx));
  }

  void handleOut(Module &m) {
    for (CallInst *call :
         getTargetAsmCalls(m, {OUTB, OUTW, OUTL, OUT_AL_0x80, OUT_AL_0xed}))
      call->eraseFromParent();
  }

  void handleMov(Module &m) {
    LLVMContext &ctx = m.getContext();
    Type *i8Ty = Type::getInt8Ty(ctx);
    Type *i32Ty = Type::getInt32Ty(ctx);
    Type *i64Ty = Type::getInt64Ty(ctx);

    auto replaceMov = [&m](const std::string &targetAsm, Type *intTy,
                           unsigned srcIdx, unsigned dstIdx) {
      SmallVector<CallInst *> calls = getTargetAsmCalls(m, targetAsm, false);
      for (CallInst *call : calls) {
        IRBuilder<> b(call);
        Value *replace;
        if (call->getType()->isVoidTy()) {
          Value *src = call->getArgOperand(srcIdx);
          Value *dst = call->getArgOperand(dstIdx);
          Type *srcType = src->getType();
          Type *dstType = dst->getType();
          if (srcType->isPointerTy()) {
            LoadInst *load =
                b.CreateLoad(srcType->getPointerElementType(), src);
            b.CreateStore(load, dst);
            replace = load;
          } else {
            Type *innerType = dstType->getPointerElementType();
            if (innerType->isPointerTy() && srcType->isIntegerTy()) {
              src = b.CreateIntToPtr(src, innerType);
            } else if (srcType != innerType) {
              errs() << "TODO: type mismatch in handleMov\n";
            }
            b.CreateStore(src, dst);
            replace = src;
          }
        } else {
          Value *src = call->getArgOperand(0);
          Type *srcType = src->getType();
          if (srcType->isPointerTy()) {
            if (srcType->getPointerElementType()->isFunctionTy()) {
              Function *f = m.getFunction(src->getName());
              replace = b.CreateCall(f);
            } else {
              replace = b.CreateLoad(srcType->getPointerElementType(), src);
            }
          } else {
            replace = src;
          }
        }
        if (replace->getType()->isPointerTy()) {
          replace = b.CreatePtrToInt(replace, intTy);
        }
        call->replaceAllUsesWith(replace);
        call->eraseFromParent();
      }
    };

    replaceMov(MOVB_0_1, i8Ty, 0, 1);
    replaceMov(MOVB_1_0, i8Ty, 1, 0);
    replaceMov(MOVW_0_1, i32Ty, 0, 1);
    replaceMov(MOVW_1_0, i32Ty, 1, 0);
    replaceMov(MOVL_0_1, i32Ty, 0, 1);
    replaceMov(MOVL_1_0, i32Ty, 1, 0);
    replaceMov(MOVQ_0_1, i64Ty, 0, 1);
    replaceMov(MOVQ_1_0, i64Ty, 1, 0);
  }

  void handleXchg(Module &m) {
    auto replace = [&m, this](const std::string &targetAsm) {
      SmallVector<CallInst *> calls = getTargetAsmCalls(m, targetAsm, false);
      for (CallInst *call : calls) {
        Value *src = call->getArgOperand(0);
        Value *dst = call->getArgOperand(1);
        IRBuilder<> b(call);
        if (src->getType() != dst->getType()->getPointerTo()) {
          errs() << "TODO: handleXchgl\n";
          FunctionCallee ndf = getNondetFn(call->getType(), m);
          Value *ndval = b.CreateCall(ndf);
          call->replaceAllUsesWith(ndval);
          call->eraseFromParent();
          continue;
        }
        Value *loaded = b.CreateLoad(dst->getType(), src);
        b.CreateStore(dst, src);
        call->replaceAllUsesWith(loaded);
        call->eraseFromParent();
      }
    };
    replace(XCHGL);
    replace(XCHGQ);
  }

  void handleCmpxchgl(Module &m) {
    LLVMContext &ctx = m.getContext();
    Type *i8Ty = Type::getInt8Ty(ctx);
    for (CallInst *call : getTargetAsmCalls(m, CMPXCHGL31_PREFIX, true,
                                            CMPXCHGL31_CONSTRAINTS)) {
      IRBuilder<> b(call);
      Value *acc = call->getArgOperand(0);
      Value *cmp = call->getArgOperand(3);
      Value *new_ = call->getArgOperand(1);
      AtomicCmpXchgInst *inst = b.CreateAtomicCmpXchg(
          acc, cmp, new_, MaybeAlign(), AtomicOrdering::SequentiallyConsistent,
          AtomicOrdering::SequentiallyConsistent);

      // convert {ty, i1} to {i8, ty}
      Value *val = b.CreateExtractValue(inst, {0});
      Type *type = call->getType();
      Value *empty = UndefValue::get(type);
      Value *isSuccess = b.CreateExtractValue(inst, {1});
      Value *castedSuccess = b.CreateZExt(isSuccess, i8Ty);
      Value *converted = b.CreateInsertValue(empty, castedSuccess, {0});
      Value *replace = b.CreateInsertValue(converted, val, {1});

      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }

    Type *i32Ty = Type::getInt32Ty(ctx);
    for (CallInst *call :
         getTargetAsmCalls(m, CMPXCHGL21, false, CMPXCHGL21_CONSTRAINTS)) {
      IRBuilder<> b(call);
      Value *acc = call->getArgOperand(0);
      Value *cmp = call->getArgOperand(2);
      Value *new_ = call->getArgOperand(1);
      Type *type = call->getType();
      if (type != i32Ty) {
        acc = b.CreateBitCast(acc, type->getPointerTo());
      }
      AtomicCmpXchgInst *inst = b.CreateAtomicCmpXchg(
          acc, cmp, new_, MaybeAlign(), AtomicOrdering::SequentiallyConsistent,
          AtomicOrdering::SequentiallyConsistent);

      Value *replace = b.CreateExtractValue(inst, {0});
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  void handleCmpxchgq(Module &m) {
    LLVMContext &ctx = m.getContext();
    Type *i8Ty = Type::getInt8Ty(ctx);
    for (CallInst *call : getTargetAsmCalls(m, CMPXCHGQ31_PREFIX, true,
                                            CMPXCHGQ31_CONSTRAINTS)) {
      IRBuilder<> b(call);
      Value *acc = call->getArgOperand(0);
      Value *cmp = call->getArgOperand(3);
      Value *new_ = call->getArgOperand(1);
      AtomicCmpXchgInst *inst = b.CreateAtomicCmpXchg(
          acc, cmp, new_, MaybeAlign(), AtomicOrdering::SequentiallyConsistent,
          AtomicOrdering::SequentiallyConsistent);

      // convert {ty, i1} to {i8, ty}
      Value *val = b.CreateExtractValue(inst, {0});
      Type *type = call->getType();
      Value *empty = UndefValue::get(type);
      Value *isSuccess = b.CreateExtractValue(inst, {1});
      Value *castedSuccess = b.CreateZExt(isSuccess, i8Ty);
      Value *converted = b.CreateInsertValue(empty, castedSuccess, {0});
      Value *replace = b.CreateInsertValue(converted, val, {1});

      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }

    Type *i64Ty = Type::getInt64Ty(ctx);
    for (CallInst *call :
         getTargetAsmCalls(m, CMPXCHGQ21, false, CMPXCHGQ21_CONSTRAINTS)) {
      IRBuilder<> b(call);
      Value *acc = call->getArgOperand(0);
      Value *cmp = call->getArgOperand(2);
      Value *new_ = call->getArgOperand(1);
      Type *type = call->getType();
      if (type != i64Ty) {
        acc = b.CreateBitCast(acc, type->getPointerTo());
      }
      AtomicCmpXchgInst *inst = b.CreateAtomicCmpXchg(
          acc, cmp, new_, MaybeAlign(), AtomicOrdering::SequentiallyConsistent,
          AtomicOrdering::SequentiallyConsistent);

      Value *replace = b.CreateExtractValue(inst, {0});
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  void handleCmpxchg8b(Module &m) {
    for (CallInst *call : getTargetAsmCalls(m, CMPXCHG8B, false)) {
      IRBuilder<> b(call);
      Value *val = call->getArgOperand(0);
      Value *lower = call->getArgOperand(1);
      Value *upper = call->getArgOperand(2);
      Value *prev = call->getArgOperand(3);

      Value *shiftedUpper = b.CreateShl(upper, 32);
      Value *new_ = b.CreateOr(shiftedUpper, lower);
      Value *atomic = b.CreateAtomicCmpXchg(
          val, prev, new_, MaybeAlign(), AtomicOrdering::SequentiallyConsistent,
          AtomicOrdering::SequentiallyConsistent);
      Value *replace = b.CreateExtractValue(atomic, {0});
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  void handleFFS(Module &m) {
    LLVMContext &ctx = m.getContext();
    Function *cttz =
        Intrinsic::getDeclaration(&m, Intrinsic::cttz, {Type::getInt64Ty(ctx)});
    for (CallInst *call : getTargetAsmCalls(m, FFS, false)) {
      IRBuilder<> B(call);
      Value *v = call->getArgOperand(0);
      Value *zero = B.getInt64(0);
      Value *isZero = B.CreateICmpEQ(v, zero);
      Value *cttzCall = B.CreateCall(cttz, {v, B.getFalse()});
      Value *nonZero = B.CreateAdd(cttzCall, B.getInt64(1));
      Value *replace = B.CreateSelect(isZero, zero, nonZero);
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  void handleFLS(Module &m) {
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt32Ty(m.getContext());
    auto replace = [i32Ty, &ctx, &m](const std::string &targetAsm,
                                     unsigned bitWidth) {
      IntegerType *ty = IntegerType::get(ctx, bitWidth);
      Function *ctlz = Intrinsic::getDeclaration(&m, Intrinsic::ctlz, {ty});
      for (CallInst *call : getTargetAsmCalls(m, targetAsm, false)) {
        IRBuilder<> b(call);
        Value *v = call->getArgOperand(0);
        Value *zero = ConstantInt::get(ty, 0);
        Value *isZero = b.CreateICmpEQ(v, zero);
        Value *ctlzCall = b.CreateCall(ctlz, {v, b.getFalse()});
        Value *nonZero = b.CreateSub(ConstantInt::get(ty, bitWidth), ctlzCall);
        Value *replace = b.CreateSelect(isZero, zero, nonZero);
        if (replace->getType() != i32Ty) {
          replace = b.CreateTrunc(replace, i32Ty);
        }
        call->replaceAllUsesWith(replace);
        call->eraseFromParent();
      }
    };

    replace(FLSL, 32);
    replace(FLSQ, 64);
  }

  void handleHWeight(Module &m) {
    LLVMContext &ctx = m.getContext();
    Function *ctpop = Intrinsic::getDeclaration(&m, Intrinsic::ctpop,
                                                {Type::getInt32Ty(ctx)});
    for (CallInst *call : getTargetAsmCalls(m, HWEIGHT, false)) {
      IRBuilder<> b(call);
      Value *v = call->getArgOperand(0);
      Value *ctpopCall = b.CreateCall(ctpop, v);
      call->replaceAllUsesWith(ctpopCall);
      call->eraseFromParent();
    }
  }

  void handleNativeReadMSRSafe(Module &m) {
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt8Ty(ctx);
    Type *i64Ty = Type::getInt8Ty(ctx);
    for (CallInst *call : getTargetAsmCalls(m, NATIVE_READ_MSR_SAFE, false)) {
      IRBuilder<> b(call);
      StructType *type = cast<StructType>(call->getType());
      // return {0 (success), 0 (msr value)} for now.
      Value *empty = UndefValue::get(type);
      Value *setSuccess =
          b.CreateInsertValue(empty, Constant::getNullValue(i32Ty), {0});
      Value *retVal =
          b.CreateInsertValue(setSuccess, Constant::getNullValue(i64Ty), {1});
      call->replaceAllUsesWith(retVal);
      call->eraseFromParent();
    }
  }

  void handleNativeWriteMSRSafe(Module &m) {
    for (CallInst *call : getTargetAsmCalls(m, NATIVE_WRITE_MSR_SAFE, false)) {
      IRBuilder<> b(call);
      // return 0 (success) for now.
      Value *zero = b.getInt32(0);
      call->replaceAllUsesWith(zero);
      call->eraseFromParent();
    }
  }

  void handleRDMSR(Module &m) {
    Type *i64Ty = Type::getInt64Ty(m.getContext());
    FunctionCallee ndf = getNondetFn(i64Ty, m);
    for (CallInst *call : getTargetAsmCalls(m, RDMSR, false)) {
      IRBuilder<> b(call);
      // return a nondet unsigned long long for now.
      Value *ret = b.CreateCall(ndf);
      call->replaceAllUsesWith(ret);
      call->eraseFromParent();
    }
  }

  void handleWRMSR(Module &m) {
    for (CallInst *call : getTargetAsmCalls(m, WRMSR, false)) {
      IRBuilder<> b(call);
      // return 0 (success) for now.
      Value *zero = b.getInt32(0);
      call->replaceAllUsesWith(zero);
      call->eraseFromParent();
    }
  }

  void handleRDTSC(Module &m) {
    auto replace = [this, &m](const std::string &targetAsm) {
      for (CallInst *call : getTargetAsmCalls(m, targetAsm, false)) {
        FunctionCallee ndf = getNondetFn(call->getType(), m);
        // return a nondet unsigned long long for now.
        CallInst *r = CallInst::Create(ndf, "", call);
        call->replaceAllUsesWith(r);
        call->eraseFromParent();
      }
    };

    replace(RDTSC);
    replace(RDTSC_ORDERED);
  }

  void handleAtomic64Read(Module &m) {
    LLVMContext &ctx = m.getContext();
    StructType *atomic64Type =
        StructType::getTypeByName(ctx, "struct.atomic64_t");
    Type *i64Ty = Type::getInt64Ty(ctx);
    for (CallInst *call : getTargetAsmCalls(m, CALL1, false)) {
      if (!call->getNumOperands())
        continue;
      Value *op = call->getOperand(0);
      if (!op->hasName() || !op->getName().equals("atomic64_read_cx8"))
        continue;
      Value *v = call->getOperand(1);
      IRBuilder<> b(call);
      Value *counterPtr =
          b.CreateStructGEP(atomic64Type, v, ATOMIC64_COUNTER_INDEX);
      Value *counter = b.CreateLoad(i64Ty, counterPtr);
      call->replaceAllUsesWith(counter);
      call->eraseFromParent();
    }
  }

  void handleAtomic64Set(Module &m) {
    LLVMContext &ctx = m.getContext();
    StructType *atomic64Type =
        StructType::getTypeByName(ctx, "struct.atomic64_t");
    Type *i64Ty = Type::getInt64Ty(ctx);
    for (CallInst *call : getTargetAsmCalls(m, CALL0, false)) {
      if (!call->getNumOperands())
        continue;
      Value *op = call->getOperand(0);
      if (!op->hasName() || !op->getName().equals("atomic64_set_cx8"))
        continue;
      Value *v = call->getOperand(1);
      Value *i = call->getOperand(2);
      IRBuilder<> b(call);
      Value *counterPtr =
          b.CreateStructGEP(atomic64Type, v, ATOMIC64_COUNTER_INDEX);
      if (!i->getType()->isIntegerTy(64)) {
        i = b.CreateZExt(i, i64Ty);
      }
      Value *set = b.CreateStore(i, counterPtr);
      call->replaceAllUsesWith(set);
      call->eraseFromParent();
    }
  }

  void handleAtomic64AddReturn(Module &m) {
    LLVMContext &ctx = m.getContext();
    StructType *atomic64Type =
        StructType::getTypeByName(ctx, "struct.atomic64_t");
    Type *i64Ty = Type::getInt64Ty(ctx);
    for (CallInst *call : getTargetAsmCalls(m, CALL2, false)) {
      if (!call->getNumOperands())
        continue;
      Value *op = call->getOperand(0);
      if (!op->hasName() || !op->getName().equals("atomic64_add_return_cx8"))
        continue;
      Value *i = call->getOperand(2);
      Value *v = call->getOperand(3);
      IRBuilder<> b(call);
      Value *counterPtr =
          b.CreateStructGEP(atomic64Type, v, ATOMIC64_COUNTER_INDEX);
      Value *counter = b.CreateLoad(i64Ty, counterPtr);
      Value *add = b.CreateAdd(counter, i);
      b.CreateStore(add, counterPtr);

      Value *empty = UndefValue::get(call->getType());
      Value *setResult = b.CreateInsertValue(empty, add, {0});
      Value *setCounter = b.CreateInsertValue(setResult, v, {1});
      call->replaceAllUsesWith(setCounter);
      call->eraseFromParent();
    }
  }

  void handleAtomic64SubReturn(Module &m) {
    LLVMContext &ctx = m.getContext();
    StructType *atomic64Type =
        StructType::getTypeByName(ctx, "struct.atomic64_t");
    Type *i64Ty = Type::getInt64Ty(ctx);
    for (CallInst *call : getTargetAsmCalls(m, CALL2, false)) {
      if (!call->getNumOperands())
        continue;
      Value *op = call->getOperand(0);
      if (!op->hasName() || !op->getName().equals("atomic64_sub_return_cx8"))
        continue;
      Value *i = call->getOperand(2);
      Value *v = call->getOperand(3);
      IRBuilder<> b(call);
      Value *counterPtr =
          b.CreateStructGEP(atomic64Type, v, ATOMIC64_COUNTER_INDEX);
      Value *counter = b.CreateLoad(i64Ty, counterPtr);
      Value *sub = b.CreateSub(counter, i);
      b.CreateStore(sub, counterPtr);

      Value *empty = UndefValue::get(call->getType());
      Value *setResult = b.CreateInsertValue(empty, sub, {0});
      Value *setCounter = b.CreateInsertValue(setResult, v, {1});
      call->replaceAllUsesWith(setCounter);
      call->eraseFromParent();
    }
  }

  void handleAtomic64Xchg(Module &m) {
    LLVMContext &ctx = m.getContext();
    Type *i64Ty = Type::getInt64Ty(ctx);
    StructType *atomic64Type =
        StructType::getTypeByName(ctx, "struct.atomic64_t");
    for (CallInst *call :
         getTargetAsmCalls(m, CALL1, false, ARCH_ATOMIC64_XCHG_CONSTRAINTS)) {
      if (!call->getNumOperands())
        continue;
      Value *op = call->getOperand(0);
      if (!op->hasName() || !op->getName().equals("atomic64_xchg_cx8"))
        continue;
      IRBuilder<> b(call);
      Value *v = call->getOperand(1);
      Value *low = b.CreateZExt(call->getOperand(2), i64Ty);
      Value *high = b.CreateZExt(call->getOperand(3), i64Ty);
      Value *new_ = b.CreateOr(b.CreateLShr(high, 32), low);
      Value *counterPtr =
          b.CreateStructGEP(atomic64Type, v, ATOMIC64_COUNTER_INDEX);
      Value *old =
          b.CreateAtomicRMW(AtomicRMWInst::Xchg, counterPtr, new_, MaybeAlign(),
                            AtomicOrdering::SequentiallyConsistent);
      call->replaceAllUsesWith(old);
      call->eraseFromParent();
    }
  }

  void handleNativeSaveFL(Module &m) {
    Type *i32Ty = Type::getInt32Ty(m.getContext());
    FunctionCallee ndf = getNondetFn(i32Ty, m);
    for (CallInst *call : getTargetAsmCalls(m, NATIVE_SAVE_FL, false)) {
      IRBuilder<> b(call);
      // return a nondet unsigned long for now.
      Value *ret = b.CreateCall(ndf);
      call->replaceAllUsesWith(ret);
      call->eraseFromParent();
    }
  }

  void handleCLI(Module &m) {
    // simply ignore the CLI instruction.
    for (CallInst *call : getTargetAsmCalls(m, CLI, false))
      call->eraseFromParent();
  }

  void handleSTI(Module &m) {
    // simply ignore the STI instruction.
    for (CallInst *call : getTargetAsmCalls(m, STI, false))
      call->eraseFromParent();
  }

  void handleRDPMC(Module &m) {
    Type *i64Ty = Type::getInt64Ty(m.getContext());
    for (CallInst *call : getTargetAsmCalls(m, RDPMC, false)) {
      IRBuilder<> b(call);
      // return a nondet unsigned long long for now.
      FunctionCallee ndf = getNondetFn(i64Ty, m);
      Value *ret = b.CreateCall(ndf);
      call->replaceAllUsesWith(ret);
      call->eraseFromParent();
    }
  }

  void handleArrayIndexMaskNoSpec(Module &m) {
    for (CallInst *call :
         getTargetAsmCalls(m, ARRAY_INDEX_MASK_NOSPEC, false)) {
      IRBuilder<> b(call);
      Value *index = call->getArgOperand(1);
      Value *size = call->getArgOperand(0);
      Value *isOk = b.CreateICmpULT(index, size);
      Value *mask = b.CreateSelect(isOk, b.getInt32(0xffffffff), b.getInt32(0));
      call->replaceAllUsesWith(mask);
      call->eraseFromParent();
    }
  }

  void handleFence(Module &m) {
    // simply ignore the FENCE instruction.
    for (CallInst *call : getTargetAsmCalls(m, LFENCE, false)) {
      call->eraseFromParent();
    }
    for (CallInst *call : getTargetAsmCalls(m, SFENCE, false)) {
      call->eraseFromParent();
    }
    for (CallInst *call : getTargetAsmCalls(m, MFENCE, false)) {
      call->eraseFromParent();
    }
  }

  void handleStaticCpuHas(Module &m) {
    for (CallBrInst *callbr :
         getTargetAsmCallBrs(m, STATIC_CPU_HAS_BRANCH, false)) {
      // TODO: Randomizing the destination should be better.
      BasicBlock *noBlk = callbr->getIndirectDest(1);
      BranchInst *branch = BranchInst::Create(noBlk, callbr);
      callbr->replaceAllUsesWith(branch);
      for (unsigned i = 0; i < callbr->getNumSuccessors(); i++) {
        BasicBlock *succ = callbr->getSuccessor(i);
        if (succ != noBlk)
          succ->removePredecessor(callbr->getParent());
      }
      callbr->dropAllReferences();
      callbr->eraseFromParent();
    }
  }

  void handleGetUserAsm(Module &m) {
    auto replace = [&m](const std::string &targetAsm,
                        const std::string &utilSuffix) {
      Type *i8PtrType = Type::getInt8Ty(m.getContext())->getPointerTo();
      for (CallBrInst *callbr : getTargetAsmCallBrs(m, targetAsm, false)) {
        IRBuilder<> b(callbr);
        Value *largeStruct = callbr->getArgOperand(0);
        Value *bytes =
            b.CreateGEP(largeStruct->getType(), largeStruct, b.getInt32(0));
        if (bytes->getType() != i8PtrType) {
          bytes = b.CreateBitCast(bytes, i8PtrType);
        }
        Function *f = m.getFunction("__DRVHORN_util_read_" + utilSuffix);
        CallInst *val = b.CreateCall(f, bytes);
        // TODO: Randomizing the destination should be better.
        b.CreateBr(callbr->getDefaultDest());
        for (unsigned i = 0; i < callbr->getNumSuccessors(); i++) {
          BasicBlock *succ = callbr->getSuccessor(i);
          if (succ != callbr->getDefaultDest())
            succ->removePredecessor(callbr->getParent());
        }
        callbr->replaceAllUsesWith(val);
        callbr->eraseFromParent();
      }
    };
    replace(GET_USER_ASM_Q, "u64");
    replace(GET_USER_ASM_L, "u32");
    replace(GET_USER_ASM_W, "u16");
    replace(GET_USER_ASM_B, "u8");
  }

  void handleCpuVmxOff(Module &m) {
    for (CallBrInst *callbr : getTargetAsmCallBrs(m, CPU_VMX_OFF, false)) {
      IRBuilder<> b(callbr);
      BasicBlock *block = callbr->getDefaultDest();
      BranchInst *br = b.CreateBr(block);
      callbr->replaceAllUsesWith(br);
      callbr->eraseFromParent();
    }
  }

  void handleBinaryCallBr(Module &m) {
    SmallVector<CallBrInst *, 16> binaryCallBrs;
    for (Function &f : m) {
      for (Instruction &inst : instructions(f)) {
        if (CallBrInst *callbr = dyn_cast<CallBrInst>(&inst)) {
          if (callbr->getNumSuccessors() == 2)
            binaryCallBrs.push_back(callbr);
        }
      }
    }
    Function *ndBool = getOrCreateNdIntFn(m, 1);
    for (CallBrInst *callbr : binaryCallBrs) {
      IRBuilder<> b(callbr);
      Value *cond = b.CreateCall(ndBool);
      BasicBlock *defaultBlk = callbr->getDefaultDest();
      BasicBlock *indBlk = callbr->getIndirectDest(0);
      BranchInst *branch = b.CreateCondBr(cond, defaultBlk, indBlk);
      callbr->replaceAllUsesWith(branch);
      callbr->eraseFromParent();
    }
  }

  void handleCallOnStack(Module &m) {
    for (CallInst *call : getTargetAsmCalls(m, CALL_ON_STACK, false)) {
      IRBuilder<> b(call);
      Value *funcPtr = call->getArgOperand(1);
      if (BitCastOperator *bitcast = dyn_cast<BitCastOperator>(funcPtr)) {
        Function *func = cast<Function>(bitcast->getOperand(0));
        Value *replace = b.CreateCall(func);
        call->replaceAllUsesWith(replace);
        call->eraseFromParent();
      } else {
        errs() << "TODO: handleCallOnStack\n";
      }
    }
  }

  void handleOptimizerHideVar(Module &m) {
    for (CallInst *call :
         getTargetAsmCalls(m, "", false, OPTIMIZER_HIDE_VAR_CONSTRAINTS)) {
      Value *v = call->getArgOperand(0);
      call->replaceAllUsesWith(v);
      call->eraseFromParent();
    }
  }

  void handleRandom(Module &m) {
    Type *i64Ty = Type::getInt64Ty(m.getContext());
    auto replace = [this, &m, i64Ty](const std::string &targetAsm) {
      for (CallInst *call : getTargetAsmCalls(m, targetAsm, true)) {
        IRBuilder<> b(call);
        StructType *type = cast<StructType>(call->getType());
        Value *empty = UndefValue::get(type);
        Value *setLow = b.CreateInsertValue(empty, b.getInt8(1), {0});
        FunctionCallee nd = getNondetFn(i64Ty, m);
        Value *replace = b.CreateInsertValue(setLow, b.CreateCall(nd), {1});
        call->replaceAllUsesWith(replace);
        call->eraseFromParent();
      }
    };
    replace(RDSEED_PREFIX);
    replace(RDRAND_PREFIX);
  }

  FunctionCallee makeNewNondetFn(Module &m, Type &type, unsigned num,
                                 std::string prefix) {
    std::string name;
    unsigned c = num;
    do {
      name = prefix + std::to_string(c++);
    } while (m.getNamedValue(name));
    FunctionCallee res = m.getOrInsertFunction(name, &type);
    return res;
  }

  FunctionCallee getNondetFn(Type *type, Module &M) {
    auto it = ndfn.find(type);
    if (it != ndfn.end()) {
      return it->second;
    }

    FunctionCallee res =
        makeNewNondetFn(M, *type, ndfn.size(), "verifier.nondet.");
    ndfn[type] = res;
    return res;
  }
};

char HandleInlineAsm::ID = 0;

Pass *createHandleInlineAsmPass() { return new HandleInlineAsm(); }
} // namespace seahorn