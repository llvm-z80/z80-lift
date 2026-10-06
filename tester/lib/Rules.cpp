// Reads rule files.

#include "z80tester/Rules.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <set>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

const char *const KeptNames[] = {"A",  "B",  "C",  "D",   "E",  "H",  "L",
                                 "BC", "DE", "HL", "IX",  "IY", "SP", "F",
                                 "SF", "ZF", "HF", "PVF", "NF", "CF"};

} // namespace

static std::optional<Kept> keptNamed(StringRef Name) {
  for (unsigned I = 0; I < std::size(KeptNames); ++I)
    if (Name == KeptNames[I])
      return Kept(I);
  return std::nullopt;
}

static bool onZ80Only(Kept K) {
  return K == Kept::IX || K == Kept::IY || K == Kept::SF || K == Kept::PVF;
}

const char *z80tester::keptName(Kept K) { return KeptNames[unsigned(K)]; }

unsigned z80tester::keptBits(Kept K) {
  if (K <= Kept::L || K == Kept::F)
    return 8;
  if (K <= Kept::SP)
    return 16;
  return 1;
}

std::string Rule::where() const {
  return (sys::path::filename(File) + ":" + Twine(Line)).str();
}

Expected<std::vector<Rule>> z80tester::loadRules(StringRef Path, Cpu C) {
  auto Buf = MemoryBuffer::getFile(Path, /*IsText=*/true);
  if (!Buf)
    return createStringError(Buf.getError(), "%s: %s", Path.str().c_str(),
                             Buf.getError().message().c_str());
  std::vector<Rule> Rules;
  std::set<std::string> Names;
  std::vector<std::pair<std::string, unsigned>> *Side = nullptr;
  SmallVector<StringRef> Lines;
  (*Buf)->getBuffer().split(Lines, '\n');
  for (unsigned N = 1; N <= Lines.size(); ++N) {
    StringRef Line = Lines[N - 1];
    Line = Line.take_front(Line.find("//")).rtrim();
    if (Line.trim().empty())
      continue;
    std::string Where = (sys::path::filename(Path) + ":" + Twine(N)).str();

    if (!isSpace(Line.front())) {
      StringRef Name;
      if (Line.size() > 4 && Line.starts_with("rule") && isSpace(Line[4]))
        Name = Line.drop_front(4).trim();
      if (Name.empty())
        return createStringError("%s: expected 'rule <name>'", Where.c_str());
      if (!Names.insert(Name.str()).second)
        return createStringError("%s: second rule named %s", Where.c_str(),
                                 Name.str().c_str());
      Rule &R = Rules.emplace_back();
      R.Name = Name.str();
      R.File = Path.str();
      R.Line = N;
      Side = nullptr;
      continue;
    }
    if (Rules.empty())
      return createStringError("%s: expected 'rule <name>'", Where.c_str());
    Rule &R = Rules.back();

    StringRef Text = Line.trim();
    if (Text.consume_front("keep:")) {
      SmallVector<StringRef> Words;
      SplitString(Text, Words, " \t,");
      for (StringRef W : Words) {
        std::optional<Kept> K = keptNamed(W);
        if (!K)
          return createStringError("%s: '%s' is not a register or flag; use A "
                                   "to L, BC, DE, HL, IX, IY, SP, F or a flag "
                                   "SF, ZF, HF, PVF, NF, CF",
                                   Where.c_str(), W.str().c_str());
        if (C == Cpu::SM83 && onZ80Only(*K))
          return createStringError("%s: the SM83 has no %s", Where.c_str(),
                                   W.str().c_str());
        R.Keep.push_back(*K);
      }
      Side = nullptr;
      continue;
    }
    if (Text.consume_front("memory:")) {
      SmallVector<StringRef> Words;
      SplitString(Text, Words);
      if (Words.size() == 1 && Words[0] == "all")
        R.AboveSP = false;
      else if (Words.size() == 2 && Words[0] == "above" && Words[1] == "SP")
        R.AboveSP = true;
      else
        return createStringError("%s: expected 'memory: all' or 'memory: "
                                 "above SP'",
                                 Where.c_str());
      Side = nullptr;
      continue;
    }
    bool Before = Text.consume_front("before:");
    if (Before || Text.consume_front("after:")) {
      Side = Before ? &R.Before : &R.After;
      if (!Side->empty())
        return createStringError("%s: second '%s' in rule %s", Where.c_str(),
                                 Before ? "before:" : "after:", R.Name.c_str());
      if (!Text.trim().empty())
        Side->push_back({Text.trim().str(), N});
      continue;
    }
    if (!Side)
      return createStringError("%s: an instruction outside 'before:' and "
                               "'after:'",
                               Where.c_str());
    Side->push_back({Text.str(), N});
  }

  for (const Rule &R : Rules) {
    if (R.Before.empty())
      return createStringError("%s: rule %s has no 'before:' code",
                               R.where().c_str(), R.Name.c_str());
    // Below SP means below the same SP on both sides.
    if (R.AboveSP && !is_contained(R.Keep, Kept::SP))
      return createStringError("%s: rule %s compares memory above SP, so it "
                               "must keep SP",
                               R.where().c_str(), R.Name.c_str());
  }
  return Rules;
}

std::string z80tester::ruleLabel(size_t I, bool After) {
  return formatv("z80test_rule_{0}_{1}", I, After ? "after" : "before").str();
}

std::string z80tester::rulesSource(ArrayRef<Rule> Rules) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "\t.area _CODE\n";
  for (size_t I = 0; I < Rules.size(); ++I)
    for (bool After : {false, true})
      OS << "\t.globl _" << ruleLabel(I, After) << '\n';
  for (size_t I = 0; I < Rules.size(); ++I)
    for (bool After : {false, true}) {
      OS << '_' << ruleLabel(I, After) << ":\n";
      // A line marker makes the assembler report errors in the rule file.
      for (const auto &[Text, Line] : After ? Rules[I].After : Rules[I].Before)
        OS << "# " << Line << " \"" << Rules[I].File << "\"\n\t" << Text
           << '\n';
      OS << "\thalt\n";
    }
  return S;
}
