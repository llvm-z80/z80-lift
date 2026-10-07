// Reads rule files and turns a rule into its cases.

#include "z80tester/Rules.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

const StateField Z80Fields[] = {
    {"A", 8},  {"B", 8},   {"C", 8},   {"D", 8},   {"E", 8},    {"H", 8},
    {"L", 8},  {"IXH", 8}, {"IXL", 8}, {"IYH", 8}, {"IYL", 8},  {"I", 8},
    {"R", 8},  {"SP", 16}, {"SF", 1},  {"ZF", 1},  {"HF", 1},   {"PVF", 1},
    {"NF", 1}, {"CF", 1},  {"A2", 8},  {"F2", 8},  {"B2", 8},   {"C2", 8},
    {"D2", 8}, {"E2", 8},  {"H2", 8},  {"L2", 8},  {"IFF1", 1}, {"IFF2", 1},
    {"IM", 8}};

const StateField SM83Fields[] = {{"A", 8},   {"B", 8},  {"C", 8},  {"D", 8},
                                 {"E", 8},   {"H", 8},  {"L", 8},  {"SP", 16},
                                 {"ZF", 1},  {"HF", 1}, {"NF", 1}, {"CF", 1},
                                 {"IFF1", 1}};

// Names the assembler takes for registers and conditions, not symbols.
const char *const RegisterNames[] = {
    "a",  "b",  "c",  "d",  "e",  "h",  "l",   "i",   "r",   "f",  "af",
    "bc", "de", "hl", "sp", "ix", "iy", "ixh", "ixl", "iyh", "iyl"};
const char *const ConditionNames[] = {"z", "nz", "nc", "po", "pe", "p", "m"};

const char *const Functions[] = {"overlaps", "isreg"};

} // namespace

static bool isRegisterName(StringRef N) {
  return is_contained(RegisterNames, N.lower()) ||
         is_contained(ConditionNames, N.lower());
}

/// The fields a name of the state stands for: one field, a pair, F or AF.
static SmallVector<StringRef, 6> fieldsNamed(StringRef N) {
  static const std::map<std::string, SmallVector<StringRef, 6>> Groups = {
      {"BC", {"B", "C"}},
      {"DE", {"D", "E"}},
      {"HL", {"H", "L"}},
      {"IX", {"IXH", "IXL"}},
      {"IY", {"IYH", "IYL"}},
      {"F", {"SF", "ZF", "HF", "PVF", "NF", "CF"}},
      {"AF", {"A", "SF", "ZF", "HF", "PVF", "NF", "CF"}}};
  if (auto It = Groups.find(N.str()); It != Groups.end())
    return It->second;
  for (const StateField &F : Z80Fields)
    if (N == F.Name)
      return {F.Name};
  return {};
}

/// The fields a register operand stands for, if it is one.
static std::optional<SmallVector<StringRef, 6>> registerFields(StringRef Op) {
  std::string L = Op.trim().lower();
  if (!is_contained(RegisterNames, L))
    return std::nullopt;
  return fieldsNamed(StringRef(L).upper());
}

/// The fields of the registers an operand reads or writes, or addresses
/// memory through.
static SmallVector<StringRef, 6> operandFields(StringRef Op) {
  std::string L = Op.lower();
  erase(L, ' ');
  if (auto R = registerFields(L))
    return *R;
  StringRef S(L);
  for (StringRef Index : {"ix", "iy"})
    if (S.contains("(" + Index.str()))
      return fieldsNamed(Index.upper());
  if (S.consume_front("(") && S.consume_back(")")) {
    S.consume_back("+");
    S.consume_back("-");
    if (auto R = registerFields(S))
      return *R;
  }
  return {};
}

// --- Expressions --------------------------------------------------------

