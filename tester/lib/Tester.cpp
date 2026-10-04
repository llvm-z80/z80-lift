// Calls a runtime function the way compiled code does and checks its contract.
// Registers that carry no argument start out random, so reading one shows up
// as a broken condition.

#include "z80tester/Tester.h"
#include "z80core/Interp.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <unistd.h>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

using U128 = unsigned __int128;

// Memory layout of a call. The image must stay below WindowLo.
constexpr uint16_t StackTop = 0xC000;
constexpr uint16_t BufBase = 0xC100; // sret and pointer buffers
constexpr unsigned BufSize = 32;
constexpr uint32_t WindowLo = 0x8000, WindowHi = 0xC200;
constexpr uint16_t Sentinel = 0xFFF0; // return address of the outer call
constexpr unsigned MaxValues = 8;

struct Rng {
  uint64_t S;
  uint64_t next() {
    S ^= S >> 12;
    S ^= S << 25;
    S ^= S >> 27;
    return S * 0x2545F4914F6CDD1DULL;
  }
  U128 next128() { return U128(next()) << 64 | next(); }
};

U128 mask(unsigned Bytes) {
  return Bytes >= 16 ? ~U128(0) : (U128(1) << (Bytes * 8)) - 1;
}

uint8_t &reg(State &S, Reg8 R) {
  switch (R) {
  case RA: return S.A;
  case RB: return S.B;
  case RC: return S.C;
  case RD: return S.D;
  case RE: return S.E;
  case RH: return S.H;
  default: return S.L;
  }
}

void writeRegs(State &S, const std::vector<Reg8> &Regs, U128 V) {
  for (size_t I = 0; I < Regs.size(); ++I)
    reg(S, Regs[Regs.size() - 1 - I]) = uint8_t(V >> (8 * I));
}

U128 readRegs(State &S, const std::vector<Reg8> &Regs) {
  U128 V = 0;
  for (Reg8 R : Regs)
    V = V << 8 | reg(S, R);
  return V;
}

void writeMem(uint8_t *M, uint16_t A, U128 V, unsigned N) {
  for (unsigned I = 0; I < N; ++I)
    M[uint16_t(A + I)] = uint8_t(V >> (8 * I));
}

U128 readMem(const uint8_t *M, uint16_t A, unsigned N) {
  U128 V = 0;
  for (unsigned I = N; I-- > 0;)
    V = V << 8 | M[uint16_t(A + I)];
  return V;
}

/// Inputs worth trying more often than random bits give them.
std::vector<U128> specials(Ty T) {
  std::vector<U128> V;
  switch (T) {
  case Ty::F32:
    for (uint32_t X :
         {0x00000000u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x3EFFFFFFu,
          0x3F000000u, 0x3F000001u, 0x3F800000u, 0x3FC00000u, 0x40000000u,
          0x4AFFFFFFu, 0x4B000000u, 0x4B000001u, 0x4B800000u, 0x4EFFFFFFu,
          0x4F000000u, 0x5EFFFFFFu, 0x5F000000u, 0x7F7FFFFFu, 0x7F800000u,
          0x7F800001u, 0x7FC00000u, 0x7FFFFFFFu}) {
      V.push_back(X);
      V.push_back(X | 0x80000000u);
    }
    return V;
  case Ty::F16:
    for (uint16_t X : {0x0000, 0x0001, 0x03FF, 0x0400, 0x3800, 0x3C00, 0x3E00,
                       0x6400, 0x7BFF, 0x7C00, 0x7C01, 0x7E00, 0x7FFF}) {
      V.push_back(X);
      V.push_back(X | 0x8000);
    }
    return V;
  default: {
    unsigned Bits = sizeOf(T) * 8;
    U128 M = mask(sizeOf(T));
    for (unsigned K = 0; K < Bits; ++K) {
      U128 P = U128(1) << K;
      for (U128 X : {P, P - 1, P + 1, ~P, ~(P - 1)})
        V.push_back(X & M);
    }
    for (U128 X : {U128(0), M, M >> 1, (M >> 1) + 1, M / 3, M / 3 * 2})
      V.push_back(X);
    return V;
  }
  }
}

std::string formatValue(Ty T, U128 V) {
  unsigned Digits = sizeOf(T) * 2;
  std::string S = "0x";
  for (unsigned I = Digits; I-- > 0;)
    S += "0123456789abcdef"[unsigned(V >> (4 * I)) & 15];
  if (T == Ty::F32) {
    uint32_t Bits = uint32_t(V);
    float F;
    std::memcpy(&F, &Bits, 4);
    char Buf[32];
    snprintf(Buf, sizeof Buf, " (%.9g)", F);
    S += Buf;
  }
  return S;
}

