// Guest function -> LLVM IR. Each instruction is a call of its semantic function with the constant instruction word and pc,
// on a local State (an alloca that SROA turns into SSA values); control flow follows program.db's CFG. finish() links the
// semantics bitcode in and inlines it, so the calls fold down to the operations themselves (Remill's approach).
#include "nxlift/lift.hpp"

#include "nxw/a64.hpp"
#include "nxw/rt.h"
#include "nxw/sem.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cstddef>
#include <mutex>
#include <set>

extern "C" int nxw_sem_sysreg_inline(uint32_t sysreg, int write);

namespace nxlift {

using namespace llvm;
using namespace nxanalyze;
using nxw::a64::Op;
using State = nxw::sem::State;

void LiftStats::add(const LiftStats& o) {
    for (auto& [k, v] : o.unsupported) unsupported[k] += v;
    functions += o.functions, instructions += o.instructions, blocks += o.blocks;
    calls_guest += o.calls_guest, calls_host += o.calls_host, calls_indirect += o.calls_indirect, tails += o.tails;
    tails_indirect += o.tails_indirect, jump_tables += o.jump_tables, traps += o.traps, svcs += o.svcs, sysreg_calls += o.sysreg_calls;
    yields += o.yields, contexts += o.contexts;
}

namespace {

constexpr uint64_t off_x = offsetof(State, x);
constexpr uint64_t off_cpu_pc = offsetof(NxwGuestCpu, pc);

std::once_flag g_targets;

[[noreturn]] void fail_llvm(const std::string& what, Error e) {
    std::string msg;
    raw_string_ostream os(msg);
    os << what << ": " << e;
    nxw::fail(msg);
}

}  // namespace

struct Lifter::Impl {
    const Inputs& in;
    LLVMContext& ctx;
    Options opt;
    std::unique_ptr<Module> sem;   // the parsed semantics; each finished module links a clone of what it uses
    std::set<std::string, std::less<>> sem_defined;
    std::string cpu_name, features;
    Triple triple;
    std::string datalayout;
    std::unique_ptr<TargetMachine> tm;

    Type *i8, *i32, *i64, *vd;
    PointerType* ptr;
    FunctionType *fn_ty, *sem_ty;

    Impl(const Inputs& i, LLVMContext& c, const Options& o) : in(i), ctx(c), opt(o) {}
};

// ---------------------------------------------------------------- the per-function builder
namespace {

struct FnBuilder {
    Lifter::Impl& L;
    LiftStats& st;
    Module& M;
    uint32_t fi;
    const FunctionRec& fr;
    uint64_t base;
    const uint32_t* words;
    Function* F = nullptr;
    Argument* cpu = nullptr;
    AllocaInst* S = nullptr;
    Value* lr0 = nullptr;
    IRBuilder<> B;
    std::vector<BasicBlock*> bbs;

    FnBuilder(Lifter::Impl& l, LiftStats& s, Module& m, uint32_t f)
        : L(l), st(s), M(m), fi(f), fr(l.in.db.functions[f]), base(l.in.db.modules[fr.module].base), words(l.in.words(f)), B(l.ctx) {}
    // a sequence of words without a program.db function (lift_words)
    FnBuilder(Lifter::Impl& l, LiftStats& s, Module& m, const FunctionRec& r, uint64_t b, const uint32_t* w)
        : L(l), st(s), M(m), fi(none32), fr(r), base(b), words(w), B(l.ctx) {}

    // The function's prologue: the attributes, the local State, the registers in
    void begin(const std::string& name) {
        F = M.getFunction(name);
        if (!F) F = Function::Create(L.fn_ty, GlobalValue::ExternalLinkage, name, M);
        F->addParamAttr(0, Attribute::NoAlias);
        F->addParamAttr(0, Attribute::NonNull);
        F->addParamAttr(0, Attribute::getWithAlignment(L.ctx, Align(16)));
        F->addParamAttr(0, Attribute::getWithDereferenceableBytes(L.ctx, sizeof(NxwGuestCpu)));
        F->addFnAttr(Attribute::NoInline);
        F->addFnAttr(Attribute::NoUnwind);
        F->addFnAttr("target-cpu", L.cpu_name);
        F->addFnAttr("target-features", L.features);
        if (!L.opt.jit) F->setUWTableKind(UWTableKind::Async);
        cpu = F->getArg(0);
        cpu->setName("cpu");
        BasicBlock* entry = BasicBlock::Create(L.ctx, "entry", F);
        B.SetInsertPoint(entry);
        S = B.CreateAlloca(ArrayType::get(L.i8, sizeof(State)), nullptr, "s");
        S->setAlignment(Align(16));
        do_enter();   // stays in the entry block: written_fields finds the State through it
        lr0 = B.CreateLoad(L.i64, xslot(30), "lr0");
        if (L.opt.entry_trace && fi != none32) entry_trace();
    }

    FunctionCallee decl(StringRef name, FunctionType* ty) { return M.getOrInsertFunction(name, ty); }
    FunctionCallee semfn(StringRef op) { return decl(("nxw_sem_" + op).str(), L.sem_ty); }
    FunctionCallee enter() { return decl("nxw_sem_enter", FunctionType::get(L.vd, {L.ptr, L.ptr}, false)); }
    FunctionCallee leave() { return decl("nxw_sem_leave", FunctionType::get(L.vd, {L.ptr, L.ptr}, false)); }
    FunctionCallee lookup() { return decl("nxw_sem_lookup", FunctionType::get(L.ptr, {L.ptr, L.i64}, false)); }
    FunctionCallee rt_trap() {
        FunctionCallee c = decl("rt_trap", FunctionType::get(L.vd, {L.ptr, L.i64, L.i32}, false));
        if (auto* f = dyn_cast<Function>(c.getCallee())) f->setDoesNotReturn();
        return c;
    }
    FunctionCallee guest(uint32_t f) { return decl(L.in.fn_name(f), L.fn_ty); }