namespace {

/// Reads an expression: C's operators and precedence over numbers, names,
/// operands in quotes and calls of the functions.
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
         {"||", "&&", "==", "!=", "<=", ">=", "<<", ">>", "|", "^", "&", "<",
          ">",  "+",  "-",  "*",  "/",  "%",  "!",  "~",  "(", ")", ","})
      if (Rest.starts_with(Op))
        return Op;
    return "";
  }

  static std::shared_ptr<Expr> node(Expr::Kind K, StringRef Name) {
    auto E = std::make_shared<Expr>();
    E->K = K;
    E->Name = Name.str();
    return E;
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
      auto E = node(Expr::Binary, Op);
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
      auto E = node(Expr::Unary, Op);
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
      return error("expected a number, a name or an operand");
    if (Rest.front() == '\'') {
      size_t End = Rest.find('\'', 1);
      if (End == StringRef::npos)
        return error("an operand in quotes that does not end");
      auto E = node(Expr::Text, Rest.slice(1, End).trim());
      Rest = Rest.drop_front(End + 1);
      return E;
    }
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
    if (Name.empty())
      return error("unexpected '" + Rest.str() + "'");
    Rest = Rest.drop_front(Name.size());
    if (peekOp() != "(") {
      E->K = Expr::Id;
      E->Name = Name.str();
      return E;
    }
    if (!is_contained(Functions, Name))
      return error("no function " + Name);
    Rest = Rest.drop_front();
    E->K = Expr::Call;
    E->Name = Name.str();
    for (auto *Arg : {&E->L, &E->R}) {
      auto V = binary(0);
      if (!V)
        return V.takeError();
      *Arg = *V;
      if (peekOp() == ")")
        break;
      if (peekOp() != ",")
        return error("expected ',' or ')'");
      Rest = Rest.drop_front();
    }
    if (peekOp() != ")")
      return error("expected ')'");
    Rest = Rest.drop_front();
    return E;
  }
};

/// What an expression is once the operand variables are chosen: an
/// operand, a number, or an expression left to assume.
struct Val {
  enum Kind { Operand, Number, Left } K = Number;
  std::string Text;
  int64_t N = 0;
  std::shared_ptr<const Expr> E;

  static Val operand(std::string T) { return {Operand, std::move(T), 0, {}}; }
  static Val number(int64_t V) { return {Number, "", V, {}}; }
  static Val left(std::shared_ptr<const Expr> E) {
    return {Left, "", 0, std::move(E)};
  }
  std::shared_ptr<const Expr> expr() const {
    if (K == Left)
      return E;
    auto X = std::make_shared<Expr>();
    X->Val = N;
    return X;
  }
};

/// What an expression may name: the operand variables with their choice,
/// the number variables and the fields of the state.
struct Scope {
  std::map<std::string, std::string> Operands;
  std::set<std::string> Numbers;
  Cpu C = Cpu::Z80;
};

} // namespace

static std::shared_ptr<const Expr> binaryExpr(StringRef Op,
                                              std::shared_ptr<const Expr> L,
                                              std::shared_ptr<const Expr> R) {
  auto E = std::make_shared<Expr>();
  E->K = Expr::Binary;
  E->Name = Op.str();
  E->L = std::move(L);
  E->R = std::move(R);
  return E;
}

/// Splits an operand into the numbers and number variables in it, which go
/// to Nums, and the text between them.
static std::vector<std::string> pieces(StringRef Op, const Scope &S,
                                       std::vector<Val> &Nums) {
  std::vector<std::string> Text = {""};
  std::string L = Op.lower();
  erase(L, ' ');
  StringRef R(L);
  while (!R.empty()) {
    bool Sign = R.front() == '-' && R.size() > 1 && isDigit(R[1]) &&
                (Text.back().empty() || strchr("(,+-#", Text.back().back()));
    if (isDigit(R.front()) || Sign) {
      size_t Len = Sign + R.drop_front(Sign).take_while(isAlnum).size();
      int64_t V = 0;
      if (R.take_front(Len).getAsInteger(0, V))
        V = 0;
      Nums.push_back(Val::number(V));
      Text.emplace_back();
      R = R.drop_front(Len);
      continue;
    }
    if (isAlpha(R.front()) || R.front() == '_') {
      StringRef Name =
          R.take_while([](char Ch) { return isAlnum(Ch) || Ch == '_'; });
      if (S.Numbers.count(Name.str())) {
        auto E = std::make_shared<Expr>();
        E->K = Expr::Id;
        E->Name = Name.str();
        Nums.push_back(Val::left(E));
        Text.emplace_back();
      } else {
        Text.back() += Name.str();
      }
      R = R.drop_front(Name.size());
      continue;
    }
    Text.back() += R.front();
    R = R.drop_front();
  }
  return Text;
}