U128 regValue(const State &S, Reg R) {
  switch (R) {
  case Reg::A: return S.A;
  case Reg::B: return S.B;
  case Reg::C: return S.C;
  case Reg::D: return S.D;
  case Reg::E: return S.E;
  case Reg::H: return S.H;
  case Reg::L: return S.L;
  case Reg::BC: return S.B << 8 | S.C;
  case Reg::DE: return S.D << 8 | S.E;
  case Reg::HL: return S.H << 8 | S.L;
  case Reg::IX: return S.IXH << 8 | S.IXL;
  case Reg::IY: return S.IYH << 8 | S.IYL;
  case Reg::SP: return S.SP;
  }
  return 0;
}

// The part of a contract being evaluated, for reporting a trap on undefined
// behaviour in it.
thread_local const char *Evaluating = nullptr;

extern "C" void onTrap(int) {
  const char *W = Evaluating;
  const char Head[] =
      "z80-tester: error: undefined behaviour in the contract at ";
  const char Other[] = "z80-tester: error: illegal instruction\n";
  if (W) {
    (void)!write(2, Head, sizeof Head - 1);
    (void)!write(2, W, strlen(W));
    (void)!write(2, "\n", 1);
  } else {
    (void)!write(2, Other, sizeof Other - 1);
  }
  _exit(2);
}

bool evaluate(const Check &C, const void *Slots) {
  alignas(16) uint8_t Out[16] = {};
  Evaluating = C.Where.c_str();
  C.Fn(Slots, Out);
  Evaluating = nullptr;
  return Out[0];
}

struct ExampleValues {
  std::string Where;
  std::vector<std::pair<unsigned, U128>> Values;
};

unsigned inputBits(const std::vector<Ty> &Params) {
  unsigned Bits = 0;
  for (Ty T : Params)
    if (T != Ty::Ptr)
      Bits += sizeOf(T) * 8;
  return Bits;
}

class Worker {
public:
  Worker(const TestPlan &P, const Image &Img, const TestOptions &O, unsigned Id)
      : P(P), O(O), Mem(Img.Mem), G{O.Seed * 0x9E3779B97F4A7C15ULL + Id + 1} {
    for (Ty T : P.Params)
      Specials.push_back(specials(T));
  }

  void runRange(uint64_t Begin, uint64_t End, bool Exhaustive) {
    U128 Vals[MaxValues];
    for (uint64_t K = Begin; K < End; ++K) {
      if (Exhaustive) {
        uint64_t Bits = K;
        for (size_t I = 0; I < P.Params.size(); ++I) {
          if (P.Params[I] == Ty::Ptr)
            continue;
          unsigned N = sizeOf(P.Params[I]) * 8;
          Vals[I] = Bits & ((uint64_t(1) << N) - 1);
          Bits >>= N;
        }
      } else {
        draw(Vals);
      }
      callOnce(Vals);
    }
  }

  /// Runs each example, drawing the values it leaves out again until
  /// `requires` holds.
  void runExamples(const std::vector<ExampleValues> &Examples) {
    unsigned Givable =
        llvm::count_if(P.Params, [](Ty T) { return T != Ty::Ptr; });
    U128 Vals[MaxValues];
    for (const ExampleValues &E : Examples) {
      unsigned Tries = E.Values.size() == Givable ? 1 : 1000;
      bool Met = false;
      for (unsigned T = 0; T < Tries && !Met; ++T) {
        draw(Vals);
        for (const auto &[I, V] : E.Values)
          Vals[I] = V;
        Met = callOnce(Vals);
      }
      if (!Met) {
        ++R.Mismatches;
        report(Vals, E.Where + ": example does not meet requires");
      }
    }
  }

  TestResult R;

private:
  const TestPlan &P;
  const TestOptions &O;
  std::vector<uint8_t> Mem;
  Rng G;
  std::vector<std::vector<U128>> Specials;

  void report(const U128 *Vals, const std::string &What) {
    if (R.Reports.size() >= O.MaxReports)
      return;
    std::string S = P.Name + "(";
    for (size_t I = 0; I < P.Params.size(); ++I) {
      if (I)
        S += ", ";
      S += P.Params[I] == Ty::Ptr ? "ptr" : formatValue(P.Params[I], Vals[I]);
    }
    R.Reports.push_back(S + "): " + What);
  }

