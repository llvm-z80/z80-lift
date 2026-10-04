// Reads contracts and compiles them to bitcode.

#include "z80tester/Contract.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <map>
#include <set>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

const char *const RegNames[] = {"A",  "B",  "C",  "D",  "E",  "H", "L",
                                "BC", "DE", "HL", "IX", "IY", "SP"};

std::optional<Reg> regNamed(StringRef Name) {
  for (unsigned I = 0; I < std::size(RegNames); ++I)
    if (Name == RegNames[I])
      return Reg(I);
  return std::nullopt;
}

bool isIdentStart(char C) { return isAlpha(C) || C == '_'; }
bool isIdentChar(char C) { return isAlnum(C) || C == '_'; }

/// The identifiers of a C expression, skipping numbers such as 0x1p31f.
std::vector<StringRef> identifiers(StringRef S) {
  std::vector<StringRef> Ids;
  size_t I = 0;
  while (I < S.size()) {
    char C = S[I];
    if (isDigit(C) || (C == '.' && I + 1 < S.size() && isDigit(S[I + 1]))) {
      ++I;
      while (I < S.size() &&
             (isIdentChar(S[I]) || S[I] == '.' ||
              ((S[I] == '+' || S[I] == '-') && strchr("eEpP", S[I - 1]))))
        ++I;
    } else if (isIdentStart(C)) {
      size_t Start = I;
      while (I < S.size() && isIdentChar(S[I]))
        ++I;
      Ids.push_back(S.slice(Start, I));
    } else {
      ++I;
    }
  }
  return Ids;
}

/// Types whose size differs between the host the contracts are compiled for
/// and the Z80.
Error checkTypes(StringRef Types, const Twine &Where) {
  std::vector<StringRef> Ids = identifiers(Types);
  for (size_t I = 0; I < Ids.size(); ++I) {
    StringRef Id = Ids[I];
    bool Bad = StringSwitch<bool>(Id)
                   .Cases({"int", "long", "short", "char", "double", "size_t",
                           "ssize_t", "ptrdiff_t", "bool", "_Bool"},
                          true)
                   .Default(false);
    if ((Id == "unsigned" || Id == "signed") &&
        (I + 1 == Ids.size() || Ids[I + 1] != "__int128"))
      Bad = true;
    if (Bad)
      return createStringError("%s: '%s' has a different size on the target; "
                               "use a fixed-width type",
                               Where.str().c_str(), Id.str().c_str());
  }
  return Error::success();
}

struct Param {
  StringRef Type, Name;
};

/// The parameters in a C parameter list.
std::vector<Param> parameters(StringRef Params) {
  std::vector<Param> Out;
  SmallVector<StringRef> Parts;
  Params.split(Parts, ',');
  for (StringRef P : Parts) {
    P = P.trim();
    std::vector<StringRef> Ids = identifiers(P);
    if (!Ids.empty() && P != "void")
      Out.push_back({P.drop_back(Ids.back().size()).rtrim(), Ids.back()});
  }
  return Out;
}

/// Splits at the commas outside brackets.
SmallVector<StringRef> splitArgs(StringRef S) {
  SmallVector<StringRef> Out;
  int Depth = 0;
  size_t Start = 0;
  for (size_t I = 0; I < S.size(); ++I) {
    if (strchr("([{", S[I]))
      ++Depth;
    else if (strchr(")]}", S[I]))
      --Depth;
    else if (S[I] == ',' && Depth == 0) {
      Out.push_back(S.slice(Start, I).trim());
      Start = I + 1;
    }
  }
  Out.push_back(S.drop_front(Start).trim());
  return Out;
}

/// The index of the parenthesis that closes the one at Open, or npos.
size_t matchParen(StringRef S, size_t Open) {
  int Depth = 0;
  for (size_t I = Open; I < S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')' && --Depth == 0)
      return I;
  }
  return StringRef::npos;
}

/// The outermost `old(...)` calls in a condition, as [start, close] indices.
std::vector<std::pair<size_t, size_t>> oldCalls(StringRef S) {
  std::vector<std::pair<size_t, size_t>> Calls;
  for (size_t I = S.find("old("); I != StringRef::npos;
       I = S.find("old(", I + 1)) {
    if (I && isIdentChar(S[I - 1]))
      continue;
    if (!Calls.empty() && I <= Calls.back().second)
      continue;
    size_t Close = matchParen(S, I + 3);
    if (Close == StringRef::npos)
      break;
    Calls.push_back({I, Close});
  }
  return Calls;
}

