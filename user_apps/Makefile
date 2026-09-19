# ==========================================================
# Kyuzen OS — User Apps Build System (64-bit)
# ==========================================================
# DESAIN BARU: SATU SLOT VIRTUAL UNTUK SEMUA APP
#
# Semua user apps di-link ke virtual address 0x4000000.
# Kernel (ELF loader) yang otomatis mencari physical RAM
# kosong dari PMM dan memetakannya ke virtual 0x4000000.
#
# Saat app selesai (exit), kernel memanggil vmm_unmap_user_space()
# yang MEMBEBASKAN semua physical pages ke PMM — RAM langsung
# tersedia kembali untuk app berikutnya.
#
# Tidak perlu lagi manual menentukan alamat RAM per app!
#
# Zona Virtual Tunggal:
#   SEMUA apps  →  0x4000000  (64 MB virtual)
#
# Penggunaan:
#   make          → Compile semua app
#   make fileman  → Compile hanya fileman.elf
#   make viewer   → Compile hanya viewer.elf
#   make clock    → Compile hanya clock.elf
#   make clean    → Hapus semua file hasil build
# ==========================================================

# --- Toolchain ---
CC    = clang
CXX   = clang++
LD    = ld.lld

# --- Direktori (relatif dari user_apps/) ---
ROOT_DIR  = ..
APPS_DIR  = $(ROOT_DIR)/apps
INC_DIR   = $(ROOT_DIR)/include
OUT_DIR   = $(ROOT_DIR)/build
$(shell mkdir -p $(OUT_DIR))

# --- Flags Compiler 64-bit ---
# -MMD -MP: header dependency tracking (pola Makefile top-level) — perubahan
# include/*.h (mis. kyuzen_event_t di userlib.h) memaksa recompile semua app.
CFLAGS_COMMON = --target=x86_64-pc-none-elf -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -MMD -MP -I$(INC_DIR) -I$(ROOT_DIR)/libs/color/include
CFLAGS_APP    = $(CFLAGS_COMMON) -O0
CFLAGS_LIB    = $(CFLAGS_COMMON) -O2

# --- Flags Compiler C++ (libui toolkit, Phase 6) ---
# Mewarisi CFLAGS_COMMON (freestanding, -mno-*) + mematikan fitur C++ yang
# butuh runtime yang tidak ada di bare-metal:
#   -fno-exceptions         (tidak ada unwinding)
#   -fno-rtti               (tidak ada typeid/typeinfo)
#   -fno-use-cxa-atexit     (tidak ada __cxa_atexit — global dtors)
#   -fno-threadsafe-statics (tidak ada __cxa_guard_* — static locals)
# operator new/delete + __cxa_pure_virtual di-stub SEKALI di
# libs/widget/src/runtime/runtime.cpp (ODR: jangan didefinisikan di file lain).
CXXFLAGS_LIB  = $(CFLAGS_COMMON) -O2 -fno-exceptions -fno-rtti \
                -fno-use-cxa-atexit -fno-threadsafe-statics -std=c++17

# --- Flags Linker 64-bit ---
LDFLAGS = -m elf_x86_64 -nostdlib -T app.ld

# --- Alamat Virtual Tunggal — OS yang alokasi physical RAM otomatis ---
APP_BASE = 0x4000000