/// Whether two operands are the same, as the numbers in them are.
static Val sameOperand(StringRef A, StringRef B, const Scope &S) {
  std::vector<Val> NA, NB;
  if (pieces(A, S, NA) != pieces(B, S, NB))
    return Val::number(0);
  std::shared_ptr<const Expr> All;
  for (size_t I = 0; I < NA.size(); ++I) {
    if (NA[I].K == Val::Number && NB[I].K == Val::Number) {
      if (NA[I].N != NB[I].N)
        return Val::number(0);
      continue;
    }
    auto Eq = binaryExpr("==", NA[I].expr(), NB[I].expr());
    All = All ? binaryExpr("&&", All, Eq) : Eq;
  }
  return All ? Val::left(All) : Val::number(1);
}

static int64_t compute(StringRef Op, int64_t L, int64_t R) {
  if (Op == "+")
    return L + R;
  if (Op == "-")
    return L - R;
  if (Op == "*")
    return L * R;
  if (Op == "/")
    return R ? L / R : 0;
  if (Op == "%")
    return R ? L % R : 0;
  if (Op == "&")
    return L & R;
  if (Op == "|")
    return L | R;
  if (Op == "^")
    return L ^ R;
  if (Op == "<<")
    return R >= 0 && R < 64 ? int64_t(uint64_t(L) << R) : 0;
  if (Op == ">>")
    return L >> std::min<int64_t>(std::max<int64_t>(R, 0), 63);
  if (Op == "==")
    return L == R;
  if (Op == "!=")
    return L != R;
  if (Op == "<")
    return L < R;
  if (Op == "<=")
    return L <= R;
  if (Op == ">")
    return L > R;
  if (Op == ">=")
    return L >= R;
  if (Op == "&&")
    return L && R;
  return L || R;
}

static Val truth(const Val &V) {
  if (V.K == Val::Number)
    return Val::number(V.N != 0);
  auto Zero = std::make_shared<Expr>();
  return Val::left(binaryExpr("!=", V.E, Zero));
}

/// An operand, or the number it is, such as a bit number.
static Val operandVal(const std::string &Text) {
  int64_t V;
  if (!StringRef(Text).getAsInteger(0, V))
    return Val::number(V);
  return Val::operand(Text);
}

/// Evaluates as much of E as the chosen operands decide.
static Expected<Val> fold(const Expr &E, const Scope &S) {
  auto Bad = [&](const Twine &Msg) {
    return createStringError("%s", Msg.str().c_str());
  };
  switch (E.K) {
  case Expr::Num: return Val::number(E.Val);
  case Expr::Text: return operandVal(E.Name);
  case Expr::Id: {
    if (auto It = S.Operands.find(E.Name); It != S.Operands.end())
      return operandVal(It->second);
    if (S.Numbers.count(E.Name))
      return Val::left(std::make_shared<Expr>(E));
    SmallVector<StringRef, 6> Fields = fieldsNamed(E.Name);
    if (!Fields.empty() && E.Name != "F" && E.Name != "AF") {
      for (StringRef F : Fields)
        if (none_of(stateFields(S.C),
                    [&](const StateField &SF) { return F == SF.Name; }))
          return Bad("the " + Twine(cpuName(S.C)) + " has no " + E.Name);
      return Val::left(std::make_shared<Expr>(E));
    }
    if (isRegisterName(E.Name) && E.Name == StringRef(E.Name).lower())
      return Val::operand(E.Name);
    return Bad("'" + E.Name +
               "' is not a variable, a field of the state or "
               "a register");
  }
  case Expr::Call: {
    // A number among the operands is no register.
    auto Arg = [&](const Expr &X) -> Expected<Val> {
      auto V = fold(X, S);
      if (V && V->K == Val::Left)
        return Bad(E.Name + "() takes operands");
      if (V && V->K == Val::Number)
        return Val::operand("");
      return V;
    };
    auto A = Arg(*E.L);
    if (!A)
      return A;
    if (E.Name == "isreg")
      return Val::number(registerFields(A->Text).has_value());
    if (!E.R)
      return Bad(E.Name + "() takes two operands");
    auto B = Arg(*E.R);
    if (!B)
      return B;
    auto FA = operandFields(A->Text), FB = operandFields(B->Text);
    return Val::number(
        any_of(FA, [&](StringRef F) { return is_contained(FB, F); }));
  }
  case Expr::Unary: {
    auto V = fold(*E.L, S);
    if (!V)
      return V;
    if (V->K == Val::Operand)
      return Bad("'" + E.Name + "' on an operand");
    if (V->K == Val::Number)
      return Val::number(E.Name == "-" ? -V->N : E.Name == "~" ? ~V->N : !V->N);
    auto X = std::make_shared<Expr>(E);
    X->L = V->E;
    return Val::left(X);
  }
  case Expr::Binary: break;
  }
  auto L = fold(*E.L, S);
  if (!L)
    return L;
  StringRef Op = E.Name;
  // A side that decides && or || leaves the other unread.
  if ((Op == "&&" || Op == "||") && L->K == Val::Number &&
      (L->N != 0) == (Op == "||"))
    return Val::number(Op == "||");
  auto R = fold(*E.R, S);
  if (!R)
    return R;
  if (Op == "&&" || Op == "||") {
    if (L->K == Val::Operand || R->K == Val::Operand)
      return Bad("'" + Op + "' on an operand");
    if (R->K == Val::Number && (R->N != 0) == (Op == "||"))
      return Val::number(Op == "||");
    if (L->K == Val::Number)
      return truth(*R);
    if (R->K == Val::Number)
      return truth(*L);
    return Val::left(binaryExpr(Op, L->E, R->E));
  }
  if (L->K == Val::Operand || R->K == Val::Operand) {
    if (Op != "==" && Op != "!=")
      return Bad("'" + Op + "' on an operand");
    Val Same = L->K == R->K ? sameOperand(L->Text, R->Text, S) : Val::number(0);
    if (Op == "==")
      return Same;
    if (Same.K == Val::Number)
      return Val::number(!Same.N);
    auto Not = std::make_shared<Expr>();
    Not->K = Expr::Unary;
    Not->Name = "!";
    Not->L = Same.E;
    return Val::left(Not);
  }
  if (L->K == Val::Number && R->K == Val::Number)
    return Val::number(compute(Op, L->N, R->N));
  return Val::left(binaryExpr(Op, L->expr(), R->expr()));
}

