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
#   make clean    → Hapus semua hasil build user_apps
#
# --- Tata letak output (tidak ada object di source tree) ---
#   ../build/obj/user/apps/*.o           library bersama (apps/*.c)
#   ../build/obj/user/user_apps/*.o      object app (user_apps/*.c)
#   ../build/obj/user/libs/color/src/*.o libs/color
#   ../build/obj/user/libs/widget/**/*.o toolkit C++ (libs/widget)
#   ../build/apps/*.elf                  ELF akhir (di-copy ke ISO oleh root)
#
# Satu rule generik per kategori kompilasi; menambah app = tambah namanya ke
# GUI_APPS/SIMPLE_APPS/OTHER_APPS saja (target ELF, alias, dan object-nya ikut
# otomatis). Dependency header tetap via -MMD -MP (file .d di-include di bawah).
# ==========================================================

# --- Toolchain ---
CC    = clang
CXX   = clang++
LD    = ld.lld

# --- Direktori (relatif dari user_apps/) ---
ROOT_DIR  = ..
APPS_DIR  = $(ROOT_DIR)/apps
INC_DIR   = $(ROOT_DIR)/include
BUILD_DIR = $(ROOT_DIR)/build
OBJ_DIR   = $(BUILD_DIR)/obj/user
ELF_DIR   = $(BUILD_DIR)/apps

# --- Flags Compiler 64-bit ---
# -MMD -MP: header dependency tracking (pola Makefile top-level) — perubahan
# include/*.h (mis. kyuzen_event_t di userlib.h) memaksa recompile semua app
# yang benar-benar meng-include-nya (bukan semua app).
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

# --- Kyuzen C SDK (LLVM libc 22.1.8) — hanya untuk app yang memakainya ---
# Di-stage oleh root Makefile (`make sdk-c` → ../build/sdk/c) SEBELUM
# sub-make ini berjalan. Saat ini: clock (snprintf/strlen). App lain tetap
# murni userlib/libui (ENTRY main, tanpa libc).
SDK_DIR = $(ROOT_DIR)/build/sdk/c
SDK_INC = $(SDK_DIR)/include
SDK_LIB = $(SDK_DIR)/lib/libc.a
SDK_CRT = $(SDK_DIR)/crt/crt.o
SDK_LD  = $(SDK_DIR)/linker/app.ld