# --- File Objek Bersama ---
USERLIB_OBJ = userlib.o
USERUTIL_OBJ = userutil.o   # helper identitas bersama (UID → username)
SHELLCORE_OBJ = shell_core.o # shell engine bersama (parser/registry/built-in)
LIBGUI_OBJ  = libgui.o
# --- Widget Toolkit Modern C++ (libs/widget/) ---
# Dulu satu file apps/libui.cpp; kini dipecah per-layer (core/primitives/editor/
# layout/containers/chrome/dialog/window/services) + abi/libui_abi.cpp. Semua .cpp
# dikompilasi jadi object terpisah lalu di-link seperti semula — tidak ada static
# ctor (ELF loader tidak menjalankan .init_array), jadi urutan link bebas.
WIDGET_DIR   = $(ROOT_DIR)/libs/widget
WIDGET_SRCS  = $(wildcard $(WIDGET_DIR)/src/*/*.cpp) $(WIDGET_DIR)/abi/libui_abi.cpp
WIDGET_BUILD = $(WIDGET_DIR)/build
WIDGET_OBJS  = $(patsubst $(WIDGET_DIR)/%.cpp,$(WIDGET_BUILD)/%.o,$(WIDGET_SRCS))
LIBUI_OBJ    = $(WIDGET_OBJS)   # Widget Toolkit (libs/widget)
PNG_OBJ     = png.o     # Dekoder PNG bersama (apps/png.c, stb_image)
COLOR_OBJS  = color_blend.o color_space.o color_utils.o   # libs/color (dipakai kernel & app)
LIBUI_COLOR_OBJS = color_utils.o   # objek libs/color yang direferensikan toolkit libui

# --- Target ELF Akhir ---
FILEMAN_ELF = $(OUT_DIR)/fileman.elf
VIEWER_ELF  = $(OUT_DIR)/viewer.elf
CLOCK_ELF   = $(OUT_DIR)/clock.elf
CALC_ELF    = $(OUT_DIR)/calc.elf
TASKMGR_ELF = $(OUT_DIR)/taskmgr.elf
NOTEPAD_ELF = $(OUT_DIR)/notepad.elf
BADPTR_ELF  = $(OUT_DIR)/badptr.elf
WIDGET_ELF  = $(OUT_DIR)/widget_demo.elf
DESKTOP_ELF  = $(OUT_DIR)/desktop.elf
TERMINAL_ELF = $(OUT_DIR)/terminal.elf
SETTINGS_ELF = $(OUT_DIR)/settings.elf
PROCINFO_ELF = $(OUT_DIR)/procinfo.elf
EXITTEST_ELF = $(OUT_DIR)/exit_test.elf
KILLTEST_ELF = $(OUT_DIR)/kill_test.elf
FDTEST_ELF   = $(OUT_DIR)/fd_test.elf
ECHOTEST_ELF = $(OUT_DIR)/echo.elf
CATTEST_ELF  = $(OUT_DIR)/cat.elf
PIPETEST_ELF = $(OUT_DIR)/pipe_test.elf
FORKTEST_ELF = $(OUT_DIR)/fork_test.elf

# ==========================================================
# TARGET UTAMA
# ==========================================================
.PHONY: all fileman viewer clock calc taskmgr notepad badptr widget_demo desktop terminal settings procinfo exit_test kill_test fd_test echo cat pipe_test fork_test clean

all: $(FILEMAN_ELF) $(VIEWER_ELF) $(CLOCK_ELF) $(CALC_ELF) $(TASKMGR_ELF) $(NOTEPAD_ELF) $(BADPTR_ELF) $(WIDGET_ELF) $(DESKTOP_ELF) $(TERMINAL_ELF) $(SETTINGS_ELF) $(PROCINFO_ELF) $(EXITTEST_ELF) $(KILLTEST_ELF) $(FDTEST_ELF) $(ECHOTEST_ELF) $(CATTEST_ELF) $(PIPETEST_ELF) $(FORKTEST_ELF)
	@echo ""
	@echo "[USER APPS] Build selesai! Semua app di virtual 0x4000000"
	@echo "  fileman.elf → $(FILEMAN_ELF)"
	@echo "  viewer.elf  → $(VIEWER_ELF)"
	@echo "  clock.elf   → $(CLOCK_ELF)"
	@echo "  calc.elf    → $(CALC_ELF)"
	@echo "  taskmgr.elf → $(TASKMGR_ELF)"
	@echo "  notepad.elf → $(NOTEPAD_ELF)"
	@echo "  widget_demo.elf → $(WIDGET_ELF)"
	@echo "  desktop.elf → $(DESKTOP_ELF)"
	@echo "  terminal.elf → $(TERMINAL_ELF)"
	@echo "  settings.elf → $(SETTINGS_ELF)"
	@echo "  OS akan otomatis alokasi physical RAM saat dijalankan."

# COMPILE SHARED USERLIB
$(USERLIB_OBJ): $(APPS_DIR)/userlib.c
	@echo "[CC] Mengompilasi userlib (64-bit)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# COMPILE USERUTIL - helper identitas bersama (current_username)
$(USERUTIL_OBJ): $(APPS_DIR)/userutil.c
	@echo "[CC] Mengompilasi userutil (identitas user)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# COMPILE SHELLCORE - shell engine bersama (shell.h)
$(SHELLCORE_OBJ): $(APPS_DIR)/shell_core.c
	@echo "[CC] Mengompilasi shell_core (shell engine)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# ==========================================================
# COMPILE LIBGUI — Kyuzen GUI Framework
# ==========================================================
$(LIBGUI_OBJ): $(APPS_DIR)/libgui.c
	@echo "[CC] Mengompilasi libgui (GUI Framework)..."
	$(CC) $(CFLAGS_LIB) -I$(INC_DIR) -c $< -o $@

# ==========================================================
# COMPILE LIBUI — Widget Toolkit (libs/widget/, satu object per .cpp)
# ==========================================================
$(WIDGET_BUILD)/%.o: $(WIDGET_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_LIB) -I$(WIDGET_DIR)/include -I$(INC_DIR) -c $< -o $@

# ==========================================================
# COMPILE PNG — dekoder bersama (apps/png.c, stb_image)
# Dipakai Image widget (libui) — dilink ke app yang memakainya.
# ==========================================================
$(PNG_OBJ): $(APPS_DIR)/png.c
	@echo "[CC] Mengompilasi png.c (stb_image decode)..."
	$(CC) $(CFLAGS_LIB) -I$(INC_DIR) -c $< -o $@

# ==========================================================
# COMPILE COLOR — libs/color bersama (header tersedia via CFLAGS_COMMON)
# Toolkit libui mereferensikan color_darken/lighten → $(LIBUI_COLOR_OBJS) ikut
# di link app GUI. App lain yang memakai blend/ruang warna menambahkan
# $(COLOR_OBJS) ke baris link-nya.
# ==========================================================
COLOR_DIR = $(ROOT_DIR)/libs/color/src

color_blend.o: $(COLOR_DIR)/color_blend.c
	@echo "[CC] Mengompilasi color_blend.c (libs/color)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

color_space.o: $(COLOR_DIR)/color_space.c
	@echo "[CC] Mengompilasi color_space.c (libs/color)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

color_utils.o: $(COLOR_DIR)/color_utils.c
	@echo "[CC] Mengompilasi color_utils.c (libs/color)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# ==========================================================
# FILEMAN — File Manager (virtual 0x4000000, phys by OS)
# ==========================================================
fileman.o: fileman.c
	@echo "[CC] Mengompilasi fileman.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(FILEMAN_ELF): fileman.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking fileman.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) fileman.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] fileman.elf siap!"

fileman: $(FILEMAN_ELF)

# ==========================================================
# VIEWER — Image Viewer PNG (virtual 0x4000000, phys by OS)
# ==========================================================
viewer.o: viewer.c
	@echo "[CC] Mengompilasi viewer.c (O2)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

$(VIEWER_ELF): viewer.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking viewer.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) viewer.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] viewer.elf siap!"

viewer: $(VIEWER_ELF)

# ==========================================================
# CLOCK — Jam Digital Real-Time (virtual 0x4000000, phys by OS)
# ==========================================================
clock.o: clock.c
	@echo "[CC] Mengompilasi clock.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(CLOCK_ELF): clock.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking clock.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) clock.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] clock.elf siap!"

clock: $(CLOCK_ELF)

# ==========================================================
# CALC — Kalkulator GUI (virtual 0x4000000, phys by OS)
# ==========================================================
calc.o: calc.c
	@echo "[CC] Mengompilasi calc.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(CALC_ELF): calc.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking calc.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) calc.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] calc.elf siap!"

calc: $(CALC_ELF)

# ==========================================================
# TaskManager — Task Manager GUI (virtual 0x4000000, phys by OS)
# ==========================================================
taskmgr.o: taskmgr.c
	@echo "[CC] Mengompilasi taskmgr.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(TASKMGR_ELF): taskmgr.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking taskmgr.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) taskmgr.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] taskmgr.elf siap!"

taskmgr: $(TASKMGR_ELF)

# ==========================================================
# NOTEPAD — Text Editor GUI (virtual 0x4000000, phys by OS)
# ==========================================================
notepad.o: notepad.c
	@echo "[CC] Mengompilasi notepad.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(NOTEPAD_ELF): notepad.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking notepad.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) notepad.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] notepad.elf siap!"

notepad: $(NOTEPAD_ELF)

# ==========================================================
# DESKTOP — Desktop Environment (Phase 10, renderer libgui)
# Shell DE: wallpaper + launcher ikon + taskbar. Event loop
# custom (render hanya saat berubah) — full-screen canvas mahal.
# ==========================================================
desktop.o: desktop.c
	@echo "[CC] Mengompilasi desktop.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(DESKTOP_ELF): desktop.o $(USERLIB_OBJ) $(LIBGUI_OBJ)
	@echo "[LD] Linking desktop.elf (libgui renderer)..."
	$(LD) $(LDFLAGS) desktop.o $(USERLIB_OBJ) $(LIBGUI_OBJ) -o $@
	@echo "[OK] desktop.elf siap!"

desktop: $(DESKTOP_ELF)

# ==========================================================
# TERMINAL — Console window (Phase 10, libui)
# Output TextEdit readonly + input TextBox; command loop userspace.
# ==========================================================
terminal.o: terminal.c
	@echo "[CC] Mengompilasi terminal.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(TERMINAL_ELF): terminal.o $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking terminal.elf (libui toolkit + shell engine + png)..."
	$(LD) $(LDFLAGS) terminal.o $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] terminal.elf siap!"

terminal: $(TERMINAL_ELF)

# ==========================================================
# SETTINGS — Tema + info sistem (Phase 10, libui)
# ==========================================================
settings.o: settings.c
	@echo "[CC] Mengompilasi settings.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(SETTINGS_ELF): settings.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking settings.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) settings.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] settings.elf siap!"

settings: $(SETTINGS_ELF)

# ==========================================================
# PROCINFO — P0 Phase 2: PID/PPID/UID/GID/argv test app
# ==========================================================
procinfo.o: procinfo.c
	@echo "[CC] Mengompilasi procinfo.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(PROCINFO_ELF): procinfo.o $(USERLIB_OBJ)
	@echo "[LD] Linking procinfo.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) procinfo.o $(USERLIB_OBJ) -o $@
	@echo "[OK] procinfo.elf siap!"

procinfo: $(PROCINFO_ELF)

# ==========================================================
# EXIT_TEST — P0 Phase 2: exit(code) test app for waitpid
# ==========================================================
exit_test.o: exit_test.c
	@echo "[CC] Mengompilasi exit_test.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(EXITTEST_ELF): exit_test.o $(USERLIB_OBJ)
	@echo "[LD] Linking exit_test.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) exit_test.o $(USERLIB_OBJ) -o $@
	@echo "[OK] exit_test.elf siap!"

exit_test: $(EXITTEST_ELF)

# ==========================================================
# KILL_TEST — P0 Phase 3: parent kills blocked child, waitpid
# ==========================================================
kill_test.o: kill_test.c
	@echo "[CC] Mengompilasi kill_test.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(KILLTEST_ELF): kill_test.o $(USERLIB_OBJ)
	@echo "[LD] Linking kill_test.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) kill_test.o $(USERLIB_OBJ) -o $@
	@echo "[OK] kill_test.elf siap!"

kill_test: $(KILLTEST_ELF)

# ==========================================================
# FD_TEST — P0 Phase 4: dup/dup2 + shared-offset test app
# ==========================================================
fd_test.o: fd_test.c
	@echo "[CC] Mengompilasi fd_test.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(FDTEST_ELF): fd_test.o $(USERLIB_OBJ)
	@echo "[LD] Linking fd_test.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) fd_test.o $(USERLIB_OBJ) -o $@
	@echo "[OK] fd_test.elf siap!"

fd_test: $(FDTEST_ELF)

# ==========================================================
# ECHO / CAT — external CLI utils for pipelines/redirection
# (P0 Phase 5; builtins can't do fd I/O, these write fd 1 / read fd 0)
# ==========================================================
echo.o: echo.c
	@echo "[CC] Mengompilasi echo.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(ECHOTEST_ELF): echo.o $(USERLIB_OBJ)
	@echo "[LD] Linking echo.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) echo.o $(USERLIB_OBJ) -o $@
	@echo "[OK] echo.elf siap!"

echo: $(ECHOTEST_ELF)

cat.o: cat.c
	@echo "[CC] Mengompilasi cat.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(CATTEST_ELF): cat.o $(USERLIB_OBJ)
	@echo "[LD] Linking cat.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) cat.o $(USERLIB_OBJ) -o $@
	@echo "[OK] cat.elf siap!"

cat: $(CATTEST_ELF)

# ==========================================================
# PIPE_TEST — P0 Phase 5: kernel pipe + dup + EOF + SMP test app
# ==========================================================
pipe_test.o: pipe_test.c
	@echo "[CC] Mengompilasi pipe_test.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(PIPETEST_ELF): pipe_test.o $(USERLIB_OBJ)
	@echo "[LD] Linking pipe_test.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) pipe_test.o $(USERLIB_OBJ) -o $@
	@echo "[OK] pipe_test.elf siap!"

pipe_test: $(PIPETEST_ELF)

# ==========================================================
# FORK_TEST — P0 Phase 6B: fork return/PID/memory/FD/pipe/kill
# ==========================================================
fork_test.o: fork_test.c
	@echo "[CC] Mengompilasi fork_test.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(FORKTEST_ELF): fork_test.o $(USERLIB_OBJ)
	@echo "[LD] Linking fork_test.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) fork_test.o $(USERLIB_OBJ) -o $@
	@echo "[OK] fork_test.elf siap!"

fork_test: $(FORKTEST_ELF)

# ==========================================================
# BADPTR — Uji boundary copy syscall (FIX_005 Tahap 2)
# ==========================================================
badptr.o: badptr.c
	@echo "[CC] Mengompilasi badptr.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(BADPTR_ELF): badptr.o $(USERLIB_OBJ)
	@echo "[LD] Linking badptr.elf ke $(APP_BASE)..."
	$(LD) $(LDFLAGS) badptr.o $(USERLIB_OBJ) -o $@
	@echo "[OK] badptr.elf siap!"

badptr: $(BADPTR_ELF)

# ==========================================================
# WIDGET_DEMO — demo Toolkit C ABI (Phase 6)
# App C murni; toolkit (libui.o) adalah C++ modern di balik C ABI.
# ==========================================================
widget_demo.o: widget_demo.c
	@echo "[CC] Mengompilasi widget_demo.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(WIDGET_ELF): widget_demo.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
	@echo "[LD] Linking widget_demo.elf (app C + libui C++ toolkit + png)..."
	$(LD) $(LDFLAGS) widget_demo.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(LIBUI_COLOR_OBJS) -o $@
	@echo "[OK] widget_demo.elf siap!"

widget_demo: $(WIDGET_ELF)

# ==========================================================
# CLEAN
# ==========================================================
clean:
	@echo "[CLEAN] Membersihkan file objek user_apps..."
	rm -f $(APP_OBJS) $(APP_OBJS:.o=.d)
	rm -rf $(WIDGET_BUILD)
	@echo "[CLEAN] Selesai."

# --- Semua objek + header deps (harus sebelum -include) ---
APP_OBJS = $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) $(COLOR_OBJS) fileman.o viewer.o clock.o calc.o taskmgr.o notepad.o badptr.o widget_demo.o desktop.o terminal.o settings.o procinfo.o exit_test.o kill_test.o fd_test.o echo.o cat.o pipe_test.o fork_test.o
-include $(APP_OBJS:.o=.d)