  /// Random inputs, often special ones.
  void draw(U128 *Vals) {
    for (size_t I = 0; I < P.Params.size(); ++I) {
      const std::vector<U128> &Sp = Specials[I];
      Vals[I] = G.next() % 4 == 0 ? Sp[G.next() % Sp.size()]
                                  : G.next128() & mask(sizeOf(P.Params[I]));
    }
  }

  void randomize(State &S) {
    uint8_t *Regs[] = {&S.A,   &S.B,   &S.C,   &S.D,   &S.E,  &S.H,  &S.L,
                       &S.IXH, &S.IXL, &S.IYH, &S.IYL, &S.I,  &S.R,  &S.A2,
                       &S.F2,  &S.B2,  &S.C2,  &S.D2,  &S.E2, &S.H2, &S.L2};
    uint64_t Bits = G.next();
    for (uint8_t *Rg : Regs) {
      *Rg = uint8_t(Bits);
      Bits = Bits >> 8 ? Bits >> 8 : G.next();
    }
    Bits = G.next();
    for (bool *F : {&S.SF, &S.ZF, &S.HF, &S.PVF, &S.NF, &S.CF}) {
      *F = Bits & 1;
      Bits >>= 1;
    }
    for (uint16_t A = StackTop - 256; A < StackTop; A += 8) {
      uint64_t V = G.next();
      std::memcpy(&Mem[A], &V, 8);
    }
  }

  /// Calls the function and checks it; false if the inputs do not meet
  /// `requires`.
  bool callOnce(const U128 *Vals) {
    State S{};
    randomize(S);
    uint8_t *M = Mem.data();
    const CallLayout &L = P.Layout;

    // Pointer buffers hold the same random bytes for both sides.
    uint8_t HostBufs[MaxValues][BufSize] = {};
    uint16_t Base = StackTop - L.StackBytes;
    unsigned NumBuf = 0;
    for (size_t I = 0; I < P.Params.size(); ++I) {
      U128 V = Vals[I];
      if (P.Params[I] == Ty::Ptr) {
        uint16_t Addr = BufBase + (1 + NumBuf) * BufSize;
        for (unsigned K = 0; K < BufSize; ++K)
          M[uint16_t(Addr + K)] = HostBufs[NumBuf][K] = uint8_t(G.next());
        ++NumBuf;
        V = Addr;
      }
      const Loc &Lc = L.Params[I];
      if (!Lc.Regs.empty())
        writeRegs(S, Lc.Regs, V);
      else
        writeMem(M, Base + Lc.Offset, V, Lc.Size);
    }
    if (L.SRet)
      writeMem(M, Base, BufBase, 2);
    S.SP = Base - 2;
    writeMem(M, S.SP, Sentinel, 2);

    uint8_t IXH = S.IXH, IXL = S.IXL;
    S.PC = P.Entry;
    S.StepLimit = O.StepLimit;
    S.WriteLo = WindowLo;
    S.WriteHi = WindowHi;
    bool Decoded = true;
    if (P.Fn)
      P.Fn(&S, M);
    else
      Decoded = run(P.C, S, M, Sentinel);

    ++R.Inputs;
    R.MaxSteps = std::max(R.MaxSteps, S.Steps);

    std::string Problem;
    uint16_t WantSP = Base + L.Popped;
    if (!Decoded)
      Problem = formatv("cannot decode 0x{0:x-4}", S.PC).str();
    else if (S.Fault == FaultStepLimit)
      Problem = "does not return within the step limit";
    else if (S.Fault == FaultBadWrite)
      Problem = formatv("writes to 0x{0:x-4}", S.FaultAddr).str();
    else if (S.Halted)
      Problem = "halts";
    else if (S.PC != Sentinel)
      Problem = formatv("leaves to 0x{0:x-4}", S.PC).str();
    else if (S.SP != WantSP)
      Problem =
          formatv("returns with SP 0x{0:x-4}, expected 0x{1:x-4}", S.SP, WantSP)
              .str();
    else if (P.C == Cpu::Z80 && (S.IXH != IXH || S.IXL != IXL))
      Problem = "clobbers IX";
    if (!Problem.empty()) {
      ++R.Faults;
      report(Vals, Problem);
      return true;
    }

    // Pointer arguments see the buffers as they were for `requires` and as
    // the call left them for `ensures`.
    uint8_t After[MaxValues][BufSize];
    for (unsigned B = 0; B < NumBuf; ++B)
      std::memcpy(After[B], &M[BufBase + (1 + B) * BufSize], BufSize);
    auto PutParams = [&](uint8_t (*Slots)[16], uint8_t (*Bufs)[BufSize]) {
      unsigned B = 0;
      for (size_t I = 0; I < P.Params.size(); ++I) {
        if (P.Params[I] == Ty::Ptr) {
          void *Ptr = Bufs[B++];
          std::memcpy(Slots[I], &Ptr, sizeof Ptr);
        } else {
          std::memcpy(Slots[I], &Vals[I], 16);
        }
      }
    };

    if (P.Requires.Fn) {
      alignas(16) uint8_t Slots[MaxValues][16] = {};
      PutParams(Slots, HostBufs);
      if (!evaluate(P.Requires, Slots))
        return false;
    }
    ++R.Checked;

    U128 Result = 0;
    if (P.Ret)
      Result = L.SRet ? readMem(M, BufBase, sizeOf(*P.Ret))
                      : readRegs(S, L.Ret->Regs);
    for (const Check &E : P.Ensures) {
      alignas(16) uint8_t Slots[1 + MaxValues + 13][16] = {};
      unsigned N = 0;
      if (P.Ret)
        std::memcpy(Slots[N++], &Result, 16);
      PutParams(Slots + N, After);
      N += P.Params.size();
      for (Reg Rg : E.Cond->Regs) {
        U128 V = regValue(S, Rg);
        std::memcpy(Slots[N++], &V, 16);
      }
      if (evaluate(E, Slots))
        continue;

      std::string What = E.Where + ": ensures " + E.Cond->Text;
      if (P.Ret)
        What += "; result=" + formatValue(*P.Ret, Result);
      for (Reg Rg : E.Cond->Regs)
        What +=
            std::string(", ") + regName(Rg) + "=" +
            formatValue(regBytes(Rg) == 1 ? Ty::I8 : Ty::I16, regValue(S, Rg));
      ++R.Mismatches;
      report(Vals, What);
      return true;
    }
    return true;
  }
};

} // namespace