    // if (rt_trace_on) rt_trace_entry(cpu, entry): the caller left the state in cpu, and the hook does not change it
    void entry_trace() {
        GlobalVariable* on = M.getGlobalVariable("rt_trace_on");
        if (!on) on = new GlobalVariable(M, L.i8, false, GlobalValue::ExternalLinkage, nullptr, "rt_trace_on");
        LoadInst* v = B.CreateLoad(L.i8, on);
        v->setVolatile(true);
        BasicBlock* t = BasicBlock::Create(L.ctx, "trace", F);
        BasicBlock* go = BasicBlock::Create(L.ctx, "body", F);
        B.CreateCondBr(B.CreateICmpNE(v, ConstantInt::get(L.i8, 0)), t, go, MDBuilder(L.ctx).createUnlikelyBranchWeights());
        B.SetInsertPoint(t);
        B.CreateCall(decl("rt_trace_entry", FunctionType::get(L.vd, {L.ptr, L.i64}, false)), {cpu, c64(base + fr.start)});
        B.CreateBr(go);
        B.SetInsertPoint(go);
    }
    // Spin-loop heads, YIELD, WFE, WFI: the host's yield, with the state in cpu
    void yield(uint32_t kind, uint64_t site) {
        st.yields++;
        do_leave();
        set_pc(site);
        B.CreateCall(decl("rt_yield", FunctionType::get(L.vd, {L.ptr, L.i32, L.i64}, false)), {cpu, c32(kind), c64(site)});
        do_enter();
    }

    Constant* c32(uint64_t v) { return ConstantInt::get(L.i32, v); }
    Constant* c64(uint64_t v) { return ConstantInt::get(L.i64, v); }
    Value* sem_call(Op op, uint32_t w, uint64_t pc) {
        return B.CreateCall(semfn(nxw::a64::op_name(op)), {S, c32(w), c64(pc)});
    }
    Value* xslot(unsigned r) { return B.CreateConstInBoundsGEP1_64(L.i8, S, off_x + 8 * r); }
    void do_leave() { B.CreateCall(leave(), {S, cpu}); }
    void do_enter() { B.CreateCall(enter(), {S, cpu}); }
    void set_pc(uint64_t pc) { B.CreateStore(c64(pc), B.CreateConstInBoundsGEP1_64(L.i8, cpu, off_cpu_pc)); }

    void trap(uint64_t pc, uint32_t why) {
        st.traps++;
        do_leave();
        set_pc(pc);
        B.CreateCall(rt_trap(), {cpu, c64(pc), c32(why)});
        B.CreateUnreachable();
    }
    BasicBlock* trap_block(uint64_t pc, uint32_t why, const char* name) {
        BasicBlock* bb = BasicBlock::Create(L.ctx, name, F);
        IRBuilder<>::InsertPointGuard g(B);
        B.SetInsertPoint(bb);
        trap(pc, why);
        return bb;
    }

    const CallRec* call_rec(uint64_t pc) const {
        auto it = L.in.call_at.find(uint64_t(fr.module) << 32 | uint32_t(pc - base));
        return it == L.in.call_at.end() ? nullptr : &L.in.db.calls[it->second];
    }
    // The host function a direct call goes to: a lifted guest function or an HLE adapter (null: no target, trap)
    FunctionCallee callee_of(const CallRec& c, bool& host) {
        host = false;
        if (c.callee != none32) return guest(c.callee);
        if ((c.binding == bind_forced || c.binding == bind_swhle || c.binding == bind_stub) && c.symbol != none32) {
            host = true;
            return decl((std::string("adapter_") + L.in.db.str(L.in.db.symbols[c.symbol].name)), L.fn_ty);
        }
        return {};
    }

    // A call bound through a PLT stub skips the stub, but leaves x16 and x17 as it would: the GOT slot and its content
    void plt_regs(const CallRec& c) {
        if (c.symbol == none32) return;
        auto it = L.in.plt_at.find(uint64_t(c.module) << 32 | c.target);
        if (it == L.in.plt_at.end()) return;
        const PltStubRec& s = L.in.db.plt_stubs[it->second];
        const uint64_t got = L.in.db.modules[s.module].base + s.got;
        B.CreateStore(c64(got), xslot(16));
        LoadInst* v = B.CreateLoad(L.i64, B.CreateIntToPtr(c64(got), L.ptr));
        v->setVolatile(true);
        B.CreateStore(v, xslot(17));
    }

    // A call: x30 is already set; the state goes to cpu and comes back
    void call_direct(uint64_t pc, uint32_t w, Op op) {
        const CallRec* c = call_rec(pc);
        bool host = false;
        FunctionCallee f = c && !L.opt.swapin ? callee_of(*c, host) : FunctionCallee{};
        Value* t = sem_call(op, w, pc);   // BL: x30 = pc + 4; returns the target
        if (!f) {   // weak zero or unbound (or swap-in): what the guest would do, through the table
            call_indirect(t);
            return;
        }
        plt_regs(*c);
        (host ? st.calls_host : st.calls_guest)++;
        do_leave();
        B.CreateCall(f, {cpu});
        do_enter();
    }
    void call_indirect(Value* target) {
        st.calls_indirect++;
        do_leave();
        Value* f = B.CreateCall(lookup(), {cpu, target});
        B.CreateCall(L.fn_ty, f, {cpu});
        do_enter();
    }
    void tail_direct(const CallRec& c) {
        bool host = false;
        FunctionCallee f = L.opt.swapin ? FunctionCallee{} : callee_of(c, host);
        if (!f) {
            const uint64_t t = L.in.db.modules[c.module].base + c.target;
            tail_indirect(c64(t));
            return;
        }
        st.tails++;
        plt_regs(c);
        do_leave();
        CallInst* ci = B.CreateCall(f, {cpu});
        ci->setTailCallKind(CallInst::TCK_MustTail);
        B.CreateRetVoid();
    }
    void tail_indirect(Value* target) {
        st.tails_indirect++;
        do_leave();
        Value* f = B.CreateCall(lookup(), {cpu, target});
        CallInst* ci = B.CreateCall(L.fn_ty, f, {cpu});
        ci->setTailCallKind(CallInst::TCK_MustTail);
        B.CreateRetVoid();
    }