/// Checks that E names only what a rule can name.
static Error checkNames(const Expr &E, const Rule &R) {
  if (E.K == Expr::Id) {
    bool Known =
        any_of(R.Vars, [&](const RuleVar &V) { return V.Name == E.Name; }) ||
        !fieldsNamed(E.Name).empty() ||
        (isRegisterName(E.Name) && E.Name == StringRef(E.Name).lower());
    if (!Known)
      return createStringError("%s: rule %s names '%s', which is not a "
                               "variable, a field of the state or a register",
                               R.where().c_str(), R.Name.c_str(),
                               E.Name.c_str());
  }
  for (const auto &Sub : {E.L, E.R})
    if (Sub)
      if (Error Err = checkNames(*Sub, R))
        return Err;
  return Error::success();
}

// --- Types --------------------------------------------------------------

/// The range of a number type.
static std::optional<std::pair<int64_t, int64_t>> numberType(StringRef T) {
  static const std::map<std::string, std::pair<int64_t, int64_t>> Named = {
      {"u8", {0, 0xFF}},
      {"s8", {-0x80, 0x7F}},
      {"u16", {0, 0xFFFF}},
      {"s16", {-0x8000, 0x7FFF}}};
  if (auto It = Named.find(T.str()); It != Named.end())
    return It->second;
  auto [Lo, Hi] = T.split("..");
  int64_t Min, Max;
  if (Hi.empty() || Lo.trim().getAsInteger(0, Min) ||
      Hi.trim().getAsInteger(0, Max))
    return std::nullopt;
  return std::pair(Min, Max);
}

/// The operands a type of operands has on C.
static std::optional<std::vector<std::string>> operandType(StringRef T, Cpu C) {
  bool Z80 = C == Cpu::Z80;
  if (T == "reg8")
    return std::vector<std::string>{"a", "b", "c", "d", "e", "h", "l"};
  if (T == "reg16")
    return std::vector<std::string>{"bc", "de", "hl"};
  if (T == "pair")
    return std::vector<std::string>{"bc", "de", "hl", "sp"};
  if (T == "idx")
    return Z80 ? std::vector<std::string>{"ix", "iy"}
               : std::vector<std::string>{};
  if (T == "cond") {
    std::vector<std::string> Out = {"z", "nz", "c", "nc"};
    if (Z80)
      Out.insert(Out.end(), {"po", "pe", "p", "m"});
    return Out;
  }
  if (T == "bit")
    return std::vector<std::string>{"0", "1", "2", "3", "4", "5", "6", "7"};
  return std::nullopt;
}