/// Bytes of a fixed-width type, or 1 if it is not one.
unsigned typeBytes(StringRef Type) {
  if (Type.contains('*'))
    return 2;
  std::vector<StringRef> Ids = identifiers(Type);
  for (StringRef Id : Ids) {
    unsigned N = StringSwitch<unsigned>(Id)
                     .Cases({"uint8_t", "int8_t"}, 1)
                     .Cases({"uint16_t", "int16_t", "_Float16"}, 2)
                     .Cases({"uint32_t", "int32_t", "float"}, 4)
                     .Cases({"uint64_t", "int64_t"}, 8)
                     .Case("__int128", 16)
                     .Default(0);
    if (N)
      return N;
  }
  return 1;
}

ParamInfo paramInfo(const Param &P) {
  ParamInfo I;
  I.Name = P.Name.str();
  I.Type = P.Type.str();
  I.Pointer = P.Type.contains('*');
  if (I.Pointer) {
    StringRef Pointee = P.Type.take_front(P.Type.rfind('*'));
    I.Pointee = typeBytes(Pointee);
  } else {
    std::vector<StringRef> Ids = identifiers(P.Type);
    I.Signed = llvm::any_of(Ids, [](StringRef Id) {
      return Id.starts_with("int") || Id == "signed";
    });
    I.Signed |= llvm::is_contained(Ids, "__int128") &&
                !llvm::is_contained(Ids, "unsigned");
  }
  return I;
}

/// Registers named one after another, most significant first, such as HLDE.
Expected<std::vector<Reg>> parseRegs(StringRef S, const std::string &Where) {
  std::vector<Reg> Regs;
  S = S.trim();
  while (!S.empty()) {
    size_t Len =
        S.starts_with("BC") || S.starts_with("DE") || S.starts_with("HL") ? 2
        : strchr("ABCDEHL", S[0])                                         ? 1
                                                                          : 0;
    if (!Len)
      return createStringError("%s: '%s' does not start with A to L, BC, DE "
                               "or HL",
                               Where.c_str(), S.str().c_str());
    Regs.push_back(*regNamed(S.take_front(Len)));
    S = S.drop_front(Len);
  }
  if (Regs.empty())
    return createStringError("%s: __reg() names no register", Where.c_str());
  return Regs;
}

/// Removes `Name(...)` from S and returns what was in the parentheses.
std::optional<std::string> takeAttr(std::string &S, StringRef Name) {
  size_t At = S.find((Name + "(").str());
  if (At == std::string::npos)
    return std::nullopt;
  size_t Arg = At + Name.size() + 1, Close = S.find(')', Arg);
  if (Close == std::string::npos)
    return std::nullopt;
  std::string Inside = S.substr(Arg, Close - Arg);
  S.erase(At, Close + 1 - At);
  return StringRef(Inside).trim().str();
}

Expected<unsigned> parseCount(StringRef S, StringRef Attr,
                              const std::string &Where) {
  unsigned N;
  if (S.getAsInteger(0, N))
    return createStringError("%s: %s needs a byte count", Where.c_str(),
                             Attr.str().c_str());
  return N;
}