/// The layout of a call whose contract places its values.
static Expected<CallLayout> placeCall(const Contract &K, ArrayRef<Ty> Params,
                                      std::optional<Ty> Ret) {
  std::string Where = K.where();
  if (Params.size() != K.ParamPlaces.size())
    return createStringError("%s: the parameters do not match their places",
                             Where.c_str());
  auto Expand = [](const std::vector<Reg> &Regs) {
    std::vector<Reg8> Out;
    for (Reg R : Regs) {
      switch (R) {
      case Reg::A: Out.push_back(RA); break;
      case Reg::B: Out.push_back(RB); break;
      case Reg::C: Out.push_back(RC); break;
      case Reg::D: Out.push_back(RD); break;
      case Reg::E: Out.push_back(RE); break;
      case Reg::H: Out.push_back(RH); break;
      case Reg::L: Out.push_back(RL); break;
      case Reg::BC: Out.insert(Out.end(), {RB, RC}); break;
      case Reg::DE: Out.insert(Out.end(), {RD, RE}); break;
      case Reg::HL: Out.insert(Out.end(), {RH, RL}); break;
      default: break;
      }
    }
    return Out;
  };

  CallLayout L;
  std::set<Reg8> Used;
  for (size_t I = 0; I < Params.size(); ++I) {
    const Place &Pl = K.ParamPlaces[I];
    Loc Lc;
    Lc.Size = sizeOf(Params[I]);
    if (Pl.Stack) {
      Lc.Offset = *Pl.Stack;
      for (const Loc &O : L.Params)
        if (O.Regs.empty() && Lc.Offset < O.Offset + O.Size &&
            O.Offset < Lc.Offset + Lc.Size)
          return createStringError("%s: parameters overlap on the stack",
                                   Where.c_str());
      L.StackBytes = std::max(L.StackBytes, Lc.Offset + Lc.Size);
    } else {
      Lc.Regs = Expand(Pl.Regs);
      if (Lc.Regs.size() != Lc.Size)
        return createStringError("%s: parameter %zu takes %u bytes, but its "
                                 "registers hold %zu",
                                 Where.c_str(), I + 1, Lc.Size, Lc.Regs.size());
      for (Reg8 R : Lc.Regs)
        if (!Used.insert(R).second)
          return createStringError("%s: parameters share a register",
                                   Where.c_str());
    }
    L.Params.push_back(Lc);
  }
  if (Ret) {
    Loc Lc;
    Lc.Size = sizeOf(*Ret);
    Lc.Regs = Expand(K.RetPlace->Regs);
    if (Lc.Regs.size() != Lc.Size)
      return createStringError("%s: the result takes %u bytes, but its "
                               "registers hold %zu",
                               Where.c_str(), Lc.Size, Lc.Regs.size());
    L.Ret = Lc;
  }
  if (K.Pops > L.StackBytes)
    return createStringError("%s: __pops(%u) is more than the %u bytes of "
                             "stack arguments",
                             Where.c_str(), K.Pops, L.StackBytes);
  L.Popped = K.Pops;
  return L;
}