/// Gives V the operands or the range its type has on C.
static Error resolve(RuleVar &V, Cpu C, const Rule &R) {
  if (auto N = numberType(V.Type)) {
    V.Number = true;
    std::tie(V.Min, V.Max) = *N;
    if (V.Min > V.Max || V.Min < INT32_MIN || V.Max > INT32_MAX)
      return createStringError("%s: the range of %s is empty or wider than "
                               "32 bits",
                               R.where().c_str(), V.Name.c_str());
    return Error::success();
  }
  SmallVector<StringRef> Parts;
  StringRef(V.Type).split(Parts, '|');
  for (StringRef P : Parts) {
    P = P.trim();
    if (P.empty())
      return createStringError("%s: an empty choice for %s", R.where().c_str(),
                               V.Name.c_str());
    if (numberType(P))
      return createStringError("%s: %s mixes numbers with operands; give the "
                               "number a variable of its own",
                               R.where().c_str(), V.Name.c_str());
    if (auto T = operandType(P, C)) {
      for (std::string &O : *T)
        if (!is_contained(V.Choices, O))
          V.Choices.push_back(O);
    } else if (!is_contained(V.Choices, P.str())) {
      V.Choices.push_back(P.str());
    }
  }
  return Error::success();
}

static bool reservedName(StringRef N) {
  return isRegisterName(N) || !fieldsNamed(StringRef(N).upper()).empty() ||
         is_contained(Functions, N);
}

// --- Assembly -----------------------------------------------------------

/// Replaces each name in Line that Map has.
static std::string replaceNames(StringRef Line,
                                const std::map<std::string, std::string> &Map,
                                std::set<std::string> *Seen = nullptr) {
  std::string Out;
  while (!Line.empty()) {
    if (isAlpha(Line.front()) || Line.front() == '_') {
      StringRef W =
          Line.take_while([](char Ch) { return isAlnum(Ch) || Ch == '_'; });
      if (Seen)
        Seen->insert(W.str());
      auto It = Map.find(W.str());
      Out += It == Map.end() ? W.str() : It->second;
      Line = Line.drop_front(W.size());
    } else if (isDigit(Line.front())) {
      StringRef W = Line.take_while(isAlnum);
      Out += W.str();
      Line = Line.drop_front(W.size());
    } else {
      Out += Line.front();
      Line = Line.drop_front();
    }
  }
  return Out;
}

/// The names that the operands of Line use, in order.
static std::vector<std::string> operandNames(StringRef Line) {
  std::vector<std::string> Out;
  StringRef Ops = Line.ltrim().drop_while([](char Ch) { return !isSpace(Ch); });
  while (!Ops.empty()) {
    if (isAlpha(Ops.front()) || Ops.front() == '_') {
      StringRef W =
          Ops.take_while([](char Ch) { return isAlnum(Ch) || Ch == '_'; });
      Out.push_back(W.str());
      Ops = Ops.drop_front(W.size());
    } else if (isDigit(Ops.front())) {
      Ops = Ops.drop_front(Ops.take_while(isAlnum).size());
    } else {
      Ops = Ops.drop_front();
    }
  }
  return Out;
}

/// The label a jump or a call in Line goes to, if it names one.
static std::optional<std::string> targetOf(StringRef Line) {
  StringRef Mnem =
      Line.ltrim().take_while([](char Ch) { return !isSpace(Ch); });
  if (!is_contained({"jp", "jr", "call", "djnz"}, Mnem.lower()))
    return std::nullopt;
  std::vector<std::string> Names = operandNames(Line);
  if (Names.empty())
    return std::nullopt;
  return Names.back();
}

static std::optional<StringRef> labelOf(StringRef Line) {
  Line = Line.trim();
  if (Line.consume_back(":") && !Line.empty() &&
      all_of(Line, [](char Ch) { return isAlnum(Ch) || Ch == '_'; }))
    return Line;
  return std::nullopt;
}

ArrayRef<StateField> z80tester::stateFields(Cpu C) {
  if (C == Cpu::Z80)
    return Z80Fields;
  return SM83Fields;
}

std::string Rule::where() const {
  return (sys::path::filename(File) + ":" + Twine(Line)).str();
}