Expected<Contract> parsePrototype(StringRef Text, StringRef File,
                                  unsigned Line) {
  std::string Where = (sys::path::filename(File) + ":" + Twine(Line)).str();
  auto NotPrototype = [&] {
    return createStringError("%s: expected a C prototype", Where.c_str());
  };
  size_t Open = Text.find('('), Close = StringRef::npos;
  for (size_t I = Open, Depth = 0; I < Text.size(); ++I) {
    if (Text[I] == '(') {
      ++Depth;
    } else if (Text[I] == ')' && --Depth == 0) {
      Close = I;
      break;
    }
  }
  if (Open == StringRef::npos || Close == StringRef::npos)
    return NotPrototype();

  Contract K;
  StringRef Head = Text.take_front(Open).rtrim();
  size_t NameStart = Head.size();
  while (NameStart && isIdentChar(Head[NameStart - 1]))
    --NameStart;
  K.Name = Head.drop_front(NameStart).str();
  K.RetType = Head.take_front(NameStart).trim().str();
  K.File = File.str();
  K.Line = Line;
  if (K.Name.empty() || K.RetType.empty())
    return NotPrototype();

  // The places of the result and of each parameter, if given.
  std::string Tail = Text.drop_front(Close + 1).str();
  std::optional<std::string> RetRegs = takeAttr(Tail, "__reg");
  std::optional<std::string> Pops = takeAttr(Tail, "__pops");
  if (!StringRef(Tail).trim().empty())
    return createStringError("%s: unexpected '%s' after the parameters",
                             Where.c_str(),
                             StringRef(Tail).trim().str().c_str());
  bool Placed = RetRegs || Pops;
  if (RetRegs) {
    auto Regs = parseRegs(*RetRegs, Where);
    if (!Regs)
      return Regs.takeError();
    K.RetPlace = Place{*Regs, std::nullopt};
  }
  if (Pops) {
    auto N = parseCount(*Pops, "__pops", Where);
    if (!N)
      return N.takeError();
    K.Pops = *N;
  }

  std::vector<std::string> Parts;
  std::vector<std::optional<Place>> Places;
  for (StringRef P : splitArgs(Text.slice(Open + 1, Close))) {
    std::string S = P.str();
    std::optional<std::string> Regs = takeAttr(S, "__reg");
    std::optional<std::string> Stack = takeAttr(S, "__stack");
    Parts.push_back(StringRef(S).trim().str());
    if (Parts.back().empty() || Parts.back() == "void")
      continue;
    if (Regs && Stack)
      return createStringError("%s: '%s' has two places", Where.c_str(),
                               Parts.back().c_str());
    std::optional<Place> Pl;
    if (Regs) {
      auto R = parseRegs(*Regs, Where);
      if (!R)
        return R.takeError();
      Pl = Place{*R, std::nullopt};
    } else if (Stack) {
      auto N = parseCount(*Stack, "__stack", Where);
      if (!N)
        return N.takeError();
      Pl = Place{{}, *N};
    }
    Placed |= Pl.has_value();
    Places.push_back(Pl);
  }
  K.Params = join(Parts, ", ");

  if (Error E = checkTypes(K.RetType + " " + K.Params, Where))
    return std::move(E);
  std::vector<Param> Params = parameters(K.Params);
  for (const Param &P : Params)
    K.ParamList.push_back(paramInfo(P));
  for (const Param &P : Params)
    if (regNamed(P.Name) || P.Name == "result")
      return createStringError("%s: parameter '%s' hides a name conditions "
                               "use",
                               Where.c_str(), P.Name.str().c_str());

  // Outside the C convention, every value needs a place.
  if (Placed) {
    K.Placed = true;
    for (size_t I = 0; I < Places.size(); ++I) {
      if (!Places[I])
        return createStringError("%s: '%s' needs __reg() or __stack(), as "
                                 "other values have places",
                                 Where.c_str(), Params[I].Name.str().c_str());
      K.ParamPlaces.push_back(*Places[I]);
    }
    if (K.RetType != "void" && !K.RetPlace)
      return createStringError("%s: the result needs __reg(), as other values "
                               "have places",
                               Where.c_str());
    if (K.RetType == "void" && K.RetPlace)
      return createStringError("%s: a void function has no result to place",
                               Where.c_str());
  }
  return K;
}

Error finishCondition(Condition &Cond, Cpu C) {
  std::vector<std::pair<size_t, size_t>> Olds = oldCalls(Cond.Text);
  Cond.UsesOld = !Olds.empty();
  StringRef Text = Cond.Text;
  for (StringRef Id : identifiers(Text)) {
    std::optional<Reg> R = regNamed(Id);
    if (!R)
      continue;
    if (C == Cpu::SM83 && (*R == Reg::IX || *R == Reg::IY))
      return createStringError("%s: the SM83 has no %s", Cond.where().c_str(),
                               Id.str().c_str());
    size_t At = Id.data() - Text.data();
    bool InOld = llvm::any_of(
        Olds, [&](const auto &O) { return O.first <= At && At <= O.second; });
    std::vector<Reg> &List = InOld ? Cond.OldRegs : Cond.Regs;
    if (!llvm::is_contained(List, *R))
      List.push_back(*R);
  }
  return Error::success();
}

/// Splits "lo .. hi" at its `..`.
std::optional<std::pair<StringRef, StringRef>> splitRange(StringRef S) {
  size_t Dots = S.find("..");
  if (Dots == StringRef::npos)
    return std::nullopt;
  StringRef Lo = S.take_front(Dots).trim(), Hi = S.drop_front(Dots + 2).trim();
  if (Lo.empty() || Hi.empty())
    return std::nullopt;
  return std::pair{Lo, Hi};
}

std::optional<unsigned> paramNamed(const Contract &K, StringRef Name) {
  for (unsigned I = 0; I < K.ParamList.size(); ++I)
    if (K.ParamList[I].Name == Name)
      return I;
  return std::nullopt;
}

