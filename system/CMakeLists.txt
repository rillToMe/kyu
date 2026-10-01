# ============================================================================
# system/CMakeLists.txt — system utilities + the shell engine.
#
# These are ELFs that run in Ring 3 (ENTRY main), NOT kernel tasks. The old
# Makefile excluded every one of them from C_SOURCES for that reason — the
# kernel used to call into them directly at CPL0, which is what the Ring-3
# migration removed (docs/design/ring3-init-migration.md).
#
#   echo cat zen init login shell        -> boot-critical utilities
#   shell_core.c                          -> shared shell engine (parser,
#                                            registry, built-ins)
#
# desktop/ is NOT here: it is an SDK app (C++), built in apps/CMakeLists.txt.
# ============================================================================

set(_sys_dir "${KYUZEN_ROOT}/system")

# ---------------------------------------------------------------------------
# Shared shell engine. Not an executable: linked into shell.elf, terminal.elf,
# and (for host tests) compiled directly by tests/host.
# ---------------------------------------------------------------------------
add_library(kyuzen-shellcore OBJECT "${_sys_dir}/shell_core.c")
target_link_libraries(kyuzen-shellcore PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-shellcore PUBLIC ${KYUZEN_ROOT}/include)

# ---------------------------------------------------------------------------
# Plain utilities: userlib only.
#
# FLAGS kyuzen-flags-applib (-O2): system/*.c is compiled -O2 in the old
# apps/Makefile — $(OBJ_DIR)/system/%.o uses CFLAGS_LIB, not CFLAGS_APP. The
# utilities are treated as library code, not as app code. Using the app set
# here would change the generated code and break binary parity.
# ---------------------------------------------------------------------------
foreach(_app echo cat zen)
    kyuzen_add_plain_app(kyuzen-${_app}
        SOURCES "${_sys_dir}/${_app}.c"
        FLAGS   kyuzen-flags-applib
        LIBS    kyuzen-userlib
    )
endforeach()

# ---------------------------------------------------------------------------
# init.elf — PID 1 supervisor.
#
# Spawns login.elf, restarts it with exponential backoff (1 s -> 30 s). Only
# userlib is needed: spawning goes through syscalls.
# ---------------------------------------------------------------------------
kyuzen_add_plain_app(kyuzen-init
    SOURCES "${_sys_dir}/init.c"
    FLAGS   kyuzen-flags-applib
    LIBS    kyuzen-userlib
)

# ---------------------------------------------------------------------------
# login.elf — authentication + desktop/shell launch.
#
# Links userutil (parses users.sys for credentials).
# After successful auth it spawns desktop.elf and shell.elf. The uid is passed
# to the shell via argv[1] and the shell calls sys_set_uid() itself — the
# order matters, because cred_inherit() would otherwise hand the shell a
# non-root uid and root-only commands would be permanently denied.
# ---------------------------------------------------------------------------
kyuzen_add_plain_app(kyuzen-login
    SOURCES "${_sys_dir}/login.c"
    FLAGS   kyuzen-flags-applib
    LIBS    kyuzen-userlib kyuzen-userutil
)

# ---------------------------------------------------------------------------
# shell.elf — console frontend.
#
# Links the shared shell engine plus userutil (auth/adduser) and netutil
# (nettest). Same engine as terminal.elf, without the GUI toolkit.
# ---------------------------------------------------------------------------
kyuzen_add_plain_app(kyuzen-shell
    SOURCES "${_sys_dir}/shell.c"
    FLAGS   kyuzen-flags-applib
    LIBS    kyuzen-userlib kyuzen-userutil kyuzen-netutil kyuzen-shellcore
)

# ---------------------------------------------------------------------------
# Register every system ELF so the ISO image depends on the real targets.
# ---------------------------------------------------------------------------
set(KYUZEN_SYSTEM_APP_TARGETS
    kyuzen-echo
    kyuzen-cat
    kyuzen-zen
    kyuzen-init
    kyuzen-login
    kyuzen-shell
    PARENT_SCOPE
)