std::string RuleInstance::choice() const {
  std::string Out;
  for (const auto &[Name, Value] : Choice) {
    if (!Out.empty())
      Out += ' ';
    Out += Name;
    Out += '=';
    Out += Value;
  }
  return Out;
}

Expected<std::vector<Rule>> z80tester::loadRules(StringRef Path) {
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
    auto Fail = [&](const Twine &Msg) {
      return createStringError("%s: %s", Where.c_str(), Msg.str().c_str());
    };

    if (!isSpace(Line.front())) {
      StringRef Name;
      if (Line.size() > 4 && Line.starts_with("rule") && isSpace(Line[4]))
        Name = Line.drop_front(4).trim();
      if (Name.empty())
        return Fail("expected 'rule <name>'");
      if (!Names.insert(Name.str()).second)
        return Fail("second rule named " + Name);
      Rule &R = Rules.emplace_back();
      R.Name = Name.str();
      R.File = Path.str();
      R.Line = N;
      Side = nullptr;
      continue;
    }
    if (Rules.empty())
      return Fail("expected 'rule <name>'");
    Rule &R = Rules.back();

    StringRef Text = Line.trim();
    bool Before = Text.consume_front("before:");
    if (Before || Text.consume_front("after:")) {
      Side = Before ? &R.Before : &R.After;
      if (!Side->empty())
        return Fail(Twine("second '") + (Before ? "before:" : "after:") +
                    "' in rule " + R.Name);
      if (!Text.trim().empty())
        Side->push_back({Text.trim().str(), N});
      continue;
    }
    bool Clause = true;
    if (Text.consume_front("cpu:")) {
      SmallVector<StringRef> Words;
      SplitString(Text, Words, " \t,");
      for (StringRef W : Words) {
        if (W == "z80")
          R.Cpus.push_back(Cpu::Z80);
        else if (W == "sm83")
          R.Cpus.push_back(Cpu::SM83);
        else
          return Fail("'" + W + "' is not z80 or sm83");
      }
    } else if (Text.consume_front("for:")) {
      SmallVector<StringRef> Groups;
      Text.split(Groups, ';');
      for (StringRef G : Groups) {
        auto [Vars, Type] = G.split(" in ");
        Type = Type.trim();
        SmallVector<StringRef> VarNames;
        Vars.split(VarNames, ',');
        if (Type.empty() || Vars.trim().empty())
          return Fail("expected 'for: <name>, ... in <type>; ...'");
        for (StringRef V : VarNames) {
          V = V.trim();
          if (V.empty() || isDigit(V.front()) ||
              !all_of(V, [](char Ch) { return isAlnum(Ch) || Ch == '_'; }))
            return Fail("'" + V + "' is not a name");
          if (reservedName(V))
            return Fail(V + " is a register, a condition, a field of the "
                            "state or a function, not a name for a variable");
          if (any_of(R.Vars, [&](const RuleVar &X) { return X.Name == V; }))
            return Fail("second variable named " + V);
          RuleVar RV;
          RV.Name = V.str();
          RV.Type = Type.str();
          if (Error E = resolve(RV, Cpu::Z80, R))
            return std::move(E);
          R.Vars.push_back(RV);
        }
      }
    } else if (Text.consume_front("if:")) {
      auto E = ExprParser(Text).parse();
      if (!E)
        return Fail(toString(E.takeError()));
      R.Conds.push_back(*E);
    } else if (Text.consume_front("dead:")) {
      std::string Items = Text.str();
      if (size_t At = Items.find("below SP"); At != std::string::npos) {
        R.DeadBelowSP = true;
        Items.erase(At, 8);
      }
      SmallVector<StringRef> Words;
      SplitString(Items, Words, " \t,");
      for (StringRef W : Words)
        R.Dead.push_back(W.str());
    } else {
      Clause = false;
    }
    if (Clause) {
      Side = nullptr;
      continue;
    }
    if (!Side)
      return Fail("an instruction outside 'before:' and 'after:'");
    Side->push_back({Text.str(), N});
  }

  for (const Rule &R : Rules) {
    if (R.Before.empty())
      return createStringError("%s: rule %s has no 'before:' code",
                               R.where().c_str(), R.Name.c_str());
    for (const auto &E : R.Conds)
      if (Error Err = checkNames(*E, R))
        return std::move(Err);
    for (const std::string &D : R.Dead)
      if (none_of(R.Vars, [&](const RuleVar &V) { return V.Name == D; }) &&
          fieldsNamed(D).empty())
        return createStringError("%s: rule %s says %s may differ, which is "
                                 "not a variable or a field of the state",
                                 R.where().c_str(), R.Name.c_str(), D.c_str());
  }
  return Rules;
}