/// Reads a `reads` or `modifies` item: `p[lo .. hi]` or `*p`.
Error applyRange(Contract &K, const Condition &Item, bool Writes) {
  std::string Where = Item.where();
  StringRef Text = Item.Text;
  Range R;
  R.Writes = Writes;
  R.File = Item.File;
  R.Line = Item.Line;
  StringRef Name;
  if (Text.consume_front("*")) {
    Name = Text.trim();
    R.Lo = "0";
    R.Hi = ("sizeof(*" + Name + ")").str();
  } else {
    Name = Text.take_while(isIdentChar);
    StringRef Index = Text.drop_front(Name.size()).trim();
    std::optional<std::pair<StringRef, StringRef>> Bounds;
    if (Index.consume_front("[") && Index.consume_back("]"))
      Bounds = splitRange(Index);
    if (!Bounds)
      return createStringError("%s: expected p[lo .. hi] or *p", Where.c_str());
    R.Lo = Bounds->first.str();
    R.Hi = Bounds->second.str();
  }
  std::optional<unsigned> P = paramNamed(K, Name);
  if (!P || !K.ParamList[*P].Pointer)
    return createStringError("%s: '%s' is not a pointer parameter",
                             Where.c_str(), Name.str().c_str());
  R.Param = *P;
  for (StringRef Id : identifiers(R.Lo + " " + R.Hi))
    if (std::optional<unsigned> Q = paramNamed(K, Id);
        Q && K.ParamList[*Q].Pointer && !llvm::is_contained(R.Pointers, *Q))
      R.Pointers.push_back(*Q);
  if (!oldCalls(R.Lo + " " + R.Hi).empty())
    return createStringError("%s: bounds are read before the call, so old() "
                             "is not needed",
                             Where.c_str());
  K.Ranges.push_back(std::move(R));
  return Error::success();
}

/// Reads `names in lo .. hi` or `names in string(lo .. hi)`.
Error applyDomain(Contract &K, const Condition &Item) {
  std::string Where = Item.where();
  StringRef Text = Item.Text;
  size_t In = Text.find(" in ");
  if (In == StringRef::npos)
    return createStringError("%s: unknown test setting '%s'", Where.c_str(),
                             Item.Text.c_str());
  StringRef Rhs = Text.drop_front(In + 4).trim();
  bool String = Rhs.consume_front("string");
  Rhs = Rhs.trim();
  std::pair<StringRef, StringRef> Bounds{"0", "65"};
  if (!String || !Rhs.empty()) {
    if (String && !(Rhs.consume_front("(") && Rhs.consume_back(")")))
      return createStringError("%s: expected string(lo .. hi)", Where.c_str());
    std::optional<std::pair<StringRef, StringRef>> R = splitRange(Rhs);
    if (!R)
      return createStringError("%s: expected lo .. hi", Where.c_str());
    Bounds = *R;
  }

  SmallVector<StringRef> Names;
  Text.take_front(In).split(Names, ',');
  for (StringRef Name : Names) {
    Name = Name.trim();
    std::optional<unsigned> P = paramNamed(K, Name);
    if (!P)
      return createStringError("%s: no parameter '%s'", Where.c_str(),
                               Name.str().c_str());
    if (K.ParamList[*P].Pointer != String)
      return createStringError(String
                                   ? "%s: '%s' is not a pointer, so it cannot "
                                     "be a string"
                                   : "%s: '%s' is a pointer; give it strings",
                               Where.c_str(), Name.str().c_str());
    if (llvm::any_of(K.Domains, [&](const Domain &D) { return D.Param == *P; }))
      return createStringError("%s: '%s' has a second domain", Where.c_str(),
                               Name.str().c_str());
    K.Domains.push_back({*P, Bounds.first.str(), Bounds.second.str(), String,
                         Item.File, Item.Line});
  }
  return Error::success();
}

