// Reads the per-function spec files.

#include "z80tester/Spec.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLTraits.h"

using namespace llvm;
using namespace z80tester;

LLVM_YAML_IS_SEQUENCE_VECTOR(FunctionSpec)

namespace llvm::yaml {
template <> struct MappingTraits<FunctionSpec> {
  static void mapping(IO &Io, FunctionSpec &S) {
    Io.mapRequired("name", S.Name);
    Io.mapOptional("ref", S.Ref);
    Io.mapOptional("results", S.Results);
    Io.mapOptional("ptr-bytes", S.PtrBytes);
    Io.mapOptional("compare", S.Compare, std::string("exact"));
    Io.mapOptional("builtin", S.Builtin, false);
  }

  static std::string validate(IO &, FunctionSpec &S) {
    if (S.Compare != "exact" && S.Compare != "sign" && S.Compare != "zero")
      return "compare must be exact, sign or zero";
    return "";
  }
};
} // namespace llvm::yaml

std::string FunctionSpec::refName() const {
  return Ref.empty() ? StringRef(Name).ltrim('_').str() : Ref;
}

Expected<std::vector<FunctionSpec>> z80tester::loadSpecs(StringRef Path) {
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return createStringError(Buf.getError(), "%s: %s", Path.str().c_str(),
                             Buf.getError().message().c_str());
  std::vector<FunctionSpec> Specs;
  yaml::Input In((*Buf)->getBuffer());
  In >> Specs;
  if (In.error())
    return createStringError(In.error(), "%s: invalid spec",
                             Path.str().c_str());
  return Specs;
}
