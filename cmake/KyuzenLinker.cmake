# ============================================================================
# KyuzenLinker.cmake — link every executable with ld.lld directly.
#
# WHY THIS IS NEEDED
#   The old build invoked the linker itself:
#       $(LD) $(LDFLAGS) $(ALL_OBJS) -o $@          # LD = ld.lld
#   and its flags are LLD's own: -flavor gnu, -m elf_x86_64, --build-id=none.
#
#   By default CMake links through the compiler driver (clang). That is wrong
#   here for two reasons:
#     1. clang does not understand -flavor / -m elf_x86_64.
#     2. On this host the MSYS clang driver delegates linking to GCC's linker,
#        which only supports PE emulations (i386pep/i386pe) and rejects
#        elf_x86_64 outright.
#
#   Overriding CMAKE_<LANG>_LINK_EXECUTABLE makes CMake call ld.lld directly
#   while still managing the object list, the response file, and dependency
#   tracking. Per-target link options are appended where the rule places
#   <LINK_FLAGS>, so each target supplies its own -T script and flags.
#
# This mirrors the old build exactly, which never used a compiler driver to
# link anything.
# ============================================================================

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# The rule template.
#
# Placeholders understood by CMake:
#   <OBJECTS> <LINK_FLAGS> <LINK_LIBRARIES> <TARGET>
#
# Ordering matters and matches the old build's link lines:
#     $(LD) $(LDFLAGS) $(ALL_OBJS) $(LIBS) -o $@
# i.e. flags first (so -flavor gnu leads), then objects, then libraries.
# Libraries must follow objects: a static archive only resolves symbols that
# are already referenced by something earlier on the command line.
# ---------------------------------------------------------------------------
set(KYUZEN_LINK_RULE
    "\"${KYUZEN_LD_LLD}\" <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES> -o <TARGET>"
)

set(CMAKE_C_LINK_EXECUTABLE   "${KYUZEN_LINK_RULE}" CACHE STRING "" FORCE)
set(CMAKE_CXX_LINK_EXECUTABLE "${KYUZEN_LINK_RULE}" CACHE STRING "" FORCE)

# Static archives are also created directly by llvm-ar (the old build used
# `llvm-ar rcs`). CMake's default already does this, but pinning it keeps the
# behaviour explicit.
set(CMAKE_C_ARCHIVE_CREATE   "\"${KYUZEN_LLVM_AR}\" qc <TARGET> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND   "\"${KYUZEN_LLVM_AR}\" q  <TARGET> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH   "\"${KYUZEN_LLVM_AR}\" s  <TARGET>")
set(CMAKE_CXX_ARCHIVE_CREATE "\"${KYUZEN_LLVM_AR}\" qc <TARGET> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_APPEND "\"${KYUZEN_LLVM_AR}\" q  <TARGET> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "\"${KYUZEN_LLVM_AR}\" s  <TARGET>")