/// Reads the items of a contract's `tests`.
Error applyTests(Contract &K, ArrayRef<Condition> Items) {
  for (const Condition &Item : Items) {
    std::string Where = Item.where();
    StringRef Word = StringRef(Item.Text).take_while(isIdentChar);
    StringRef Rest = StringRef(Item.Text).drop_front(Word.size()).trim();

    if (Word == "exhaustive" || Word == "samples") {
      if (K.Exhaustive || K.Samples)
        return createStringError("%s: exhaustive or samples given twice",
                                 Where.c_str());
      uint64_t N = 0;
      if (Word == "exhaustive" && !Rest.empty())
        return createStringError("%s: expected ';' after exhaustive",
                                 Where.c_str());
      if (Word == "samples" && (Rest.getAsInteger(0, N) || N == 0))
        return createStringError("%s: samples needs a positive count",
                                 Where.c_str());
      K.Exhaustive = Word == "exhaustive";
      if (N)
        K.Samples = N;
      continue;
    }

    if (Word != "example") {
      if (Error E = applyDomain(K, Item))
        return E;
      continue;
    }
    if (Rest.empty())
      return createStringError("%s: example gives no values", Where.c_str());
    Example E;
    E.File = Item.File;
    E.Line = Item.Line;
    for (StringRef A : splitArgs(Rest)) {
      StringRef Name = A.split('=').first.trim();
      StringRef Value = A.split('=').second.trim();
      std::optional<unsigned> P = paramNamed(K, Name);
      if (!P)
        return createStringError("%s: no parameter '%s'", Where.c_str(),
                                 Name.str().c_str());
      if (Value.empty())
        return createStringError("%s: no value for '%s'", Where.c_str(),
                                 Name.str().c_str());
      if (llvm::any_of(E.Values, [&](const auto &V) { return V.first == *P; }))
        return createStringError("%s: '%s' given twice", Where.c_str(),
                                 Name.str().c_str());
      E.Values.push_back({*P, Value.str()});
    }
    K.Examples.push_back(std::move(E));
  }
  return Error::success();
}

/// An `#include` line, with a quoted path made absolute.
std::string includeLine(StringRef Text, StringRef File) {
  StringRef Name = Text.drop_front(strlen("#include")).trim();
  if (!Name.consume_front("\"") || !Name.consume_back("\""))
    return Text.str();
  SmallString<128> Path(sys::path::parent_path(File));
  sys::path::append(Path, Name);
  sys::fs::make_absolute(Path);
  return ("#include \"" + Path + "\"").str();
}

enum class Part { None, Requires, Ensures, Reads, Modifies, Tests };

Error parseFile(StringRef Path, Cpu C, std::vector<Contract> &Out) {
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return createStringError(Buf.getError(), "%s: %s", Path.str().c_str(),
                             Buf.getError().message().c_str());

  // Assembly holds contracts in `;@` comments; any other file is all contract.
  StringRef Ext = sys::path::extension(Path);
  bool InAsm = Ext == ".asm" || Ext == ".s";

  size_t First = Out.size();
  std::vector<std::string> Includes;
  Contract *Cur = nullptr;
  Part Section = Part::None;
  // The items of reads, modifies and tests, by index into Out.
  std::map<size_t, std::vector<std::pair<Part, Condition>>> Items;
  Condition Pending;
  auto Unfinished = [&]() -> Error {
    if (Pending.Text.empty())
      return Error::success();
    return createStringError("%s: condition does not end with ';'",
                             Pending.where().c_str());
  };

  SmallVector<StringRef> Lines;
  (*Buf)->getBuffer().split(Lines, '\n');
  for (unsigned N = 0; N < Lines.size(); ++N) {
    unsigned LineNo = N + 1;
    StringRef Line = Lines[N];
    if (InAsm) {
      Line = Line.ltrim();
      if (!Line.consume_front(";@")) {
        if (Error E = Unfinished())
          return E;
        Cur = nullptr;
        continue;
      }
      Line.consume_front(" ");
    }
    Line = Line.take_front(Line.find("//")).rtrim();
    if (Line.trim().empty())
      continue;

    // An unindented line starts a contract, or includes C for all of them;
    // the indented lines belong to the contract above.
    StringRef Text = Line.trim();
    if (Line.find_first_not_of(" \t") == 0) {
      if (Error E = Unfinished())
        return E;
      Cur = nullptr;
      if (Text.starts_with("#include")) {
        Includes.push_back(includeLine(Text, Path));
        continue;
      }
      auto K = parsePrototype(Text, Path, LineNo);
      if (!K)
        return K.takeError();
      Out.push_back(std::move(*K));
      Cur = &Out.back();
      Section = Part::None;
      continue;
    }
    std::string Where = (sys::path::filename(Path) + ":" + Twine(LineNo)).str();
    if (!Cur)
      return createStringError("%s: clause outside a contract", Where.c_str());

    StringRef Word = Text.take_while(isIdentChar);
    Part P = StringSwitch<Part>(Word)
                 .Case("requires", Part::Requires)
                 .Case("ensures", Part::Ensures)
                 .Case("reads", Part::Reads)
                 .Case("modifies", Part::Modifies)
                 .Case("tests", Part::Tests)
                 .Default(Part::None);
    if (P != Part::None) {
      if (Error E = Unfinished())
        return E;
      Section = P;
      Text = Text.drop_front(Word.size()).trim();
      if (Text.empty())
        continue;
    }
    if (Section == Part::None)
      return createStringError("%s: condition outside requires or ensures",
                               Where.c_str());

    // An item runs to its ';' and may span lines.
    while (!Text.empty()) {
      if (Pending.Text.empty()) {
        Pending.File = Path.str();
        Pending.Line = LineNo;
      } else {
        Pending.Text += ' ';
      }
      auto [Head, Rest] = Text.split(';');
      Pending.Text += Head.trim().str();
      if (Head.size() == Text.size())
        break;
      Pending.Text = StringRef(Pending.Text).trim().str();
      if (Pending.Text.empty())
        return createStringError("%s: empty condition", Where.c_str());
      if (Section == Part::Requires || Section == Part::Ensures) {
        if (Error E = finishCondition(Pending, C))
          return E;
        if (Section == Part::Requires && Pending.UsesOld)
          return createStringError("%s: requires is read before the call, so "
                                   "old() is not needed",
                                   Pending.where().c_str());
        (Section == Part::Requires ? Cur->Requires : Cur->Ensures)
            .push_back(std::move(Pending));
      } else {
        Items[Out.size() - 1].push_back({Section, std::move(Pending)});
      }
      Pending = Condition();
      Text = Rest.trim();
    }
  }
  if (Error E = Unfinished())
    return E;

  for (auto &[I, List] : Items) {
    std::vector<Condition> Tests;
    for (auto &[P, Item] : List) {
      if (P == Part::Tests) {
        Tests.push_back(Item);
        continue;
      }
      if (Error E = applyRange(Out[I], Item, P == Part::Modifies))
        return E;
    }
    if (Error E = applyTests(Out[I], Tests))
      return E;
  }
  for (size_t I = First; I < Out.size(); ++I)
    Out[I].Includes = Includes;
  return Error::success();
}

