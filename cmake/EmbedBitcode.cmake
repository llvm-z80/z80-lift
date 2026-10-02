# Writes INPUT as a byte array returned by NAMESPACE::FUNCTION().

file(READ ${INPUT} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUTPUT}
  "#include \"llvm/ADT/StringRef.h\"\n"
  "\n"
  "namespace ${NAMESPACE} {\n"
  "llvm::StringRef ${FUNCTION}();\n"
  "}\n"
  "\n"
  "static const unsigned char Data[] = {${bytes}};\n"
  "\n"
  "llvm::StringRef ${NAMESPACE}::${FUNCTION}() {\n"
  "  return {reinterpret_cast<const char *>(Data), sizeof(Data)};\n"
  "}\n")