    // One instruction that does not end its block (or ends it by falling through). False: it trapped, the block is closed.
    bool insn(Op op, uint32_t w, uint64_t pc) {
        st.instructions++;
        switch (op) {
        case Op::BL: call_direct(pc, w, op); return true;
        case Op::BLR:
        case Op::BLRAA:
        case Op::BLRAAZ: call_indirect(sem_call(op, w, pc)); return true;
        case Op::SVC:
            st.svcs++;
            do_leave();
            set_pc(pc);
            B.CreateCall(decl("rt_svc", FunctionType::get(L.vd, {L.ptr, L.i32}, false)), {cpu, c32(w >> 5 & 0xffff)});
            do_enter();
            return true;
        case Op::MRS:
        case Op::MSR_reg: {
            const uint32_t sr = w >> 5 & 0x7fff;
            const bool write = op == Op::MSR_reg;
            if (nxw_sem_sysreg_inline(sr, write)) break;
            st.sysreg_calls++;
            const unsigned rt = w & 31;
            if (write) {
                Value* v = rt == 31 ? static_cast<Value*>(c64(0)) : B.CreateLoad(L.i64, xslot(rt));
                do_leave();
                set_pc(pc);
                B.CreateCall(decl("rt_msr", FunctionType::get(L.vd, {L.ptr, L.i32, L.i64}, false)), {cpu, c32(sr), v});
                do_enter();
            } else {
                do_leave();
                set_pc(pc);
                Value* v = B.CreateCall(decl("rt_mrs", FunctionType::get(L.i64, {L.ptr, L.i32}, false)), {cpu, c32(sr)});
                do_enter();
                if (rt != 31) B.CreateStore(v, xslot(rt));
            }
            return true;
        }
        case Op::HINT:   // YIELD, WFE and WFI yield to the host; the other hints are the semantics' (nothing)
            if (L.opt.spin_yield && (w >> 8 & 15) == 0 && (w >> 5 & 7) >= 1 && (w >> 5 & 7) <= 3) {
                yield((w >> 5 & 7) == 1 ? NXW_YIELD_HINT : NXW_YIELD_WFE, pc);
                return true;
            }
            break;
        case Op::UDF: trap(pc, NXW_TRAP_UDF | (w & 0xffff) << 8); return false;
        case Op::BRK: trap(pc, NXW_TRAP_BRK | (w >> 5 & 0xffff) << 8); return false;
        case Op::HLT: trap(pc, NXW_TRAP_HLT | (w >> 5 & 0xffff) << 8); return false;
        case Op::Unallocated: trap(pc, NXW_TRAP_UNDECODABLE); return false;
        case Op::ERET:
        case Op::ERETAA:
        case Op::DRPS: trap(pc, NXW_TRAP_ERET); return false;
        case Op::B:
        case Op::BR:
        case Op::BRAA:
        case Op::BRAAZ:
        case Op::RET:
        case Op::RETAA:
        case Op::B_cond:
        case Op::CBZ:
        case Op::CBNZ:
        case Op::TBZ:
        case Op::TBNZ:   // a branch that program.db did not end a block with: cannot happen for its CFG
            trap(pc, NXW_TRAP_OFF_CFG);
            return false;
        default: break;
        }
        const char* name = nxw::a64::op_name(op);
        if (!L.sem_defined.count(std::string("nxw_sem_") + name)) {
            st.unsupported[name]++;
            trap(pc, (op == Op::HVC || op == Op::SMC || op == Op::MSR_imm ? NXW_TRAP_SYSTEM : NXW_TRAP_UNSUPPORTED) | uint32_t(op) << 8);
            return false;
        }
        sem_call(op, w, pc);
        return true;
    }

    BasicBlock* edge(const BlockRec& b, uint8_t kind) {
        for (uint32_t e = b.first_edge; e < b.first_edge + b.edge_count; e++)
            if (L.in.db.edges[e].kind == kind) return bbs[L.in.db.edges[e].block - fr.first_block];
        return nullptr;
    }