/// A parameter list, or "void" for none.
std::string paramList(ArrayRef<std::string> Parts) {
  std::string S;
  for (const std::string &P : Parts) {
    if (P.empty() || P == "void")
      continue;
    if (!S.empty())
      S += ", ";
    S += P;
  }
  return S.empty() ? "void" : S;
}

std::string lineDirective(unsigned Line, StringRef File) {
  return formatv("#line {0} \"{1}\"\n", Line, File).str();
}

// Helpers for conditions. `same` compares floats bit for bit, except that any
// two NaNs are the same.
const char *const Prelude = R"(#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool z80tester_same32(float X, float Y) {
  uint32_t A, B;
  memcpy(&A, &X, 4);
  memcpy(&B, &Y, 4);
  return (X != X && Y != Y) || A == B;
}

static inline bool z80tester_same16(_Float16 X, _Float16 Y) {
  uint16_t A, B;
  memcpy(&A, &X, 2);
  memcpy(&B, &Y, 2);
  return (X != X && Y != Y) || A == B;
}

#define same(X, Y)                                                             \
  _Generic((X), _Float16: z80tester_same16, default: z80tester_same32)(X, Y)
#define forall(I, Lo, Hi, Cond)                                                \
  ({                                                                           \
    bool z80tester_all = true;                                                 \
    for (int64_t I = (Lo); z80tester_all && I < (Hi); ++I)                     \
      z80tester_all = (Cond);                                                  \
    z80tester_all;                                                             \
  })
#define exists(I, Lo, Hi, Cond)                                                \
  ({                                                                           \
    bool z80tester_any = false;                                                \
    for (int64_t I = (Lo); !z80tester_any && I < (Hi); ++I)                    \
      z80tester_any = (Cond);                                                  \
    z80tester_any;                                                             \
  })
)";

/// Rewrites each `old(e)` to evaluate e with the pointers and registers as
/// they were before the call.
std::string rewriteOld(StringRef Text, StringRef Decls) {
  std::string Out;
  size_t From = 0;
  for (auto [At, Close] : oldCalls(Text)) {
    Out += Text.slice(From, At);
    Out += ("({ " + Decls + "(" + rewriteOld(Text.slice(At + 4, Close), Decls) +
            "); })")
               .str();
    From = Close + 1;
  }
  Out += Text.drop_front(From);
  return Out;
}

std::string regDecl(Reg R, StringRef Name) {
  return ((regBytes(R) == 1 ? "uint8_t " : "uint16_t ") + Name).str();
}

} // namespace

const char *z80tester::regName(Reg R) { return RegNames[unsigned(R)]; }

unsigned z80tester::regBytes(Reg R) { return R <= Reg::L ? 1 : 2; }

std::string Condition::where() const {
  return (sys::path::filename(File) + ":" + Twine(Line)).str();
}

std::string Range::where() const {
  return (sys::path::filename(File) + ":" + Twine(Line)).str();
}

std::string Domain::where() const {
  return (sys::path::filename(File) + ":" + Twine(Line)).str();
}

