// Calls a runtime function the way compiled code does and checks its contract.
// Registers that carry no argument start out random, so reading one shows up
// as a broken condition. Each input has its own random numbers, so results do
// not depend on how the inputs are split between threads.

#include "z80tester/Tester.h"
#include "z80core/Interp.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <array>
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

constexpr unsigned MaxValues = 8;
constexpr unsigned Margin = 16; // random bytes kept around each buffer

} // namespace

static uint64_t mix(uint64_t X) {
  X += 0x9E3779B97F4A7C15ULL;
  X = (X ^ (X >> 30)) * 0xBF58476D1CE4E5B9ULL;
  X = (X ^ (X >> 27)) * 0x94D049BB133111EBULL;
  return X ^ (X >> 31);
}

/// Stores a host pointer in a slot that a condition reads.
static void putPointer(uint8_t *Slot, const void *Ptr) {
  std::memcpy(Slot, static_cast<const void *>(&Ptr), sizeof Ptr);
}

namespace {

struct Rng {
  uint64_t S;
  uint64_t next() {
    S ^= S >> 12;
    S ^= S << 25;
    S ^= S >> 27;
    return S * 0x2545F4914F6CDD1DULL;
  }
  U128 next128() { return U128(next()) << 64 | next(); }
  uint64_t below(uint64_t N) { return N ? next() % N : 0; }
};

enum Stream : uint64_t { InputStream, ExampleStream, ArenaStream };

} // namespace

/// The random numbers of input K of a stream, whichever thread draws them.
static Rng rngFor(uint64_t Seed, Stream St, uint64_t K) {
  return Rng{mix(mix(Seed ^ mix(St)) + K) | 1};
}

static U128 mask(unsigned Bytes) {
  return Bytes >= 16 ? ~U128(0) : (U128(1) << (Bytes * 8)) - 1;
}