    void block(uint32_t bi) {
        const BlockRec& b = L.in.db.blocks[fr.first_block + bi];
        B.SetInsertPoint(bbs[bi]);
        st.blocks++;
        if ((b.flags & blk_spin_head) && L.opt.spin_yield) yield(NXW_YIELD_SPIN, base + b.start);
        const uint32_t n = b.size / 4, first = (b.start - fr.start) / 4;
        for (uint32_t k = 0; k + 1 < n; k++) {
            const uint64_t pc = base + b.start + 4 * k;
            const uint32_t w = words[first + k];
            if (!insn(nxw::a64::decode(w), w, pc)) return;
        }
        const uint64_t pc = base + b.start + 4 * (n - 1);
        const uint32_t w = words[first + n - 1];
        const Op op = nxw::a64::decode(w);
        switch (b.term) {
        case term_fall:
            if (!insn(op, w, pc)) return;
            if (BasicBlock* t = edge(b, edge_fall)) B.CreateBr(t);
            else trap(pc + 4, NXW_TRAP_FALLOFF);
            return;
        case term_branch:
            st.instructions++;
            if (BasicBlock* t = edge(b, edge_branch)) B.CreateBr(t);
            else trap(pc, NXW_TRAP_FALLOFF);
            return;
        case term_cond: {
            st.instructions++;
            Value* taken = B.CreateICmpNE(sem_call(op, w, pc), c64(0));
            BasicBlock* t = edge(b, edge_taken);
            if (!t) {   // a conditional tail call
                t = BasicBlock::Create(L.ctx, "ctail", F);
                IRBuilder<>::InsertPointGuard g(B);
                B.SetInsertPoint(t);
                if (const CallRec* c = call_rec(pc)) tail_direct(*c);
                else trap(pc, NXW_TRAP_FALLOFF);
            }
            BasicBlock* nt = edge(b, edge_not_taken);
            if (!nt) nt = trap_block(pc + 4, NXW_TRAP_FALLOFF, "falloff");
            B.CreateCondBr(taken, t, nt);
            return;
        }
        case term_jump_table: {
            st.instructions++, st.jump_tables++;
            Value* t = sem_call(op, w, pc);
            SwitchInst* sw = B.CreateSwitch(t, trap_block(pc, NXW_TRAP_JUMP_TABLE, "jt_miss"), b.edge_count);
            for (uint32_t e = b.first_edge; e < b.first_edge + b.edge_count; e++) {
                const EdgeRec& er = L.in.db.edges[e];
                if (er.kind != edge_jump_table) continue;
                const BlockRec& tb = L.in.db.blocks[er.block];
                sw->addCase(cast<ConstantInt>(c64(base + tb.start)), bbs[er.block - fr.first_block]);
            }
            return;
        }
        case term_tail: {
            st.instructions++;
            if (const CallRec* c = call_rec(pc)) tail_direct(*c);
            else tail_indirect(sem_call(op, w, pc));
            return;
        }
        case term_indirect_tail: st.instructions++; tail_indirect(sem_call(op, w, pc)); return;
        case term_return: {
            st.instructions++;
            if (op == Op::RET && (w >> 5 & 31) != 30) {   // ret xN: a return only when xN is the caller's return address
                Value* t = sem_call(op, w, pc);
                BasicBlock* r = BasicBlock::Create(L.ctx, "ret", F);
                BasicBlock* j = BasicBlock::Create(L.ctx, "ret_other", F);
                B.CreateCondBr(B.CreateICmpEQ(t, lr0), r, j);
                B.SetInsertPoint(r);
                do_leave();
                B.CreateRetVoid();
                B.SetInsertPoint(j);
                tail_indirect(t);
                return;
            }
            if (op != Op::RET && op != Op::RETAA) {
                if (!insn(op, w, pc)) return;
            }
            do_leave();
            B.CreateRetVoid();
            return;
        }
        case term_trap:
            if (!insn(op, w, pc)) return;
            trap(pc, NXW_TRAP_UNDECODABLE);
            return;
        case term_noreturn_call:
            if (!insn(op, w, pc)) return;
            trap(pc + 4, NXW_TRAP_NORETURN);
            return;
        case term_eret: st.instructions++; trap(pc, NXW_TRAP_ERET); return;
        default:   // term_end: the last instruction falls off the function
            if (!insn(op, w, pc)) return;
            trap(pc + 4, NXW_TRAP_FALLOFF);
            return;
        }
    }

    Function* build() {
        const std::string name = L.in.fn_name(fi);
        if (Function* f = M.getFunction(name); f && !f->isDeclaration()) return f;
        begin(name);
        // SDK 22's setjmp signs x30 with pacib and restores it before ret (only an x30 write to the analysis): lifted as an ordinary function, whose
        // first return is the only one; longjmp stays a context function
        const std::string sym = L.in.db.str(fr.name);
        const bool setjmp_like = sym == "__setjmp" || sym == "_setjmp" || sym == "setjmp" || sym == "__sigsetjmp" || sym == "sigsetjmp";
        const uint32_t ctx_flags = fr.flags & (fn_sp_switch | fn_lr_write | fn_context_branch | fn_ret_other);
        if (ctx_flags && !(setjmp_like && ctx_flags == fn_lr_write)) {
            // one of sdk's four context functions (docs/m2-nxanalyze.md §6): no host stack can follow its SP or x30 rewrite
            st.contexts++, st.functions++;
            do_leave();
            set_pc(base + fr.start);
            if (sym.find("DispatchFiberContext") != std::string::npos) {
                // nn::os fibers: the runtime swaps host coroutines and the guest register frame (nxrt host/fiber.c), then this call returns on the
                // resumed context's host stack, whose caller reloads the registers from cpu
                B.CreateCall(decl("rt_fiber_switch", FunctionType::get(L.vd, {L.ptr}, false)), {cpu});
                B.CreateRetVoid();
                return F;
            }
            CallInst* c = B.CreateCall(decl("rt_nonlocal", FunctionType::get(L.vd, {L.ptr}, false)), {cpu});
            c->setDoesNotReturn();
            B.CreateUnreachable();
            return F;
        }
        if (fr.block_count == 0 && fr.size == 0) {   // an empty function (a zero-length FDE or symbol): running into it falls off the end
            st.functions++;
            trap(base + fr.start, NXW_TRAP_FALLOFF);
            return F;
        }
        bbs.resize(fr.block_count);
        for (uint32_t i = 0; i < fr.block_count; i++) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "b%llx", static_cast<unsigned long long>(base + L.in.db.blocks[fr.first_block + i].start));
            bbs[i] = BasicBlock::Create(L.ctx, nm, F);
        }
        auto hex_s = [](uint64_t v) { char t[24]; std::snprintf(t, sizeof t, "%llx", static_cast<unsigned long long>(v)); return std::string(t); };
        if (fr.block_count == 0 || L.in.db.blocks[fr.first_block].start != fr.start) nxw::fail("function without an entry block: " + name + " (start 0x" + hex_s(fr.start) + ", " + std::to_string(fr.block_count) + " blocks, first block at 0x" + hex_s(fr.block_count ? L.in.db.blocks[fr.first_block].start : 0) + ", size " + std::to_string(fr.size) + ")");
        B.CreateBr(bbs[0]);
        for (uint32_t i = 0; i < fr.block_count; i++) block(i);
        st.functions++;
        return F;
    }

    Function* build_words(const std::string& name, uint32_t n) {
        begin(name);
        for (uint32_t k = 0; k < n; k++)
            if (!insn(nxw::a64::decode(words[k]), words[k], base + 4 * k)) return F;
        do_leave();
        B.CreateRetVoid();
        return F;
    }
};

}  // namespace

