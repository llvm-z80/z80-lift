// Contracts written in the runtime's assembly as `;@` comments, and the C they
// are compiled from:
//
//   ;@ uint16_t __udivmodhi4(uint16_t a, uint16_t b)
//   ;@     requires
//   ;@         b != 0;
//   ;@     ensures
//   ;@         result == a / b;
//   ;@         HL == a % b;
//   ;@     tests
//   ;@         example a = 0x8000, b = 0xFFFF;
//
// A file that is not assembly holds the same lines without the `;@`. The syntax
// is described in tester/docs/Contracts.md.

#ifndef Z80TESTER_CONTRACT_H
#define Z80TESTER_CONTRACT_H

#include "z80core/Decoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace z80tester {

enum class Reg { A, B, C, D, E, H, L, BC, DE, HL, IX, IY, SP };

const char *regName(Reg R);
unsigned regBytes(Reg R);

struct Condition {
  std::string Text;
  std::string File;
  unsigned Line = 0;
  std::vector<Reg> Regs;    // the registers it reads, in order of first use
  std::vector<Reg> OldRegs; // the registers it reads inside old()
  bool UsesOld = false;

  std::string where() const;
};

/// A parameter of the prototype.
struct ParamInfo {
  std::string Name;
  std::string Type; // as written, without the name
  bool Pointer = false;
  bool Signed = false;  // for an integer
  unsigned Pointee = 1; // bytes a pointer's type points to
};

/// Memory a function reads or writes through a pointer parameter, from Lo up
/// to Hi bytes past it.
struct Range {
  unsigned Param = 0;
  std::string Lo, Hi;             // C expressions
  bool Writes = false;            // from `modifies`, else from `reads`
  std::vector<unsigned> Pointers; // pointer parameters the bounds read through
  std::string File;
  unsigned Line = 0;

  std::string where() const;
};

/// The values `tests` draws a parameter from: a half-open range of numbers,
/// or strings whose length is in the range.
struct Domain {
  unsigned Param = 0;
  std::string Lo, Hi; // C expressions
  bool String = false;
  std::string File;
  unsigned Line = 0;

  std::string where() const;
};

/// Where a value is passed, for a function outside the C calling convention:
/// in registers, most significant first, or in the stack arguments.
struct Place {
  std::vector<Reg> Regs;
  std::optional<unsigned> Stack; // bytes above the return address
};

/// Arguments to try before any others.
struct Example {
  std::vector<std::pair<unsigned, std::string>> Values; // parameter, C value
  std::string File;
  unsigned Line = 0;

  std::string where() const;
};

struct Contract {
  std::string Name; // the function's C name
  std::string RetType;
  std::string Params; // without the parentheses and the places
  std::vector<ParamInfo> ParamList;
  std::string File;
  unsigned Line = 0;
  std::vector<std::string> Includes; // `#include` lines of its file
  std::vector<Condition> Requires, Ensures;
  std::vector<Range> Ranges;

  // Where the prototype places its values; all empty under the C convention.
  std::vector<Place> ParamPlaces;
  std::optional<Place> RetPlace;
  unsigned Pops = 0; // stack argument bytes the function removes
  bool Placed = false;

  // How to test it, where the contract says.
  bool Exhaustive = false;
  std::optional<uint64_t> Samples;
  std::vector<Domain> Domains;
  std::vector<Example> Examples;

  std::string where() const;
};

/// Reads the contracts in the given files, and in every *.asm file of the
/// given directories.
llvm::Expected<std::vector<Contract>>
loadContracts(llvm::ArrayRef<std::string> Paths, z80core::Cpu C);

/// Names of the functions contractSource defines for contract I.
std::string signatureFunction(size_t I);
std::string requiresFunction(size_t I);
std::string ensuresFunction(size_t I, size_t K);
std::string exampleFunction(size_t I, size_t E, size_t V);
std::string rangeFunction(size_t I, size_t R, bool Hi);
std::string domainFunction(size_t I, size_t D, bool Hi);

/// C that defines, for each contract, a function with its prototype, one
/// testing all of `requires`, one per `ensures` condition, one per bound of
/// each range and domain, and one per value of each example.
std::string contractSource(llvm::ArrayRef<Contract> Contracts);

/// Compiles contractSource with the given clang to bitcode.
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
compileContracts(llvm::StringRef Source, llvm::StringRef Clang);

} // namespace z80tester

#endif // Z80TESTER_CONTRACT_H