std::vector<Cpu> z80tester::ruleCpus(const Rule &R, Cpu Default) {
  if (R.Cpus.empty())
    return {Default};
  return R.Cpus;
}

/// The names in an expression.
static void namesIn(const Expr &E, std::set<std::string> &Out) {
  if (E.K == Expr::Id)
    Out.insert(E.Name);
  for (const auto &Sub : {E.L, E.R})
    if (Sub)
      namesIn(*Sub, Out);
}

/// The names in a line of assembly, the mnemonic's too.
static std::set<std::string> namesIn(StringRef Line) {
  std::set<std::string> Out;
  replaceNames(Line, {}, &Out);
  return Out;
}

Expected<std::vector<RuleInstance>>
z80tester::instantiate(const Rule &R, Cpu C, LineCheck Assembles,
                       std::string *AsmError) {
  std::vector<RuleVar> Operands, Numbers;
  for (RuleVar V : R.Vars) {
    V.Choices.clear();
    if (Error E = resolve(V, C, R))
      return std::move(E);
    (V.Number ? Numbers : Operands).push_back(V);
  }
  // What to check once the operand variable at each index is chosen: the
  // conditions, the variables that may differ and the lines whose operand
  // variables are all chosen by then.
  auto Last = [&](const std::set<std::string> &Names) {
    size_t At = 0;
    for (size_t K = 0; K < Operands.size(); ++K)
      if (Names.count(Operands[K].Name))
        At = K + 1;
    return At;
  };
  std::vector<std::vector<const Expr *>> CondsAt(Operands.size() + 1);
  for (const auto &E : R.Conds) {
    std::set<std::string> Names;
    namesIn(*E, Names);
    CondsAt[Last(Names)].push_back(E.get());
  }
  std::vector<std::vector<const std::pair<std::string, unsigned> *>> LinesAt(
      Operands.size() + 1);
  for (const auto *Side : {&R.Before, &R.After})
    for (const auto &L : *Side)
      LinesAt[Last(namesIn(L.first))].push_back(&L);

  std::vector<RuleInstance> Out;
  Scope S;
  S.C = C;
  for (const RuleVar &V : Numbers)
    S.Numbers.insert(V.Name);
  std::vector<std::shared_ptr<const Expr>> Assumes;
  std::map<std::string, bool> Checked;

  // Whether what can be checked once K operand variables are chosen holds.
  auto Holds = [&](size_t K) -> Expected<bool> {
    for (const Expr *E : CondsAt[K]) {
      Expected<Val> V = fold(*E, S);
      if (!V)
        return createStringError("%s: rule %s: %s", R.where().c_str(),
                                 R.Name.c_str(),
                                 toString(V.takeError()).c_str());
      if (V->K == Val::Operand)
        return createStringError("%s: rule %s has a condition that is an "
                                 "operand, not a truth",
                                 R.where().c_str(), R.Name.c_str());
      if (V->K == Val::Number && !V->N)
        return false;
      if (V->K == Val::Left)
        Assumes.push_back(V->E);
    }
    if (K && is_contained(R.Dead, Operands[K - 1].Name) &&
        !registerFields(S.Operands[Operands[K - 1].Name]))
      return false;
    for (const auto *L : LinesAt[K]) {
      if (!Assembles || labelOf(L->first))
        continue;
      std::string Text = replaceNames(L->first, S.Operands);
      auto [It, New] = Checked.try_emplace(Text, true);
      if (New) {
        std::optional<std::string> Err = Assembles(Text, L->second);
        It->second = !Err;
        if (Err && AsmError && AsmError->empty())
          *AsmError = *Err;
      }
      if (!It->second)
        return false;
    }
    return true;
  };

  std::function<Error(size_t)> Choose = [&](size_t K) -> Error {
    size_t Mark = Assumes.size();
    Expected<bool> Ok = Holds(K);
    if (!Ok)
      return Ok.takeError();
    if (*Ok && K < Operands.size()) {
      for (const std::string &O : Operands[K].Choices) {
        S.Operands[Operands[K].Name] = O;
        if (Error E = Choose(K + 1))
          return E;
      }
      S.Operands.erase(Operands[K].Name);
    } else if (*Ok) {
      if (Out.size() == 1000000)
        return createStringError("%s: rule %s has over a million cases",
                                 R.where().c_str(), R.Name.c_str());
      RuleInstance I;
      I.R = &R;
      I.C = C;
      I.Numbers = Numbers;
      I.AboveSP = R.DeadBelowSP;
      I.Assumes = Assumes;
      for (const RuleVar &V : Operands)
        I.Choice.push_back({V.Name, S.Operands[V.Name]});
      std::set<std::string> Dead;
      for (const std::string &D : R.Dead) {
        SmallVector<StringRef, 6> Fields;
        if (auto It = S.Operands.find(D); It != S.Operands.end())
          Fields = *registerFields(It->second);
        else
          Fields = fieldsNamed(D);
        for (StringRef F : Fields)
          Dead.insert(F.str());
      }
      for (const StateField &F : stateFields(C))
        if (!Dead.count(F.Name))
          I.Compared.push_back(F.Name);
      if (I.AboveSP && Dead.count("SP"))
        return createStringError("%s: rule %s compares memory above SP, so "
                                 "SP cannot differ",
                                 R.where().c_str(), R.Name.c_str());
      for (bool After : {false, true})
        for (const auto &[Text, Line] : After ? R.After : R.Before)
          (After ? I.After : I.Before)
              .push_back({replaceNames(Text, S.Operands), Line});
      Out.push_back(std::move(I));
    }
    Assumes.resize(Mark);
    return Error::success();
  };
  if (Error E = Choose(0))
    return std::move(E);
  return Out;
}