// ---------------------------------------------------------------- Lifter
Lifter::Lifter(const Inputs& in, LLVMContext& ctx, const Options& opt) : p(std::make_unique<Impl>(in, ctx, opt)) {
    std::call_once(g_targets, [] {
#if defined(NXW_LIFT_AARCH64)   // the Mac swap-in harness (the semantics bitcode's triple is arm64-apple-macos)
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
#else
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeX86AsmParser();
#endif
    });
    const std::string_view bc = semantics_bitcode();
    auto m = parseBitcodeFile(MemoryBufferRef(StringRef(bc.data(), bc.size()), "nxw_sem.bc"), ctx);
    if (!m) fail_llvm("cannot read the semantics bitcode", m.takeError());
    p->sem = std::move(*m);
    for (Function& f : *p->sem) {
        if (f.isDeclaration()) continue;
        p->sem_defined.insert(f.getName().str());
        if (p->cpu_name.empty() && f.hasFnAttribute("target-cpu")) {
            p->cpu_name = f.getFnAttribute("target-cpu").getValueAsString().str();
            p->features = f.getFnAttribute("target-features").getValueAsString().str();
        }
    }
    p->triple = p->sem->getTargetTriple();
    p->datalayout = p->sem->getDataLayoutStr();
    p->i8 = Type::getInt8Ty(ctx), p->i32 = Type::getInt32Ty(ctx), p->i64 = Type::getInt64Ty(ctx);
    p->vd = Type::getVoidTy(ctx);
    p->ptr = PointerType::getUnqual(ctx);
    p->fn_ty = FunctionType::get(p->vd, {p->ptr}, false);
    p->sem_ty = FunctionType::get(p->i64, {p->ptr, p->i32, p->i64}, false);

    std::string err;
    const Target* t = TargetRegistry::lookupTarget(p->triple, err);
    if (!t) nxw::fail("no " + p->triple.str() + " target in LLVM: " + err);
    TargetOptions to;
    to.AllowFPOpFusion = FPOpFusion::Strict;   // never fuse a separate FMUL and FADD
    const auto cm = opt.jit ? std::optional<CodeModel::Model>(CodeModel::Large) : std::nullopt;
    // ELF: the game is a shared object the runtime dlopens, so position-independent code
    const auto rm = !opt.jit && p->triple.isOSBinFormatELF() ? std::optional<Reloc::Model>(Reloc::PIC_) : std::nullopt;
    p->tm.reset(t->createTargetMachine(p->triple, p->cpu_name, p->features, to, rm, cm,
                                       opt.opt >= 2 ? CodeGenOptLevel::Default : CodeGenOptLevel::Less));
    if (!p->tm) nxw::fail("cannot create the " + p->triple.str() + " target machine");
}

Lifter::~Lifter() = default;

TargetMachine& Lifter::target() { return *p->tm; }
bool Lifter::has_semantics(const std::string& op) const { return p->sem_defined.count("nxw_sem_" + op) != 0; }

std::unique_ptr<Module> Lifter::new_module(const std::string& name) {
    auto m = std::make_unique<Module>(name, p->ctx);
    m->setTargetTriple(p->triple);
    m->setDataLayout(p->tm->createDataLayout());
    return m;
}

Function* Lifter::lift(Module& m, uint32_t fn) {
    FnBuilder b(*p, stats, m, fn);
    return b.build();
}

Function* Lifter::lift_words(Module& m, const std::string& name, const std::vector<uint32_t>& words, uint64_t pc) {
    static const FunctionRec none{};
    FnBuilder b(*p, stats, m, none, pc, words.data());
    return b.build_words(name, uint32_t(words.size()));
}

