// Recovers each function's control flow by recursive descent.

#include "z80lift/CFG.h"

using namespace llvm;
using namespace z80core;
using namespace z80lift;

Expected<CFG> z80lift::recoverCFG(Cpu C, const Image &Img, uint16_t Entry) {
  CFG F;
  F.Entry = Entry;
  std::map<uint16_t, Inst> Insts;
  std::set<uint16_t> Leaders = {Entry};
  std::vector<uint16_t> Work = {Entry};

  auto Leaves = [&](uint16_t A) {
    if (A != Entry && Img.Entries.count(A)) {
      F.Callees.insert(A);
      return true;
    }
    return false;
  };
  auto Branch = [&](uint16_t A) {
    if (!Leaves(A) && Leaders.insert(A).second)
      Work.push_back(A);
  };

  while (!Work.empty()) {
    uint16_t A = Work.back();
    Work.pop_back();
    for (;;) {
      if (Insts.count(A))
        break;
      Inst I;
      if (!decode(C, Img.Mem.data(), A, I))
        return createStringError("%s: cannot decode 0x%04x",
                                 Img.nameAt(Entry).c_str(), A);
      Insts[A] = I;
      bool FallsThrough = true;
      switch (I.K) {
      case Kind::Seq: break;
      case Kind::Jump:
        Branch(I.Dest);
        FallsThrough = false;
        break;
      case Kind::CondJump:
        Branch(I.Dest);
        Leaders.insert(I.next());
        break;
      case Kind::Call:
      case Kind::CondCall:
        F.Callees.insert(I.Dest);
        Leaders.insert(I.next());
        break;
      case Kind::CondRet: Leaders.insert(I.next()); break;
      case Kind::Ret:
      case Kind::JumpInd:
      case Kind::Halt: FallsThrough = false; break;
      }
      if (!FallsThrough || Leaves(I.next()))
        break;
      A = I.next();
    }
  }

  // A jump into the middle of a decoded instruction would need two decodings
  // of the same bytes.
  for (auto It = Insts.begin(); It != Insts.end(); ++It) {
    auto Next = std::next(It);
    if (Next != Insts.end() && It->second.next() > Next->first &&
        It->second.next() > It->first)
      return createStringError("%s: overlapping instructions at 0x%04x",
                               Img.nameAt(Entry).c_str(), Next->first);
  }

  for (uint16_t L : Leaders) {
    if (!Insts.count(L))
      continue; // fall-through into another function
    Block B;
    B.Start = L;
    uint16_t A = L;
    for (;;) {
      const Inst &I = Insts.at(A);
      B.Insts.push_back(I);
      A = I.next();
      if (I.K != Kind::Seq || Leaders.count(A) || !Insts.count(A))
        break;
    }
    F.Blocks.emplace(L, std::move(B));
  }
  return F;
}
