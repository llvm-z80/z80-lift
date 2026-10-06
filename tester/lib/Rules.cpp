// Reads rule files.

#include "z80tester/Rules.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

const char *const KeptNames[] = {"A",  "B",  "C",  "D",   "E",  "H",  "L",
                                 "BC", "DE", "HL", "IX",  "IY", "SP", "F",
                                 "SF", "ZF", "HF", "PVF", "NF", "CF"};

// Names the assembler takes for registers, not symbols.
const char *const RegisterNames[] = {
    "a",  "b",  "c",  "d",  "e",  "h",  "l",   "i",   "r",   "af",
    "bc", "de", "hl", "sp", "ix", "iy", "ixh", "ixl", "iyh", "iyl"};

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

namespace {

/// Reads an assumption: an integer expression with C's operators and
/// precedence, over numbers and names.
class ExprParser {
public:
  explicit ExprParser(StringRef Text) : Rest(Text) {}

  Expected<std::shared_ptr<const Expr>> parse() {
    auto E = binary(0);
    if (!E)
      return E;
    if (!Rest.ltrim().empty())
      return error("unexpected '" + Rest.ltrim().str() + "'");
    return E;
  }

private:
  StringRef Rest;

  // From the loosest binding to the tightest.
  static constexpr std::array<std::initializer_list<StringRef>, 10> Levels = {{
      {"||"},
      {"&&"},
      {"|"},
      {"^"},
      {"&"},
      {"==", "!="},
      {"<=", ">=", "<", ">"},
      {"<<", ">>"},
      {"+", "-"},
      {"*", "/", "%"},
  }};

  static Error error(const Twine &Msg) {
    return createStringError("%s", Msg.str().c_str());
  }

  // The longest operator that starts Rest.
  StringRef peekOp() {
    Rest = Rest.ltrim();
    for (StringRef Op :
         {"||", "&&", "==", "!=", "<=", ">=", "<<", ">>", "|", "^", "&",
          "<",  ">",  "+",  "-",  "*",  "/",  "%",  "!",  "~", "(", ")"})
      if (Rest.starts_with(Op))
        return Op;
    return "";
  }

  Expected<std::shared_ptr<const Expr>> binary(unsigned Level) {
    if (Level == Levels.size())
      return unary();
    auto First = binary(Level + 1);
    if (!First)
      return First;
    std::shared_ptr<const Expr> L = *First;
    for (;;) {
      StringRef Op = peekOp();
      if (!is_contained(Levels[Level], Op))
        return L;
      Rest = Rest.drop_front(Op.size());
      auto R = binary(Level + 1);
      if (!R)
        return R;
      auto E = std::make_shared<Expr>();
      E->K = Expr::Binary;
      E->Name = Op.str();
      E->L = L;
      E->R = *R;
      L = E;
    }
  }

  Expected<std::shared_ptr<const Expr>> unary() {
    StringRef Op = peekOp();
    if (Op == "-" || Op == "!" || Op == "~") {
      Rest = Rest.drop_front();
      auto V = unary();
      if (!V)
        return V;
      auto E = std::make_shared<Expr>();
      E->K = Expr::Unary;
      E->Name = Op.str();
      E->L = *V;
      return E;
    }
    if (Op == "(") {
      Rest = Rest.drop_front();
      auto V = binary(0);
      if (!V)
        return V;
      if (peekOp() != ")")
        return error("expected ')'");
      Rest = Rest.drop_front();
      return V;
    }
    if (Rest.empty())
      return error("expected a number or a constant");
    auto E = std::make_shared<Expr>();
    if (isDigit(Rest.front())) {
      StringRef Num = Rest.take_while(isAlnum);
      Rest = Rest.drop_front(Num.size());
      if (Num.getAsInteger(0, E->Val))
        return error("'" + Num + "' is not a number");
      return E;
    }
    StringRef Name =
        Rest.take_while([](char Ch) { return isAlnum(Ch) || Ch == '_'; });
    if (Name.empty() || isDigit(Name.front()))
      return error("unexpected '" + Rest.str() + "'");
    Rest = Rest.drop_front(Name.size());
    E->K = Expr::Const;
    E->Name = Name.str();
    return E;
  }
};

} // namespace

/// Checks that E names only constants of R.
static Error checkNames(const Expr &E, const Rule &R) {
  if (E.K == Expr::Const &&
      none_of(R.Consts, [&](const RuleConst &C) { return C.Name == E.Name; }))
    return createStringError("%s: rule %s assumes something of %s, which is "
                             "not one of its constants",
                             R.where().c_str(), R.Name.c_str(), E.Name.c_str());
  for (const auto &Sub : {E.L, E.R})
    if (Sub)
      if (Error Err = checkNames(*Sub, R))
        return Err;
  return Error::success();
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
    if (Text.consume_front("const:")) {
      SmallVector<StringRef> Words;
      SplitString(Text, Words);
      auto Bad = [&] {
        return createStringError("%s: expected 'const: <name>' or 'const: "
                                 "<name> <min>..<max>'",
                                 Where.c_str());
      };
      auto NameChar = [](char Ch) { return isAlnum(Ch) || Ch == '_'; };
      if (Words.empty() || Words.size() > 2 || isDigit(Words[0].front()) ||
          !all_of(Words[0], NameChar))
        return Bad();
      RuleConst K;
      K.Name = Words[0].str();
      if (is_contained(RegisterNames, Words[0].lower()))
        return createStringError("%s: %s is a register, not a name for a "
                                 "constant",
                                 Where.c_str(), K.Name.c_str());
      if (Words.size() == 2) {
        auto [Min, Max] = Words[1].split("..");
        if (Min.getAsInteger(0, K.Min) || Max.getAsInteger(0, K.Max))
          return Bad();
        if (K.Min > K.Max || K.Min < INT32_MIN || K.Max > INT32_MAX)
          return createStringError("%s: the range of %s is empty or wider "
                                   "than 32 bits",
                                   Where.c_str(), K.Name.c_str());
      }
      if (any_of(R.Consts,
                 [&](const RuleConst &C) { return C.Name == K.Name; }))
        return createStringError("%s: second constant named %s", Where.c_str(),
                                 K.Name.c_str());
      R.Consts.push_back(K);
      Side = nullptr;
      continue;
    }
    if (Text.consume_front("assume:")) {
      auto E = ExprParser(Text).parse();
      if (!E)
        return createStringError("%s: %s", Where.c_str(),
                                 toString(E.takeError()).c_str());
      R.Assumes.push_back(*E);
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
    for (const auto &E : R.Assumes)
      if (Error Err = checkNames(*E, R))
        return std::move(Err);
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