namespace {

// Guest memory accesses (anything not on cpu or the local state) never alias NxwGuestCpu: say so with one alias scope, so
// GVN and DSE can keep registers in SSA values across volatile guest loads and stores (calls still clobber cpu)
void tag_alias_scopes(Module& M) {
    LLVMContext& ctx = M.getContext();
    MDBuilder mdb(ctx);
    MDNode* dom = mdb.createAnonymousAliasScopeDomain("nxw");
    MDNode* scope = mdb.createAnonymousAliasScope(dom, "cpu");
    MDNode* list = MDNode::get(ctx, {scope});
    for (Function& F : M) {
        if (F.isDeclaration() || !F.getName().starts_with("fn_")) continue;
        const Argument* cpu = F.getArg(0);
        for (Instruction& I : instructions(F)) {
            const Value* ptr = getLoadStorePointerOperand(&I);
            if (!ptr) {
                if (auto* a = dyn_cast<AtomicRMWInst>(&I)) ptr = a->getPointerOperand();
                else if (auto* c = dyn_cast<AtomicCmpXchgInst>(&I)) ptr = c->getPointerOperand();
            }
            if (ptr) {
                const Value* u = getUnderlyingObject(ptr, 0);
                if (u == cpu) I.setMetadata(LLVMContext::MD_alias_scope, list);
                else if (!isa<AllocaInst>(u)) I.setMetadata(LLVMContext::MD_noalias, list);
                continue;
            }
            if (auto* mt = dyn_cast<MemTransferInst>(&I))
                if (getUnderlyingObject(mt->getSource(), 0) == cpu || getUnderlyingObject(mt->getDest(), 0) == cpu)
                    I.setMetadata(LLVMContext::MD_alias_scope, list);
        }
    }
}

// The state fields a lifted function's instructions write, as nxw_sem_leave_mask's masks: every store into the State alloca
// once the instruction semantics are inlined (enter and leave still calls). Unknown offsets count as writing everything.
std::pair<uint64_t, uint64_t> written_fields(Function& F, const DataLayout& dl) {
    // the State is what nxw_sem_enter fills (inlining moves the semantics' own allocas in front of it)
    Value* s = nullptr;
    for (Instruction& i : F.getEntryBlock())
        if (auto* c = dyn_cast<CallInst>(&i))
            if (c->getCalledFunction() && c->getCalledFunction()->getName() == "nxw_sem_enter") {
                s = c->getArgOperand(0);
                break;
            }
    if (!s || !isa<AllocaInst>(s)) return {~0ull, ~0ull};
    std::vector<std::pair<uint64_t, uint64_t>> ranges;   // [begin, end) byte ranges written
    bool unknown = false;
    std::vector<std::pair<Value*, int64_t>> work{{s, 0}};   // pointer, constant offset (-1: unknown)
    std::set<Value*> seen;
    while (!work.empty() && !unknown) {
        auto [v, off] = work.back();
        work.pop_back();
        if (!seen.insert(v).second) continue;
        for (User* u : v->users()) {
            if (auto* g = dyn_cast<GetElementPtrInst>(u)) {
                APInt o(64, 0);
                if (off >= 0 && g->getPointerOperand() == v && g->accumulateConstantOffset(dl, o)) work.push_back({g, off + o.getSExtValue()});
                else unknown = true;
            } else if (auto* st = dyn_cast<StoreInst>(u)) {
                if (st->getPointerOperand() != v || off < 0) { unknown = true; continue; }
                ranges.push_back({uint64_t(off), uint64_t(off) + dl.getTypeStoreSize(st->getValueOperand()->getType())});
            } else if (isa<LoadInst>(u)) {
            } else if (auto* mi = dyn_cast<MemIntrinsic>(u)) {
                auto* len = dyn_cast<ConstantInt>(mi->getLength());
                if (mi->getDest() == v && len && off >= 0) ranges.push_back({uint64_t(off), uint64_t(off) + len->getZExtValue()});
                else if (mi->getDest() == v || !isa<MemTransferInst>(mi)) unknown = true;
            } else if (auto* c = dyn_cast<CallInst>(u)) {
                const Function* cf = c->getCalledFunction();
                if (cf && (cf->getName() == "nxw_sem_enter" || cf->getName() == "nxw_sem_leave")) continue;
                if (cf && cf->isIntrinsic() && cf->getIntrinsicID() == Intrinsic::lifetime_start) continue;
                if (cf && cf->isIntrinsic() && cf->getIntrinsicID() == Intrinsic::lifetime_end) continue;
                unknown = true;
            } else {
                unknown = true;   // selects, phis, casts to integers: give up
            }
        }
    }
    if (unknown) return {~0ull, ~0ull};
    auto hit = [&](uint64_t b, uint64_t e) {
        for (auto& [rb, re] : ranges)
            if (rb < e && b < re) return true;
        return false;
    };
    uint64_t m0 = 0, m1 = 0;
    for (unsigned i = 0; i < 31; i++)
        if (hit(offsetof(State, x) + 8 * i, offsetof(State, x) + 8 * i + 8)) m0 |= 1ull << i;
    if (hit(offsetof(State, sp), offsetof(State, sp) + 8)) m0 |= 1ull << 31;
    for (unsigned i = 0; i < 32; i++)
        if (hit(offsetof(State, q) + 16 * i, offsetof(State, q) + 16 * i + 16)) m0 |= 1ull << (32 + i);
    if (hit(offsetof(State, n), offsetof(State, v) + 1)) m1 |= nxw::sem::lm_nzcv;
    if (hit(offsetof(State, fpcr), offsetof(State, fpcr) + 8)) m1 |= nxw::sem::lm_fpcr;
    if (hit(offsetof(State, fpsr), offsetof(State, fpsr) + 8)) m1 |= nxw::sem::lm_fpsr;
    if (hit(offsetof(State, tpidr_el0), offsetof(State, tpidr_el0) + 8)) m1 |= nxw::sem::lm_tpidr;
    if (hit(offsetof(State, tpidrro_el0), offsetof(State, tpidrro_el0) + 8)) m1 |= nxw::sem::lm_tpidrro;
    if (hit(offsetof(State, excl_addr), offsetof(State, excl_val) + 16)) m1 |= nxw::sem::lm_excl;
    return {m0, m1};
}

void run_passes(Module& m, TargetMachine* tm, bool simplify) {
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    PassBuilder pb(tm);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    ModulePassManager mpm;
    mpm.addPass(AlwaysInlinerPass(false));
    if (simplify) mpm.addPass(createModuleToFunctionPassAdaptor(InstSimplifyPass()));
    mpm.addPass(GlobalDCEPass());
    mpm.run(m, mam);
}

}  // namespace