std::string Example::where() const {
  return (sys::path::filename(File) + ":" + Twine(Line)).str();
}

std::string Contract::where() const {
  return (sys::path::filename(File) + ":" + Twine(Line)).str();
}

Expected<std::vector<Contract>>
z80tester::loadContracts(ArrayRef<std::string> Paths, Cpu C) {
  std::vector<std::string> Files;
  for (const std::string &Path : Paths) {
    if (!sys::fs::is_directory(Path)) {
      Files.push_back(Path);
      continue;
    }
    std::vector<std::string> InDir;
    std::error_code EC;
    for (sys::fs::directory_iterator It(Path, EC), End; It != End && !EC;
         It.increment(EC))
      if (sys::path::extension(It->path()) == ".asm")
        InDir.push_back(It->path());
    if (EC)
      return createStringError(EC, "%s: %s", Path.c_str(),
                               EC.message().c_str());
    llvm::sort(InDir);
    llvm::append_range(Files, InDir);
  }

  std::vector<Contract> Out;
  for (const std::string &F : Files)
    if (Error E = parseFile(F, C, Out))
      return std::move(E);

  std::map<std::string, const Contract *> Seen;
  for (const Contract &K : Out) {
    auto [It, New] = Seen.emplace(K.Name, &K);
    if (!New)
      return createStringError("%s: second contract for %s, after %s",
                               K.where().c_str(), K.Name.c_str(),
                               It->second->where().c_str());
  }
  return Out;
}

std::string z80tester::signatureFunction(size_t I) {
  return formatv("z80tester_sig_{0}", I).str();
}

std::string z80tester::requiresFunction(size_t I) {
  return formatv("z80tester_req_{0}", I).str();
}

std::string z80tester::ensuresFunction(size_t I, size_t K) {
  return formatv("z80tester_ens_{0}_{1}", I, K).str();
}

std::string z80tester::exampleFunction(size_t I, size_t E, size_t V) {
  return formatv("z80tester_ex_{0}_{1}_{2}", I, E, V).str();
}

std::string z80tester::rangeFunction(size_t I, size_t R, bool Hi) {
  return formatv("z80tester_range_{0}_{1}_{2}", I, R, Hi ? "hi" : "lo").str();
}

std::string z80tester::domainFunction(size_t I, size_t D, bool Hi) {
  return formatv("z80tester_dom_{0}_{1}_{2}", I, D, Hi ? "hi" : "lo").str();
}