std::string z80tester::ruleLabel(bool After) {
  return After ? "z80test_after" : "z80test_before";
}

std::string z80tester::instanceSource(const RuleInstance &I) {
  std::set<std::string> NumberNames;
  for (const RuleVar &V : I.Numbers)
    NumberNames.insert(V.Name);

  // Each side has its own labels. A jump to another leaves the code, for an
  // address of its own; a call to one returns at once.
  std::map<bool, std::set<std::string>> Defined;
  std::vector<std::string> Outside;
  std::set<std::string> Called;
  for (bool After : {false, true})
    for (const auto &[Text, Line] : After ? I.After : I.Before)
      if (auto L = labelOf(Text))
        Defined[After].insert(L->str());
  for (bool After : {false, true})
    for (const auto &[Text, Line] : After ? I.After : I.Before) {
      std::optional<std::string> N = targetOf(Text);
      if (!N || Defined[After].count(*N) || NumberNames.count(*N) ||
          isRegisterName(*N))
        continue;
      if (!is_contained(Outside, *N))
        Outside.push_back(*N);
      if (StringRef(Text).ltrim().starts_with_insensitive("call"))
        Called.insert(*N);
    }

  std::string S;
  raw_string_ostream OS(S);
  OS << "\t.area _CODE\n";
  for (bool After : {false, true})
    OS << "\t.globl _" << ruleLabel(After) << '\n';
  for (bool After : {false, true}) {
    std::string Prefix = After ? "z80test_a_" : "z80test_b_";
    std::map<std::string, std::string> Local;
    for (const std::string &L : Defined[After])
      Local[L] = Prefix + L;
    for (const std::string &N : Outside)
      Local[N] = Prefix + N;
    OS << '_' << ruleLabel(After) << ":\n";
    std::set<std::string> Used;
    // A line marker makes the assembler report errors in the rule file.
    for (const auto &[Text, Line] : After ? I.After : I.Before) {
      if (std::optional<std::string> N = targetOf(Text))
        Used.insert(*N);
      OS << "# " << Line << " \"" << I.R->File << "\"\n"
         << (labelOf(Text) ? "" : "\t") << replaceNames(Text, Local) << '\n';
    }
    OS << "\thalt\n";
    for (size_t K = 0; K < Outside.size(); ++K) {
      if (!Used.count(Outside[K]))
        continue;
      OS << Prefix << Outside[K] << ":\n";
      if (Called.count(Outside[K]))
        OS << "\tret\n";
      else
        OS << formatv("\tjp\t0x{0:x-4}\n", 0xF000 + K);
    }
  }
  return S;
}