void Lifter::finish(Module& m) {
    // the semantics this module calls, cloned from the parsed bitcode, then forced inline and dropped
    m.getOrInsertFunction("nxw_sem_leave_mask", FunctionType::get(p->vd, {p->ptr, p->ptr, p->i64, p->i64}, false));   // for phase 2
    if (Linker::linkModules(m, CloneModule(*p->sem), Linker::Flags::LinkOnlyNeeded)) nxw::fail("cannot link the semantics bitcode");
    auto is_sync = [](StringRef n) { return n == "nxw_sem_enter" || n == "nxw_sem_leave" || n == "nxw_sem_leave_mask" || n == "nxw_sem_lookup"; };
    for (Function& f : m) {
        if (f.isDeclaration() || f.getName().starts_with("fn_")) continue;
        f.setLinkage(GlobalValue::InternalLinkage);
        f.removeFnAttr(Attribute::OptimizeNone);
        if (is_sync(f.getName())) {   // phase 1: instructions only
            f.removeFnAttr(Attribute::AlwaysInline);
            f.addFnAttr(Attribute::NoInline);
        } else {
            f.removeFnAttr(Attribute::NoInline);
            f.addFnAttr(Attribute::AlwaysInline);
        }
    }
    for (GlobalVariable& g : m.globals())
        if (!g.isDeclaration()) g.setLinkage(GlobalValue::InternalLinkage);
    run_passes(m, p->tm.get(), true);

    // phase 2: leave stores only the fields the function writes
    Function* lm = m.getFunction("nxw_sem_leave_mask");
    const DataLayout& dl = m.getDataLayout();
    for (Function& F : m) {
        if (F.isDeclaration() || !F.getName().starts_with("fn_")) continue;
        const auto [m0, m1] = written_fields(F, dl);
        std::vector<CallInst*> leaves;
        for (Instruction& i : instructions(F))
            if (auto* c = dyn_cast<CallInst>(&i))
                if (c->getCalledFunction() && c->getCalledFunction()->getName() == "nxw_sem_leave") leaves.push_back(c);
        for (CallInst* c : leaves) {
            IRBuilder<> b(c);
            b.CreateCall(lm, {c->getArgOperand(0), c->getArgOperand(1), b.getInt64(m0), b.getInt64(m1)});
            c->eraseFromParent();
        }
    }
    // phase 3: the state moves inline too
    for (Function& f : m)
        if (!f.isDeclaration() && is_sync(f.getName())) f.removeFnAttr(Attribute::NoInline), f.addFnAttr(Attribute::AlwaysInline);
    run_passes(m, p->tm.get(), false);
    tag_alias_scopes(m);
    if (!p->opt.jit && p->opt.dllimport && p->triple.isOSWindows())
        for (const char* n : {"rt_code_table", "rt_code_base", "rt_code_words", "rt_host_base", "rt_trace_on"})
            if (GlobalVariable* g = m.getGlobalVariable(n)) g->setDSOLocal(false), g->setDLLStorageClass(GlobalValue::DLLImportStorageClass);
    if (!p->opt.jit && p->triple.isOSDarwin())   // the swap-in dylib defines them itself: no GOT loads
        for (const char* n : {"rt_code_table", "rt_code_base", "rt_code_words", "rt_host_base", "rt_trace_on"})
            if (GlobalVariable* g = m.getGlobalVariable(n)) g->setVisibility(GlobalValue::HiddenVisibility), g->setDSOLocal(true);
    if (!p->opt.jit && p->triple.isOSBinFormatELF()) {
        // The shared object exports only the nxw_game_* contract: the guest functions bind within it, with no dynamic symbols and no PLT between partitions.
        // What it imports from the runtime (the rt_* functions and data) stays preemptible, reached through the GOT
        for (Function& f : m)
            if (f.getName().starts_with("fn_") && !f.hasLocalLinkage()) f.setVisibility(GlobalValue::HiddenVisibility), f.setDSOLocal(true);
        for (const char* n : {"rt_code_table", "rt_code_base", "rt_code_words", "rt_host_base", "rt_trace_on"})
            if (GlobalVariable* g = m.getGlobalVariable(n)) g->setDSOLocal(false);
    }
    std::string err;
    raw_string_ostream os(err);
    if (verifyModule(m, &os)) nxw::fail("lifted IR does not verify: " + err.substr(0, 2000));
}

void Lifter::optimize(Module& m) {
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    PassBuilder pb(p->tm.get());
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(p->opt.opt >= 2 ? OptimizationLevel::O2 : OptimizationLevel::O1);
    mpm.run(m, mam);
}

std::vector<char> Lifter::emit_object(Module& m) {
    SmallVector<char, 0> buf;
    raw_svector_ostream os(buf);
    legacy::PassManager pm;
    if (p->tm->addPassesToEmitFile(pm, os, nullptr, CodeGenFileType::ObjectFile)) nxw::fail("the target cannot emit objects");
    pm.run(m);
    return std::vector<char>(buf.begin(), buf.end());
}

// What a freestanding DLL of LLVM-generated code may call, in the Microsoft x64 convention. __chkstk probes each page of a
// frame larger than a page (rax = the size; only r10, r11 and flags may change); memcpy/memmove/memset use rep movsb/stosb.
static const char k_dll_support_asm[] = R"(
    .text
    .globl __chkstk
__chkstk:
    pushq %rcx
    pushq %rax
    leaq 24(%rsp), %rcx
    cmpq $0x1000, %rax
    jb 1f
2:  subq $0x1000, %rcx
    testq %rcx, (%rcx)
    subq $0x1000, %rax
    cmpq $0x1000, %rax
    ja 2b
1:  subq %rax, %rcx
    testq %rcx, (%rcx)
    popq %rax
    popq %rcx
    retq

    .globl memcpy
memcpy:
    movq %rcx, %rax
    movq %rdi, %r9
    movq %rsi, %r10
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r8, %rcx
    rep movsb
    movq %r9, %rdi
    movq %r10, %rsi
    retq

    .globl memmove
memmove:
    movq %rcx, %rax
    movq %rdi, %r9
    movq %rsi, %r10
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r8, %rcx
    cmpq %rsi, %rdi
    jbe 1f
    leaq (%rsi,%rcx), %r11
    cmpq %r11, %rdi
    jae 1f
    leaq -1(%rsi,%rcx), %rsi
    leaq -1(%rdi,%rcx), %rdi
    std
    rep movsb
    cld
    jmp 2f
1:  rep movsb
2:  movq %r9, %rdi
    movq %r10, %rsi
    retq

    .globl memset
memset:
    movq %rcx, %r11
    movq %rdi, %r9
    movq %rcx, %rdi
    movl %edx, %eax
    movq %r8, %rcx
    rep stosb
    movq %r9, %rdi
    movq %r11, %rax
    retq
)";