std::string z80tester::contractSource(ArrayRef<Contract> Contracts) {
  std::string S = Prelude;
  raw_string_ostream OS(S);
  std::set<std::string> Included;
  for (const Contract &K : Contracts)
    for (const std::string &Inc : K.Includes)
      if (Included.insert(Inc).second)
        OS << Inc << '\n';

  for (size_t I = 0; I < Contracts.size(); ++I) {
    const Contract &K = Contracts[I];
    bool Void = K.RetType == "void";
    OS << '\n' << lineDirective(K.Line, K.File);
    OS << K.RetType << ' ' << signatureFunction(I) << '('
       << paramList({K.Params}) << ") {"
       << (Void ? "" : " return (" + K.RetType + ")0;") << " }\n";

    if (!K.Requires.empty()) {
      OS << lineDirective(K.Line, K.File);
      OS << "bool " << requiresFunction(I) << '(' << paramList({K.Params})
         << ") {\n  return 1\n";
      for (const Condition &Cond : K.Requires)
        OS << lineDirective(Cond.Line, Cond.File) << "    && (" << Cond.Text
           << ")\n";
      OS << "  ;\n}\n";
    }

    for (size_t E = 0; E < K.Ensures.size(); ++E) {
      const Condition &Cond = K.Ensures[E];
      std::vector<std::string> Parts;
      if (!Void)
        Parts.push_back(K.RetType + " result");
      Parts.push_back(K.Params);
      for (Reg R : Cond.Regs)
        Parts.push_back(regDecl(R, regName(R)));
      std::string Text = Cond.Text;
      if (Cond.UsesOld) {
        // old() sees the pointers into memory as it was, and the registers.
        std::string Decls;
        for (const ParamInfo &P : K.ParamList) {
          if (!P.Pointer)
            continue;
          Parts.push_back(P.Type + " z80tester_old_" + P.Name);
          Decls += P.Type + " " + P.Name + " = z80tester_old_" + P.Name + "; ";
        }
        for (Reg R : Cond.OldRegs) {
          Parts.push_back(
              regDecl(R, "z80tester_old_" + std::string(regName(R))));
          Decls +=
              regDecl(R, regName(R)) + " = z80tester_old_" + regName(R) + "; ";
        }
        Text = rewriteOld(Text, Decls);
      }
      OS << lineDirective(K.Line, K.File);
      OS << "bool " << ensuresFunction(I, E) << '(' << paramList(Parts)
         << ") {\n"
         << lineDirective(Cond.Line, Cond.File) << "  return (" << Text
         << ");\n}\n";
    }

    for (size_t R = 0; R < K.Ranges.size(); ++R) {
      const Range &Rg = K.Ranges[R];
      for (bool Hi : {false, true}) {
        OS << lineDirective(K.Line, K.File);
        OS << "int64_t " << rangeFunction(I, R, Hi) << '('
           << paramList({K.Params}) << ") {\n"
           << lineDirective(Rg.Line, Rg.File) << "  return (int64_t)("
           << (Hi ? Rg.Hi : Rg.Lo) << ");\n}\n";
      }
    }

    // Integer bounds come back wide, as the upper one may not fit the type.
    for (size_t D = 0; D < K.Domains.size(); ++D) {
      const Domain &Dm = K.Domains[D];
      const ParamInfo &P = K.ParamList[Dm.Param];
      bool Float = StringRef(P.Type).contains("float") ||
                   StringRef(P.Type).contains("_Float16");
      std::string Type = Dm.String ? "int64_t" : Float ? P.Type : "__int128";
      for (bool Hi : {false, true}) {
        OS << lineDirective(K.Line, K.File);
        OS << Type << ' ' << domainFunction(I, D, Hi) << "(void) {\n"
           << lineDirective(Dm.Line, Dm.File) << "  return (" << Type << ")("
           << (Hi ? Dm.Hi : Dm.Lo) << ");\n}\n";
      }
    }

    // A pointer's example is a string, whose bytes go in its buffer.
    for (size_t E = 0; E < K.Examples.size(); ++E) {
      const Example &Ex = K.Examples[E];
      for (size_t V = 0; V < Ex.Values.size(); ++V) {
        const auto &[P, Value] = Ex.Values[V];
        const ParamInfo &Info = K.ParamList[P];
        OS << lineDirective(K.Line, K.File);
        OS << (Info.Pointer ? std::string("const char *") : Info.Type) << ' '
           << exampleFunction(I, E, V) << "(void) {\n"
           << lineDirective(Ex.Line, Ex.File) << "  return (" << Value
           << ");\n}\n";
      }
    }
  }
  return S;
}

Expected<std::unique_ptr<MemoryBuffer>>
z80tester::compileContracts(StringRef Source, StringRef Clang) {
  SmallString<128> In, Out;
  if (std::error_code EC =
          sys::fs::createTemporaryFile("z80tester-contracts", "c", In))
    return createStringError(EC, "cannot create a temporary file");
  FileRemover RemoveIn(In);
  if (std::error_code EC =
          sys::fs::createTemporaryFile("z80tester-contracts", "bc", Out))
    return createStringError(EC, "cannot create a temporary file");
  FileRemover RemoveOut(Out);
  {
    std::error_code EC;
    raw_fd_ostream OS(In, EC);
    if (EC)
      return createStringError(EC, "%s: %s", In.c_str(), EC.message().c_str());
    OS << Source;
  }

  // Signed overflow from integer promotion is defined by -fwrapv; any other
  // undefined behaviour in a condition traps. Pointers can be at any address,
  // as the Z80 has no alignment.
  StringRef Args[] = {Clang,
                      "-x",
                      "c",
                      "-std=c17",
                      "-O2",
                      "-fwrapv",
                      "-ffp-contract=off",
                      "-fsanitize=undefined",
                      "-fno-sanitize=signed-integer-overflow,alignment",
                      "-fmax-type-align=1",
                      "-fsanitize-trap=undefined",
                      "-emit-llvm",
                      "-c",
                      In,
                      "-o",
                      Out};
  std::string ErrMsg;
  int RC = sys::ExecuteAndWait(Clang, Args, std::nullopt, {}, 0, 0, &ErrMsg);
  if (RC < 0)
    return createStringError("cannot run %s: %s", Clang.str().c_str(),
                             ErrMsg.c_str());
  if (RC != 0)
    return createStringError("the contracts do not compile");
  auto Buf = MemoryBuffer::getFile(Out, /*IsText=*/false,
                                   /*RequiresNullTerminator=*/false,
                                   /*IsVolatile=*/true);
  if (!Buf)
    return createStringError(Buf.getError(), "%s: %s", Out.c_str(),
                             Buf.getError().message().c_str());
  return std::move(*Buf);
}