static uint8_t &reg(State &S, Reg8 R) {
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

static void writeRegs(State &S, const std::vector<Reg8> &Regs, U128 V) {
  for (size_t I = 0; I < Regs.size(); ++I)
    reg(S, Regs[Regs.size() - 1 - I]) = uint8_t(V >> (8 * I));
}

static U128 readRegs(State &S, const std::vector<Reg8> &Regs) {
  U128 V = 0;
  for (Reg8 R : Regs)
    V = V << 8 | reg(S, R);
  return V;
}

static void writeMem(uint8_t *M, uint16_t A, U128 V, unsigned N) {
  for (unsigned I = 0; I < N; ++I)
    M[uint16_t(A + I)] = uint8_t(V >> (8 * I));
}

static U128 readMem(const uint8_t *M, uint16_t A, unsigned N) {
  U128 V = 0;
  for (unsigned I = N; I-- > 0;)
    V = V << 8 | M[uint16_t(A + I)];
  return V;
}

static bool isFloat(Ty T) { return T == Ty::F32 || T == Ty::F16; }

static U128 signBit(Ty T) { return U128(1) << (sizeOf(T) * 8 - 1); }

static bool isNaN(Ty T, U128 V) {
  return T == Ty::F32 ? (V & 0x7FFFFFFF) > 0x7F800000 : (V & 0x7FFF) > 0x7C00;
}

/// Orders values: integers by value, floats by value with -0 just below +0.
static U128 toKey(Ty T, bool Signed, U128 V) {
  U128 S = signBit(T), M = mask(sizeOf(T));
  if (isFloat(T))
    return (V & S) ? (~V & M) : (V | S);
  return Signed ? V ^ S : V;
}

static U128 fromKey(Ty T, bool Signed, U128 K) {
  U128 S = signBit(T), M = mask(sizeOf(T));
  if (isFloat(T))
    return (K & S) ? (K & ~S) : (~K & M);
  return Signed ? K ^ S : K;
}

/// Inputs worth trying more often than random bits give them.
static std::vector<U128> specials(Ty T) {
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

static std::string formatValue(Ty T, U128 V) {
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

static U128 regValue(const State &S, Reg R) {
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

static std::string formatString(const std::string &Bytes) {
  std::string S = "\"";
  size_t N = Bytes.empty() ? 0 : Bytes.size() - 1; // without the NUL
  for (size_t I = 0; I < N && I < 24; ++I) {
    unsigned char C = Bytes[I];
    if (C >= 0x20 && C < 0x7F && C != '"' && C != '\\')
      S += char(C);
    else
      S += formatv("\\x{0:x-2}", C).str();
  }
  return S + (N > 24 ? "\"..." : "\"");
}

namespace {

// The part of a contract being evaluated, for reporting a trap in it.
thread_local const char *Evaluating = nullptr;

} // namespace

static void onSignal(int Sig) {
  const char *W = Evaluating;
  if (!W && Sig != SIGILL) {
    std::signal(Sig, SIG_DFL);
    return;
  }
  const char Head[] = "z80-test: error: ";
  const char *What = !W ? "illegal instruction"
                     : Sig == SIGILL
                         ? "undefined behaviour in the contract at "
                         : "invalid memory access in the contract at ";
  (void)!write(2, Head, sizeof Head - 1);
  (void)!write(2, What, strlen(What));
  if (W)
    (void)!write(2, W, strlen(W));
  (void)!write(2, "\n", 1);
  _exit(2);
}

/// Calls a compiled part of a contract, naming it if it traps.
static void call(AdapterFn Fn, const std::string &Where, const void *Slots,
                 void *Out) {
  Evaluating = Where.c_str();
  Fn(Slots, Out);
  Evaluating = nullptr;
}

static bool evaluate(const Check &C, const void *Slots) {
  alignas(16) uint8_t Out[16] = {};
  call(C.Fn, C.Where, Slots, Out);
  return Out[0];
}

static int64_t bound(AdapterFn Fn, const std::string &Where,
                     const void *Slots) {
  alignas(16) uint8_t Out[16] = {};
  call(Fn, Where, Slots, Out);
  int64_t V;
  std::memcpy(&V, Out, 8);
  return V;
}

/// The number of inputs there are, if it is below 2^64.
static std::optional<uint64_t> allInputs(const TestPlan &P) {
  U128 Total = 1;
  for (size_t I = 0; I < P.Params.size(); ++I) {
    const Draw &D = P.Draws[I];
    U128 N;
    if (D.K == Draw::Keys)
      N = D.Size;
    else if (D.K == Draw::Bits && sizeOf(P.Params[I]) < 16)
      N = U128(1) << (sizeOf(P.Params[I]) * 8);
    else if (D.K == Draw::Bits)
      return std::nullopt;
    else
      continue;
    if (N > (U128(1) << 64) || Total * N >= (U128(1) << 64))
      return std::nullopt;
    Total *= N;
  }
  return uint64_t(Total);
}

namespace {

/// One input: a value for each parameter, an address for a pointer, and the
/// bytes a pointer's buffer starts with, if any.
struct Input {
  U128 Vals[MaxValues] = {};
  std::array<std::optional<std::string>, MaxValues> Content;
};

class Worker {
public:
  Worker(const TestPlan &P, const Image &Img, const TestOptions &O)
      : P(P), O(O), Mem(Img.Mem) {
    HasPointers =
        llvm::any_of(P.Info, [](const ParamInfo &I) { return I.Pointer; });
    if (HasPointers) {
      Rng G = rngFor(O.Seed, ArenaStream, 0);
      fill(ArenaLo, ArenaHi, G);
      Arena.assign(Mem.begin() + ArenaLo, Mem.begin() + ArenaHi);
      Pre.resize(Mem.size());
    }
  }

  void runRange(uint64_t Begin, uint64_t End, bool Exhaustive) {
    for (uint64_t K = Begin; K < End; ++K) {
      Rng G = rngFor(O.Seed, InputStream, K);
      Input In;
      draw(In, G, !Exhaustive);
      if (Exhaustive)
        decode(In, K);
      runInput(In, G);
    }
  }

  /// Runs each example, drawing the values it leaves out again until
  /// `requires` holds.
  void runExamples(const std::vector<ExampleValues> &Examples) {
    for (size_t E = 0; E < Examples.size(); ++E) {
      const ExampleValues &Ex = Examples[E];
      bool Full = Ex.Values.size() + Ex.Strings.size() == P.Params.size();
      bool Met = false;
      Input In;
      for (unsigned T = 0; T < (Full ? 1 : 1000) && !Met; ++T) {
        Rng G = rngFor(O.Seed, ExampleStream, E * 1000 + T);
        In = Input();
        draw(In, G, true);
        for (const auto &[I, V] : Ex.Values)
          In.Vals[I] = V;
        for (const auto &[I, S] : Ex.Strings)
          In.Content[I] = S;
        Met = runInput(In, G);
      }
      if (!Met) {
        ++R.Mismatches;
        report(In, Ex.Where + ": example does not meet requires");
      }
    }
  }

  TestResult R;

private:
  const TestPlan &P;
  const TestOptions &O;
  std::vector<uint8_t> Mem;
  bool HasPointers = false;
  std::vector<uint8_t> Arena; // the arena's contents between calls
  std::vector<uint8_t> Pre;   // memory before the call

  uint8_t *preAt(uint16_t A) { return Pre.data() + A; }

  void fill(uint32_t Lo, uint32_t Hi, Rng &G) {
    for (uint32_t A = Lo; A < Hi; A += 8) {
      uint64_t V = G.next();
      std::memcpy(&Mem[A], &V, std::min<uint32_t>(8, Hi - A));
    }
  }

  void report(const Input &In, const std::string &What) {
    if (R.Reports.size() >= O.MaxReports)
      return;
    std::string S = P.Name + "(";
    for (size_t I = 0; I < P.Params.size(); ++I) {
      if (I)
        S += ", ";
      const std::optional<std::string> &Content = In.Content[I];
      if (P.Info[I].Pointer && Content)
        S += formatString(*Content);
      else
        S += formatValue(P.Params[I], In.Vals[I]);
    }
    R.Reports.push_back(S + "): " + What);
  }

  static uint8_t drawChar(Rng &G) {
    switch (G.below(8)) {
    case 0: return "\x01\x7F\x80\xFF"[G.below(4)];
    case 1: return 1 + G.below(255);
    case 2:
    case 3: return 0x20 + G.below(0x5F);
    default: return 'a' + G.below(2);
    }
  }

  /// A NUL-terminated string. It often starts like an earlier string
  /// argument, as comparisons need.
  std::string drawString(const Draw &D, size_t Param, const Input &In, Rng &G) {
    uint64_t Len = G.below(4) == 0 ? (G.below(2) ? D.LenLo : D.LenHi - 1)
                                   : D.LenLo + G.below(D.LenHi - D.LenLo);
    std::string S;
    const std::string *Earlier = nullptr;
    for (size_t J = 0; J < Param; ++J)
      if (const std::optional<std::string> &Content = In.Content[J];
          P.Draws[J].K == Draw::String && Content)
        Earlier = &*Content;
    if (Earlier && G.below(2)) {
      size_t Most = std::min<size_t>(Earlier->size() - 1, Len);
      S = Earlier->substr(0, G.below(2) ? Most : G.below(Most + 1));
    }
    while (S.size() < Len)
      S += char(drawChar(G));
    return S + '\0';
  }

  /// Random inputs, often special ones; strings and pointers either way.
  void draw(Input &In, Rng &G, bool Values) {
    for (size_t I = 0; I < P.Params.size(); ++I) {
      const Draw &D = P.Draws[I];
      Ty T = P.Params[I];
      bool Special = !D.Specials.empty() && G.below(4) == 0;
      switch (D.K) {
      case Draw::Bits:
        if (Values)
          In.Vals[I] = Special ? D.Specials[G.below(D.Specials.size())]
                               : G.next128() & mask(sizeOf(T));
        break;
      case Draw::Keys:
        if (Values)
          In.Vals[I] = Special ? D.Specials[G.below(D.Specials.size())]
                               : fromKey(T, P.Info[I].Signed,
                                         D.Lo + G.next128() % D.Size);
        break;
      case Draw::String: In.Content[I] = drawString(D, I, In, G); break;
      case Draw::Pointer: break;
      }
    }
  }

  /// The values of input K when trying them all.
  void decode(Input &In, uint64_t K) {
    for (size_t I = 0; I < P.Params.size(); ++I) {
      const Draw &D = P.Draws[I];
      unsigned Bits = sizeOf(P.Params[I]) * 8;
      if (D.K == Draw::Bits) {
        In.Vals[I] = K & mask(sizeOf(P.Params[I]));
        K = Bits >= 64 ? 0 : K >> Bits;
      } else if (D.K == Draw::Keys) {
        In.Vals[I] =
            fromKey(P.Params[I], P.Info[I].Signed, D.Lo + U128(K) % D.Size);
        K = uint64_t(U128(K) / D.Size);
      }
    }
  }

  /// Puts each argument in a slot; pointers point into memory as it is, or as
  /// it was before the call.
  void putParams(uint8_t (*Slots)[16], const Input &In, bool Before) {
    for (size_t I = 0; I < P.Params.size(); ++I) {
      if (!P.Info[I].Pointer) {
        std::memcpy(Slots[I], &In.Vals[I], 16);
        continue;
      }
      uint16_t A = uint16_t(In.Vals[I]);
      putPointer(Slots[I], Before ? preAt(A) : &Mem[A]);
    }
  }

  /// Picks an address for each pointer and fills its buffer; false if the
  /// buffers do not fit. Sizes come from the strings, the pointed-to type, and
  /// the ranges whose bounds read only strings.
  bool place(Input &In, Rng &G) {
    int64_t Lo[MaxValues] = {}, Hi[MaxValues] = {};
    for (size_t I = 0; I < P.Params.size(); ++I)
      if (const std::optional<std::string> &Content = In.Content[I];
          P.Info[I].Pointer)
        Hi[I] = int64_t(Content ? Content->size() : P.Info[I].Pointee);
    if (llvm::any_of(P.Ranges, [](const RangeCheck &R) { return R.Sizes; })) {
      alignas(16) uint8_t Slots[MaxValues][16] = {};
      for (size_t I = 0; I < P.Params.size(); ++I) {
        if (!P.Info[I].Pointer) {
          std::memcpy(Slots[I], &In.Vals[I], 16);
          continue;
        }
        const std::optional<std::string> &Content = In.Content[I];
        putPointer(Slots[I], Content ? Content->data() : nullptr);
      }
      for (const RangeCheck &Rg : P.Ranges) {
        if (!Rg.Sizes)
          continue;
        int64_t L = bound(Rg.Lo, Rg.Where, Slots);
        int64_t H = bound(Rg.Hi, Rg.Where, Slots);
        if (H > L) {
          Lo[Rg.Param] = std::min(Lo[Rg.Param], L);
          Hi[Rg.Param] = std::max(Hi[Rg.Param], H);
        }
      }
    }

    // Half the time a buffer goes next to or over an earlier one.
    struct Spot {
      int64_t Start, Size;
    };
    std::vector<Spot> Spots;
    const int64_t First = ArenaLo + Margin,
                  Room = ArenaHi - ArenaLo - 2 * Margin;
    for (size_t I = 0; I < P.Params.size(); ++I) {
      if (!P.Info[I].Pointer)
        continue;
      int64_t Size = Hi[I] - Lo[I];
      if (Size > Room)
        return false;
      int64_t Start = First + int64_t(G.below(Room - Size + 1));
      if (!Spots.empty() && G.below(2)) {
        const Spot &Q = Spots[G.below(Spots.size())];
        int64_t Spread = std::max(Size, Q.Size) + 4;
        int64_t Near = Q.Start + int64_t(G.below(2 * Spread + 1)) - Spread;
        if (Near >= First && Near + Size <= First + Room)
          Start = Near;
      }
      Spots.push_back({Start, Size});
      In.Vals[I] = uint16_t(Start - Lo[I]);
    }
    for (const Spot &S : Spots)
      fill(S.Start - Margin, S.Start + S.Size + Margin, G);
    for (size_t I = 0; I < P.Params.size(); ++I)
      if (const std::optional<std::string> &Content = In.Content[I];
          P.Info[I].Pointer && Content)
        std::memcpy(&Mem[uint16_t(In.Vals[I])], Content->data(),
                    Content->size());
    return true;
  }

  void randomize(State &S, Rng &G) {
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
    fill(StackTop - 256, StackTop, G);
  }

  /// The first byte of the arena the call changed outside `modifies`, as a
  /// report, or nothing.
  std::string strayWrite(const Input &In) {
    std::vector<std::pair<int64_t, int64_t>> Allowed;
    alignas(16) uint8_t Slots[MaxValues][16] = {};
    putParams(Slots, In, true);
    for (const RangeCheck &Rg : P.Ranges) {
      int64_t A = uint16_t(In.Vals[Rg.Param]);
      Allowed.push_back({A + bound(Rg.Lo, Rg.Where, Slots),
                         A + bound(Rg.Hi, Rg.Where, Slots)});
    }
    for (uint32_t A = ArenaLo; A < ArenaHi; A += 64) {
      if (!std::memcmp(&Mem[A], preAt(A), 64))
        continue;
      for (uint32_t B = A; B < A + 64; ++B)
        if (Mem[B] != *preAt(B) && llvm::none_of(Allowed, [&](const auto &R) {
              return R.first <= B && B < R.second;
            }))
          return formatv("writes to 0x{0:x-4}, which modifies does not cover",
                         B)
              .str();
    }
    return "";
  }

  /// Draws what the input leaves to chance and runs it; false if it does not
  /// meet `requires`.
  bool runInput(Input &In, Rng &G) {
    ++R.Inputs;
    if (HasPointers && !place(In, G)) {
      ++R.Unplaced;
      return false;
    }
    bool Met = callOnce(In, G);
    if (HasPointers)
      std::memcpy(&Mem[ArenaLo], Arena.data(), Arena.size());
    return Met;
  }

  bool callOnce(const Input &In, Rng &G) {
    State S{};
    randomize(S, G);
    uint8_t *M = Mem.data();
    const CallLayout &L = P.Layout;

    uint16_t Base = StackTop - L.StackBytes;
    for (size_t I = 0; I < P.Params.size(); ++I) {
      const Loc &Lc = L.Params[I];
      if (!Lc.Regs.empty())
        writeRegs(S, Lc.Regs, In.Vals[I]);
      else
        writeMem(M, Base + Lc.Offset, In.Vals[I], Lc.Size);
    }
    if (L.SRet)
      writeMem(M, Base, SRetBuf, 2);
    S.SP = Base - 2;
    writeMem(M, S.SP, Sentinel, 2);
    if (HasPointers)
      std::memcpy(Pre.data(), M, Pre.size());

    if (P.Requires.Fn) {
      alignas(16) uint8_t Slots[MaxValues][16] = {};
      putParams(Slots, In, true);
      if (!evaluate(P.Requires, Slots))
        return false;
    }

    const State Before = S;
    S.PC = P.Entry;
    S.StepLimit = O.StepLimit;
    S.WriteLo = HasPointers ? ArenaLo : StackLo;
    S.WriteHi = WindowHi;
    bool Decoded = true;
    if (P.Fn)
      P.Fn(&S, M);
    else
      Decoded = run(P.C, S, M, Sentinel);
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
    else if (P.C == Cpu::Z80 && !P.Placed &&
             (S.IXH != Before.IXH || S.IXL != Before.IXL))
      Problem = "clobbers IX";
    if (!Problem.empty()) {
      ++R.Faults;
      report(In, Problem);
      return true;
    }
    if (HasPointers) {
      std::string Stray = strayWrite(In);
      if (!Stray.empty()) {
        ++R.Mismatches;
        report(In, Stray);
        return true;
      }
    }
    ++R.Checked;

    U128 Result = 0;
    if (const std::optional<Ty> &Ret = P.Ret)
      Result =
          L.Ret ? readRegs(S, L.Ret->Regs) : readMem(M, SRetBuf, sizeOf(*Ret));
    for (const Check &E : P.Ensures) {
      // The result, the arguments, the registers, and for old() the pointers
      // into memory as it was and the registers as they were.
      alignas(16) uint8_t Slots[1 + 2 * MaxValues + 2 * 13][16] = {};
      unsigned N = 0;
      if (P.Ret == Ty::Ptr) {
        putPointer(Slots[N++], Result ? M + uint16_t(Result) : nullptr);
      } else if (P.Ret) {
        std::memcpy(Slots[N++], &Result, 16);
      }
      putParams(Slots + N, In, false);
      N += P.Params.size();
      for (Reg Rg : E.Cond->Regs) {
        U128 V = regValue(S, Rg);
        std::memcpy(Slots[N++], &V, 16);
      }
      if (E.Cond->UsesOld) {
        for (size_t I = 0; I < P.Params.size(); ++I) {
          if (!P.Info[I].Pointer)
            continue;
          putPointer(Slots[N++], preAt(uint16_t(In.Vals[I])));
        }
        for (Reg Rg : E.Cond->OldRegs) {
          U128 V = regValue(Before, Rg);
          std::memcpy(Slots[N++], &V, 16);
        }
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
      report(In, What);
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
    if (!K.RetPlace)
      return createStringError("%s: the result has no place", Where.c_str());
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
  P.Where = K.where();
  std::optional<uint16_t> Entry = Img.lookup(K.Name);
  if (!Entry)
    return createStringError("%s: no function %s in the image",
                             K.where().c_str(), K.Name.c_str());
  P.Entry = *Entry;
  if (Img.End > ArenaLo)
    return createStringError("the image reaches 0x%04x, where the tests keep "
                             "their memory",
                             ArenaLo);
  P.Params = Sig.Params;
  P.Ret = Sig.Ret;
  P.Info = K.ParamList;
  if (P.Params.size() > MaxValues)
    return createStringError("%s: too many parameters", K.where().c_str());
  if (P.Params.size() != P.Info.size())
    return createStringError("%s: the prototype's parameters do not compile "
                             "one to one",
                             K.where().c_str());
  P.Exhaustive = K.Exhaustive;
  P.Samples = K.Samples;
  P.OnlyExamples = K.Prove && !K.Samples;
  P.Placed = K.Placed;

  auto IsString = [&](unsigned I) {
    return llvm::any_of(
        K.Domains, [&](const Domain &D) { return D.Param == I && D.String; });
  };
  for (const Range &R : K.Ranges)
    P.Ranges.push_back(
        {R.Param, llvm::all_of(R.Pointers, IsString), R.where()});
  for (const Domain &D : K.Domains)
    P.Domains.push_back({D.Param, D.String, D.where()});

  auto Layout = K.Placed ? placeCall(K, P.Params, P.Ret)
                         : layoutCall(C, CallConv::SDCCCall1, P.Params, P.Ret);
  if (!Layout)
    return Layout.takeError();
  P.Layout = *Layout;
  return P;
}

Error z80tester::finishPlan(TestPlan &P) {
  P.Draws.assign(P.Params.size(), Draw());
  for (size_t I = 0; I < P.Params.size(); ++I) {
    if (P.Info[I].Pointer)
      P.Draws[I].K = Draw::Pointer;
    else
      P.Draws[I].Specials = specials(P.Params[I]);
  }

  for (const DomainCheck &Dm : P.Domains) {
    Draw &D = P.Draws[Dm.Param];
    Ty T = P.Params[Dm.Param];
    bool Signed = P.Info[Dm.Param].Signed;
    const char *Where = Dm.Where.c_str();
    alignas(16) uint8_t Lo[16] = {}, Hi[16] = {};
    call(Dm.Lo, Dm.Where, nullptr, Lo);
    call(Dm.Hi, Dm.Where, nullptr, Hi);

    if (Dm.String) {
      int64_t L, H;
      std::memcpy(&L, Lo, 8);
      std::memcpy(&H, Hi, 8);
      if (L < 0 || H <= L || H > 4096)
        return createStringError("%s: string lengths must be a non-empty "
                                 "range within 0 .. 4096",
                                 Where);
      D.K = Draw::String;
      D.LenLo = L;
      D.LenHi = H;
      continue;
    }

    U128 KLo, KHi;
    if (isFloat(T)) {
      U128 L = 0, H = 0;
      std::memcpy(&L, Lo, sizeOf(T));
      std::memcpy(&H, Hi, sizeOf(T));
      if (isNaN(T, L) || isNaN(T, H))
        return createStringError("%s: NaN cannot bound a range", Where);
      KLo = toKey(T, false, L);
      KHi = toKey(T, false, H);
    } else {
      __int128 L, H;
      std::memcpy(&L, Lo, 16);
      std::memcpy(&H, Hi, 16);
      unsigned Bits = sizeOf(T) * 8;
      if (Bits < 128) {
        __int128 Min = Signed ? -(__int128(1) << (Bits - 1)) : 0;
        __int128 Max = Signed ? __int128(1) << (Bits - 1) : __int128(1) << Bits;
        if (L < Min || H > Max)
          return createStringError("%s: the range does not fit the type",
                                   Where);
      }
      KLo = toKey(T, Signed, U128(L) & mask(sizeOf(T)));
      KHi = H > L ? KLo + U128(H - L) : KLo;
    }
    if (KHi <= KLo)
      return createStringError("%s: the range is empty", Where);
    D.K = Draw::Keys;
    D.Lo = KLo;
    D.Size = KHi - KLo;
    // The type's special values that fall in the range, and the range's ends.
    std::vector<U128> Specials;
    for (U128 V : specials(T))
      if (toKey(T, Signed, V) - KLo < D.Size)
        Specials.push_back(V);
    for (U128 Key : {KLo, KLo + 1, KHi - 1, KHi - 2})
      if (Key - KLo < D.Size)
        Specials.push_back(fromKey(T, Signed, Key));
    D.Specials = Specials;
  }

  if (P.Exhaustive && !allInputs(P))
    return createStringError("%s: there are too many inputs to try them all",
                             P.Where.c_str());

  for (const ExampleCheck &E : P.Examples) {
    ExampleValues &EV = P.ExampleInputs.emplace_back();
    EV.Where = E.Where;
    for (const auto &[I, Fn] : E.Values) {
      alignas(16) uint8_t Out[16] = {};
      call(Fn, E.Where, nullptr, Out);
      if (P.Info[I].Pointer) {
        const char *Str;
        std::memcpy(static_cast<void *>(&Str), Out, sizeof Str);
        EV.Strings.push_back({I, std::string(Str ? Str : "") + '\0'});
      } else {
        U128 V;
        std::memcpy(&V, Out, 16);
        EV.Values.push_back({I, V & mask(sizeOf(P.Params[I]))});
      }
    }
  }
  return Error::success();
}

TestResult z80tester::runTest(const TestPlan &P, const Image &Img,
                              const TestOptions &O) {
  for (int Sig : {SIGILL, SIGSEGV, SIGBUS})
    std::signal(Sig, onSignal);
  std::optional<uint64_t> All = allInputs(P);
  bool Exhaustive =
      All && (P.Exhaustive ||
              (!P.Samples && (O.MaxExhaustiveBits >= 64 ||
                              *All <= uint64_t(1) << O.MaxExhaustiveBits)));
  uint64_t Total = P.OnlyExamples ? 0
                   : Exhaustive   ? *All
                                  : P.Samples.value_or(O.Samples);

  unsigned N = O.Threads ? O.Threads : std::thread::hardware_concurrency();
  N = unsigned(std::max<uint64_t>(1, std::min<uint64_t>(N, Total)));
  std::vector<std::unique_ptr<Worker>> Workers;
  std::vector<std::thread> Threads;
  for (unsigned I = 0; I < N; ++I) {
    Workers.push_back(std::make_unique<Worker>(P, Img, O));
    uint64_t Begin = uint64_t(U128(Total) * I / N);
    uint64_t End = uint64_t(U128(Total) * (I + 1) / N);
    Threads.emplace_back([&, &W = *Workers.back(), I, Begin, End] {
      if (I == 0)
        W.runExamples(P.ExampleInputs);
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
    R.Unplaced += W->R.Unplaced;
    R.Mismatches += W->R.Mismatches;
    R.Faults += W->R.Faults;
    R.MaxSteps = std::max(R.MaxSteps, W->R.MaxSteps);
    for (const std::string &S : W->R.Reports)
      if (R.Reports.size() < O.MaxReports)
        R.Reports.push_back(S);
  }
  return R;
}