std::vector<char> game_table_object(Lifter& L, const Inputs& in, const std::vector<uint32_t>& fns) {
    auto M = L.new_module("nxw_game");
    LLVMContext& ctx = M->getContext();
    Type* i8 = Type::getInt8Ty(ctx);
    Type* i32 = Type::getInt32Ty(ctx);
    Type* i64 = Type::getInt64Ty(ctx);
    FunctionType* fty = FunctionType::get(Type::getVoidTy(ctx), {PointerType::getUnqual(ctx)}, false);
    auto exported = [&](Type* t, Constant* init, const char* name) {
        auto* g = new GlobalVariable(*M, t, true, GlobalValue::ExternalLinkage, init, name);
        if (L.target().getTargetTriple().isOSWindows()) g->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
        return g;
    };
    exported(i32, ConstantInt::get(i32, NXW_ABI_VERSION), "nxw_game_abi");
    std::string id;
    for (const ModuleRec& m : in.db.modules)
        if (std::string_view(in.db.str(m.name)) == "main")
            for (uint8_t b : m.build_id) {
                char h[3];
                std::snprintf(h, sizeof h, "%02x", b);
                id += h;
            }
    exported(ArrayType::get(i8, id.size() + 1), ConstantDataArray::getString(ctx, id), "nxw_game_build_id");
    exported(i32, ConstantInt::get(i32, fns.size()), "nxw_game_function_count");
    // {guest_va, rva, flags}. COFF: the RVA is fn - __ImageBase, which it lowers to an image-relative relocation. ELF has no such relocation, and in a
    // shared object no load-time relocations are wanted on 188,000 entries: the field holds fn minus the field's own address (a link-time PC-relative
    // value in read-only data), which the runtime turns into an offset from the load base
    const bool elf = L.target().getTargetTriple().isOSBinFormatELF();
    StructType* ent = StructType::get(ctx, {i64, i32, i32});
    ArrayType* table_ty = ArrayType::get(ent, fns.size());
    GlobalVariable* elf_table = nullptr;
    if (elf) {
        elf_table = new GlobalVariable(*M, table_ty, true, GlobalValue::ExternalLinkage, nullptr, "nxw_game_functions");
        elf_table->setVisibility(GlobalValue::ProtectedVisibility);
        elf_table->setDSOLocal(true);
    }
    auto* image_base = elf ? nullptr : new GlobalVariable(*M, i8, true, GlobalValue::ExternalLinkage, nullptr, "__ImageBase");
    std::vector<Constant*> es;
    es.reserve(fns.size());
    for (uint32_t f : fns) {
        auto* fn = cast<Function>(M->getOrInsertFunction(in.fn_name(f), fty).getCallee());
        fn->setDSOLocal(true);
        fn->setVisibility(elf ? GlobalValue::HiddenVisibility : fn->getVisibility());
        Constant* base = elf ? ConstantExpr::getGetElementPtr(table_ty, elf_table, ArrayRef<Constant*>{ConstantInt::get(i32, 0), ConstantInt::get(i32, es.size()), ConstantInt::get(i32, 1)})
                             : static_cast<Constant*>(image_base);
        Constant* rva = ConstantExpr::getTrunc(ConstantExpr::getSub(ConstantExpr::getPtrToInt(fn, i64), ConstantExpr::getPtrToInt(base, i64)), i32);
        es.push_back(ConstantStruct::get(ent, {ConstantInt::get(i64, in.fn_address(f)), rva, ConstantInt::get(i32, 0)}));
    }
    if (elf) {
        elf_table->setInitializer(ConstantArray::get(table_ty, es));   // no __chkstk and no memcpy of its own: the C library is in the process
        return L.emit_object(*M);
    }
    exported(ArrayType::get(ent, es.size()), ConstantArray::get(ArrayType::get(ent, es.size()), es), "nxw_game_functions");
    new GlobalVariable(*M, i32, true, GlobalValue::ExternalLinkage, ConstantInt::get(i32, 0x9875), "_fltused");
    M->appendModuleInlineAsm(StringRef(k_dll_support_asm));
    return L.emit_object(*M);
}

std::vector<char> swapin_table_object(Lifter& L, const Inputs& in, const std::vector<uint32_t>& fns) {
    auto M = L.new_module("nxw_swapin_table");
    LLVMContext& ctx = M->getContext();
    Type* i8 = Type::getInt8Ty(ctx);
    Type* i32 = Type::getInt32Ty(ctx);
    Type* i64 = Type::getInt64Ty(ctx);
    PointerType* ptr = PointerType::getUnqual(ctx);
    FunctionType* fty = FunctionType::get(Type::getVoidTy(ctx), {ptr}, false);
    auto global = [&](Type* t, Constant* init, const char* name) {
        return new GlobalVariable(*M, t, true, GlobalValue::ExternalLinkage, init, name);
    };
    std::string id;
    for (const ModuleRec& m : in.db.modules)
        if (std::string_view(in.db.str(m.name)) == "main")
            for (uint8_t b : m.build_id) {
                char h[3];
                std::snprintf(h, sizeof h, "%02x", b);
                id += h;
            }
    global(ArrayType::get(i8, id.size() + 1), ConstantDataArray::getString(ctx, id), "nxw_swapin_build_id");
    // {name[16], base, text size} per module, as lifted
    StructType* mod = StructType::get(ctx, {ArrayType::get(i8, 16), i64, i64});
    std::vector<Constant*> ms;
    for (const ModuleRec& m : in.db.modules) {
        std::string n = in.db.str(m.name);
        n.resize(15, '\0');
        ms.push_back(ConstantStruct::get(mod, {ConstantDataArray::getString(ctx, n), ConstantInt::get(i64, m.base), ConstantInt::get(i64, m.text_size)}));
    }
    global(i32, ConstantInt::get(i32, ms.size()), "nxw_swapin_module_count");
    global(ArrayType::get(mod, ms.size()), ConstantArray::get(ArrayType::get(mod, ms.size()), ms), "nxw_swapin_modules");
    StructType* ent = StructType::get(ctx, {i64, ptr, i32, i32});
    std::vector<Constant*> es;
    es.reserve(fns.size());
    for (uint32_t f : fns) {
        auto* fn = cast<Function>(M->getOrInsertFunction(in.fn_name(f), fty).getCallee());
        const FunctionRec& r = in.db.functions[f];
        es.push_back(ConstantStruct::get(ent, {ConstantInt::get(i64, in.fn_address(f)), fn, ConstantInt::get(i32, r.flags), ConstantInt::get(i32, r.size)}));
    }
    global(i32, ConstantInt::get(i32, fns.size()), "nxw_swapin_count");
    global(ArrayType::get(ent, es.size()), ConstantArray::get(ArrayType::get(ent, es.size()), es), "nxw_swapin_table");
    return L.emit_object(*M);
}

}  // namespace nxlift