# ==========================================================
# OBJECT BERSAMA
# ==========================================================
# --- Widget Toolkit Modern C++ (libs/widget/) ---
# Dulu satu file apps/libui.cpp; kini dipecah per-layer (core/primitives/editor/
# layout/containers/chrome/dialog/window/services) + abi/libui_abi.cpp. Semua .cpp
# dikompilasi jadi object terpisah lalu di-link seperti semula — tidak ada static
# ctor (ELF loader tidak menjalankan .init_array), jadi urutan link bebas.
WIDGET_DIR  = $(ROOT_DIR)/libs/widget
WIDGET_SRCS = $(wildcard $(WIDGET_DIR)/src/*/*.cpp) $(WIDGET_DIR)/abi/libui_abi.cpp
WIDGET_OBJS = $(patsubst $(ROOT_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(WIDGET_SRCS))

# --- libs/color (dipakai kernel & app) ---
COLOR_DIR       = $(ROOT_DIR)/libs/color/src
COLOR_SRCS      = $(COLOR_DIR)/color_blend.c $(COLOR_DIR)/color_space.c $(COLOR_DIR)/color_utils.c
COLOR_OBJS      = $(patsubst $(ROOT_DIR)/%.c,$(OBJ_DIR)/%.o,$(COLOR_SRCS))
LIBUI_COLOR_OBJS = $(OBJ_DIR)/libs/color/src/color_utils.o

USERLIB_OBJ    = $(OBJ_DIR)/apps/userlib.o    # int $0x80 syscall wrappers
USERUTIL_OBJ   = $(OBJ_DIR)/apps/userutil.o   # helper identitas bersama (UID → username)
SHELLCORE_OBJ  = $(OBJ_DIR)/apps/shell_core.o # shell engine bersama (parser/registry/built-in)
LIBGUI_OBJ     = $(OBJ_DIR)/apps/libgui.o     # GUI framework lama (font8x16, dll)
PNG_OBJ        = $(OBJ_DIR)/apps/png.o        # dekoder PNG bersama (apps/png.c, stb_image)

# ==========================================================
# DAFTAR APP
# ==========================================================
# Nama app = nama target = nama ELF. Sinkron dengan manifests/*.app,
# limine.conf, dan APP_NAMES di Makefile root.
#   GUI_APPS    : link libui toolkit + png + color_utils
#   SIMPLE_APPS : hanya butuh userlib
#   OTHER_APPS  : desktop (libgui saja) dan terminal (shell engine + toolkit)
GUI_APPS    = fileman viewer clock calc taskmgr notepad settings widget_demo
SIMPLE_APPS = procinfo exit_test kill_test fd_test echo cat pipe_test fork_test badptr
OTHER_APPS  = desktop terminal
APP_NAMES   = $(GUI_APPS) $(SIMPLE_APPS) $(OTHER_APPS)

ALL_ELFS = $(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(APP_NAMES)))
APP_OBJS = $(addprefix $(OBJ_DIR)/user_apps/,$(addsuffix .o,$(APP_NAMES)))
ALL_OBJS = $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) \
           $(WIDGET_OBJS) $(PNG_OBJ) $(COLOR_OBJS) $(APP_OBJS)

# Direktori object dibuat sekali (order-only) — aman `make -j8` dan tidak
# memanggil `mkdir` untuk tiap object (mahal di Windows).
OBJ_DIRS = $(sort $(dir $(ALL_OBJS)))
$(OBJ_DIRS):
	@mkdir -p $@

# Default goal dipatok eksplisit: rule direktori object di atas (dan file .d
# yang di-include di bawah) bisa jadi "target pertama" dan mencuri default goal.
.DEFAULT_GOAL := all

.PHONY: all $(APP_NAMES) clean
all: $(ALL_ELFS)
	@echo ""
	@echo "[USER APPS] Build selesai — $(words $(APP_NAMES)) ELF di $(ELF_DIR)/"
	@echo "  (virtual 0x4000000; OS mengalokasikan physical RAM saat dijalankan)"

# ==========================================================
# COMPILE (satu rule generik per kategori)
# ==========================================================
# Library bersama (apps/*.c, dipakai juga oleh test host) — -O2
$(OBJ_DIR)/apps/%.o: $(APPS_DIR)/%.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# App C di user_apps/ — -O0 (seperti sebelumnya). Path sumber relatif ke
# direktori ini (make selalu dijalankan dari user_apps/, termasuk saat dipanggil
# root Makefile via $(MAKE) -C user_apps).
$(OBJ_DIR)/user_apps/%.o: %.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_APP) -c $< -o $@

# viewer tetap -O2 (memproses gambar, sengaja dioptimasi)
$(OBJ_DIR)/user_apps/viewer.o: viewer.c | $(OBJ_DIRS)
	@echo "[CC] $< (O2)"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# clock memakai Kyuzen C SDK (snprintf/strlen dari LLVM libc): header SDK
# via -isystem (JANGAN -I third_party/... — app hanya kenal SDK).
$(OBJ_DIR)/user_apps/clock.o: clock.c | $(OBJ_DIRS)
	@echo "[CC-SDK] $<"
	$(CC) $(CFLAGS_LIB) -isystem $(SDK_INC) -c $< -o $@

# libs/color — -O2 (header tersedia via CFLAGS_COMMON)
$(OBJ_DIR)/libs/color/src/%.o: $(ROOT_DIR)/libs/color/src/%.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# Toolkit C++ (libs/widget/src/<layer>/*.cpp + libs/widget/abi/libui_abi.cpp)
$(OBJ_DIR)/libs/widget/%.o: $(WIDGET_DIR)/%.cpp | $(OBJ_DIRS)
	@echo "[CXX] $<"
	$(CXX) $(CXXFLAGS_LIB) -I$(WIDGET_DIR)/include -I$(INC_DIR) -c $< -o $@

# ==========================================================
# LINK (satu rule per grup; isi link identik dengan sebelumnya)
# ==========================================================
GUI_LIBS     = $(USERLIB_OBJ) $(LIBGUI_OBJ) $(WIDGET_OBJS) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
TERMINAL_LIBS= $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(WIDGET_OBJS) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
DESKTOP_LIBS = $(USERLIB_OBJ) $(LIBGUI_OBJ)

# Link generic: ELF dibangun dari object app-nya + library grup-nya.
# clock DIKECUALIKAN dari pola ini — ia di-link via SDK (ENTRY _start).
$(filter-out $(ELF_DIR)/clock.elf,$(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(GUI_APPS)))): $(ELF_DIR)/%.elf: $(OBJ_DIR)/user_apps/%.o $(GUI_LIBS)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] $(notdir $@) (libui toolkit + png)"
	$(LD) $(LDFLAGS) $< $(GUI_LIBS) -o $@

# clock: GUI libs + SDK crt/lib/ld (LLVM libc). libc.a PALING AKHIR
# (archive: simbol diambil kiri-ke-kanan).
$(ELF_DIR)/clock.elf: $(OBJ_DIR)/user_apps/clock.o $(GUI_LIBS) $(SDK_CRT) $(SDK_LIB)
	@mkdir -p $(ELF_DIR)
	@echo "[LD-SDK] clock.elf (libui toolkit + Kyuzen C SDK)"
	$(LD) -m elf_x86_64 -nostdlib -T $(SDK_LD) $< $(GUI_LIBS) $(SDK_CRT) $(SDK_LIB) -o $@

$(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(SIMPLE_APPS))): $(ELF_DIR)/%.elf: $(OBJ_DIR)/user_apps/%.o $(USERLIB_OBJ)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] $(notdir $@) ke $(APP_BASE)"
	$(LD) $(LDFLAGS) $< $(USERLIB_OBJ) -o $@

$(ELF_DIR)/desktop.elf: $(OBJ_DIR)/user_apps/desktop.o $(DESKTOP_LIBS)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] desktop.elf (libgui renderer)"
	$(LD) $(LDFLAGS) $< $(DESKTOP_LIBS) -o $@

$(ELF_DIR)/terminal.elf: $(OBJ_DIR)/user_apps/terminal.o $(TERMINAL_LIBS)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] terminal.elf (libui toolkit + shell engine + png)"
	$(LD) $(LDFLAGS) $< $(TERMINAL_LIBS) -o $@

# Alias lama: `make fileman` → ../build/apps/fileman.elf
$(APP_NAMES): %: $(ELF_DIR)/%.elf

# ==========================================================
# CLEAN
# ==========================================================
clean:
	@echo "[CLEAN] Menghapus object + ELF user_apps..."
	rm -rf $(OBJ_DIR) $(ELF_DIR)
	rm -rf $(ROOT_DIR)/libs/widget/build
	@echo "[CLEAN] Selesai."

# --- Header deps hasil -MMD (di-include paling akhir) ---
-include $(ALL_OBJS:.o=.d)