Expected<TestPlan> z80tester::planTest(Cpu C, const Image &Img,
                                       const Contract &K,
                                       const Signature &Sig) {
  TestPlan P;
  P.C = C;
  P.Name = K.Name;
  std::optional<uint16_t> Entry = Img.lookup(K.Name);
  if (!Entry)
    return createStringError("%s: no function %s in the image",
                             K.where().c_str(), K.Name.c_str());
  P.Entry = *Entry;
  if (Img.End > WindowLo)
    return createStringError("the image reaches into the stack at 0x%04x",
                             WindowLo);
  P.Params = Sig.Params;
  P.Ret = Sig.Ret;
  if (P.Params.size() > MaxValues)
    return createStringError("%s: too many parameters", K.where().c_str());
  P.Exhaustive = K.Exhaustive;
  P.Samples = K.Samples;
  if (P.Exhaustive && inputBits(P.Params) >= 64)
    return createStringError("%s: %u input bits are too many to try them all",
                             K.where().c_str(), inputBits(P.Params));
  auto Layout = K.Placed ? placeCall(K, P.Params, P.Ret)
                         : layoutCall(C, CallConv::SDCCCall1, P.Params, P.Ret);
  if (!Layout)
    return Layout.takeError();
  P.Layout = *Layout;
  return P;
}

TestResult z80tester::runTest(const TestPlan &P, const Image &Img,
                              const TestOptions &O) {
  std::signal(SIGILL, onTrap);
  unsigned Bits = inputBits(P.Params);
  bool Exhaustive = P.Exhaustive || (!P.Samples && Bits <= O.MaxExhaustiveBits);
  uint64_t Total =
      Exhaustive ? uint64_t(1) << Bits : P.Samples.value_or(O.Samples);

  std::vector<ExampleValues> Examples;
  for (const ExampleCheck &E : P.Examples) {
    ExampleValues &EV = Examples.emplace_back();
    EV.Where = E.Where;
    for (const auto &[I, Fn] : E.Values) {
      alignas(16) uint8_t Out[16] = {};
      Evaluating = E.Where.c_str();
      Fn(nullptr, Out);
      Evaluating = nullptr;
      U128 V;
      std::memcpy(&V, Out, 16);
      EV.Values.push_back({I, V & mask(sizeOf(P.Params[I]))});
    }
  }

  unsigned N = O.Threads ? O.Threads : std::thread::hardware_concurrency();
  N = unsigned(std::max<uint64_t>(1, std::min<uint64_t>(N, Total)));
  std::vector<std::unique_ptr<Worker>> Workers;
  std::vector<std::thread> Threads;
  for (unsigned I = 0; I < N; ++I) {
    Workers.push_back(std::make_unique<Worker>(P, Img, O, I));
    uint64_t Begin = Total * I / N, End = Total * (I + 1) / N;
    Threads.emplace_back([&, &W = *Workers.back(), I, Begin, End] {
      if (I == 0)
        W.runExamples(Examples);
      W.runRange(Begin, End, Exhaustive);
    });
  }
  for (std::thread &Th : Threads)
    Th.join();

  TestResult R;
  R.Exhaustive = Exhaustive;
  for (const auto &W : Workers) {
    R.Inputs += W->R.Inputs;
    R.Checked += W->R.Checked;
    R.Mismatches += W->R.Mismatches;
    R.Faults += W->R.Faults;
    R.MaxSteps = std::max(R.MaxSteps, W->R.MaxSteps);
    for (const std::string &S : W->R.Reports)
      if (R.Reports.size() < O.MaxReports)
        R.Reports.push_back(S);
  }
  return R;
}
