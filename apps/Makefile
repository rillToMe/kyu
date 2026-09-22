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
#   make clean    → Hapus semua hasil build apps
#
# --- Tata letak output (tidak ada object di source tree) ---
#   ../build/obj/user/libs/core/*.o      library user-space (libs/core/*.c)
#   ../build/obj/user/system/*.o         engine shell + util CLI (system/*.c)
#   ../build/obj/user/apps/*.o           object app (apps/*.c)
#   ../build/obj/user/libs/media/*.o     lib media/gambar (libs/media/*.c)
#   ../build/obj/user/tests/*.o          object ELF uji (tests/target/*.c)
#   ../build/obj/user/libs/gui/color/src/*.o libs/gui/color
#   ../build/obj/user/libs/gui/widget/**/*.o toolkit C++ (libs/gui/widget)
#   ../build/obj/user/apps/<app>/*.o     object app C++ (apps/<app>/*.cpp)
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

# --- Direktori (relatif dari apps/) ---
ROOT_DIR  = ..
LIB_DIR   = $(ROOT_DIR)/libs/core
SYS_DIR   = $(ROOT_DIR)/system
TEST_DIR  = $(ROOT_DIR)/tests/target
MEDIA_DIR = $(ROOT_DIR)/libs/media
INC_DIR   = $(ROOT_DIR)/include
BUILD_DIR = $(ROOT_DIR)/build
OBJ_DIR   = $(BUILD_DIR)/obj/user
ELF_DIR   = $(BUILD_DIR)/apps

# --- Flags Compiler 64-bit ---
# -MMD -MP: header dependency tracking (pola Makefile top-level) — perubahan
# include/*.h (mis. kyuzen_event_t di userlib.h) memaksa recompile semua app
# yang benar-benar meng-include-nya (bukan semua app).
CFLAGS_COMMON = --target=x86_64-pc-none-elf -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -MMD -MP -I$(INC_DIR) -I$(ROOT_DIR)/libs/gui/color/include
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
# libs/gui/widget/src/runtime/runtime.cpp (ODR: jangan didefinisikan di file lain).
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
# --- Widget Toolkit Modern C++ (libs/gui/widget/) ---
# Dulu satu file apps/libui.cpp; kini dipecah per-layer (core/primitives/editor/
# layout/containers/chrome/dialog/window/services) + abi/libui_abi.cpp. Semua .cpp
# dikompilasi jadi object terpisah lalu di-link seperti semula — tidak ada static
# ctor (ELF loader tidak menjalankan .init_array), jadi urutan link bebas.
WIDGET_DIR  = $(ROOT_DIR)/libs/gui/widget
WIDGET_SRCS = $(wildcard $(WIDGET_DIR)/src/*/*.cpp) $(WIDGET_DIR)/abi/libui_abi.cpp
WIDGET_OBJS = $(patsubst $(ROOT_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(WIDGET_SRCS))

# --- libs/gui/color (dipakai kernel & app) ---
COLOR_DIR       = $(ROOT_DIR)/libs/gui/color/src
COLOR_SRCS      = $(COLOR_DIR)/color_blend.c $(COLOR_DIR)/color_space.c $(COLOR_DIR)/color_utils.c
COLOR_OBJS      = $(patsubst $(ROOT_DIR)/%.c,$(OBJ_DIR)/%.o,$(COLOR_SRCS))
LIBUI_COLOR_OBJS = $(OBJ_DIR)/libs/gui/color/src/color_utils.o

USERLIB_OBJ    = $(OBJ_DIR)/libs/core/userlib.o  # int $0x80 syscall wrappers
USERUTIL_OBJ   = $(OBJ_DIR)/libs/core/userutil.o # helper identitas bersama (UID → username)
SHELLCORE_OBJ  = $(OBJ_DIR)/system/shell_core.o # shell engine bersama (parser/registry/built-in)
LIBGUI_OBJ     = $(OBJ_DIR)/libs/core/libgui.o   # GUI framework lama (font8x16, dll)
PNG_OBJ        = $(OBJ_DIR)/libs/media/png.o   # dekoder gambar bersama (libs/media/png.c: PNG + BMP)
MEDIA_OBJ      = $(OBJ_DIR)/libs/media/media.o # abstraksi media bersama (tipe/path/scan/probe)

# ==========================================================
# DAFTAR APP
# ==========================================================
# Nama app = nama target = nama ELF. Sinkron dengan manifests/*.app,
# limine.conf, dan APP_NAMES di Makefile root.
#   GUI_APPS    : link libui toolkit + png + color_utils
#   SYS_APPS    : util CLI pendamping shell, sumber di system/ (hanya butuh userlib)
#   TEST_APPS   : ELF uji kernel/syscall, sumber di tests/ (hanya butuh userlib)
#   SIMPLE_APPS : gabungan SYS_APPS + TEST_APPS (rule link sama, sumber beda)
#   OTHER_APPS  : terminal (shell engine + toolkit). Desktop TIDAK di sini —
#                 dibangun root Makefile dari system/$(DESKTOP_APP)/ via
#                 libdesktop + C++ SDK (aturan Phase 8), output tetap
#                 ../build/apps/desktop.elf (slot boot).
#   CPP_APPS    : C++ (gallery/imageview). Sumber di subdirektori app sendiri
#                 (apps/<app>/*.cpp) karena tiap app punya >1 modul; pakai
#                 toolkit libui lewat ABI C + libs/gui/color + png + media.
#                 Bukan kelompok lain: rule kompilasi/link-nya beda (clang++,
#                 subdir, -I libs/gui/widget/include).
GUI_APPS    = viewer clock calc taskmgr notepad settings widget_demo
SYS_APPS    = echo cat
TEST_APPS   = procinfo exit_test kill_test fd_test pipe_test fork_test badptr
SIMPLE_APPS = $(SYS_APPS) $(TEST_APPS)
OTHER_APPS  = terminal
CPP_APPS    = gallery imageview
APP_NAMES   = $(GUI_APPS) $(SIMPLE_APPS) $(OTHER_APPS)
ALL_NAMES   = $(APP_NAMES) $(CPP_APPS)

# Objek app C++ (satu baris per app = daftar modulnya).
GALLERY_OBJS   = $(addprefix $(OBJ_DIR)/apps/gallery/,$(addsuffix .o,main gallery thumbs))
IMAGEVIEW_OBJS = $(addprefix $(OBJ_DIR)/apps/imageview/,$(addsuffix .o,main viewer image))
CPP_APP_OBJS   = $(GALLERY_OBJS) $(IMAGEVIEW_OBJS)

ALL_ELFS = $(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(ALL_NAMES)))
APP_OBJS = $(addprefix $(OBJ_DIR)/apps/,$(addsuffix .o,$(GUI_APPS) $(OTHER_APPS)))
SYS_OBJS = $(addprefix $(OBJ_DIR)/system/,$(addsuffix .o,$(SYS_APPS)))
TEST_OBJS = $(addprefix $(OBJ_DIR)/tests/,$(addsuffix .o,$(TEST_APPS)))
ALL_OBJS = $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) \
           $(WIDGET_OBJS) $(PNG_OBJ) $(MEDIA_OBJ) $(COLOR_OBJS) $(APP_OBJS) \
           $(SYS_OBJS) $(TEST_OBJS) $(CPP_APP_OBJS)

# Direktori object dibuat sekali (order-only) — aman `make -j8` dan tidak
# memanggil `mkdir` untuk tiap object (mahal di Windows).
OBJ_DIRS = $(sort $(dir $(ALL_OBJS)))
$(OBJ_DIRS):
	@mkdir -p $@

# Default goal dipatok eksplisit: rule direktori object di atas (dan file .d
# yang di-include di bawah) bisa jadi "target pertama" dan mencuri default goal.
.DEFAULT_GOAL := all

.PHONY: all $(ALL_NAMES) clean
all: $(ALL_ELFS)
	@echo ""
	@echo "[USER APPS] Build selesai — $(words $(ALL_NAMES)) ELF di $(ELF_DIR)/"
	@echo "  (virtual 0x4000000; OS mengalokasikan physical RAM saat dijalankan)"

# ==========================================================
# COMPILE (satu rule generik per kategori)
# ==========================================================
# Library bersama libs/core/ (userlib/userutil/libgui, dipakai juga oleh test host) — -O2
$(OBJ_DIR)/libs/core/%.o: $(LIB_DIR)/%.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# Engine shell + util CLI system/ (shell_core/echo/cat) — -O2
$(OBJ_DIR)/system/%.o: $(SYS_DIR)/%.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# App C di apps/ (GUI/terminal) — -O0 (seperti sebelumnya). Path sumber
# relatif ke direktori ini (make selalu dijalankan dari apps/, termasuk saat
# dipanggil root Makefile via $(MAKE) -C apps).
$(OBJ_DIR)/apps/%.o: %.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_APP) -c $< -o $@

# Lib media/gambar libs/media/ (png/media) — -O2
$(OBJ_DIR)/libs/media/%.o: $(MEDIA_DIR)/%.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# ELF uji di tests/ — -O0
$(OBJ_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_APP) -c $< -o $@

# viewer tetap -O2 (memproses gambar, sengaja dioptimasi)
$(OBJ_DIR)/apps/viewer.o: viewer.c | $(OBJ_DIRS)
	@echo "[CC] $< (O2)"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# clock memakai Kyuzen C SDK (snprintf/strlen dari LLVM libc): header SDK
# via -isystem (JANGAN -I third_party/... — app hanya kenal SDK).
$(OBJ_DIR)/apps/clock.o: clock.c | $(OBJ_DIRS)
	@echo "[CC-SDK] $<"
	$(CC) $(CFLAGS_LIB) -isystem $(SDK_INC) -c $< -o $@

# libs/gui/color — -O2 (header tersedia via CFLAGS_COMMON)
$(OBJ_DIR)/libs/gui/color/src/%.o: $(ROOT_DIR)/libs/gui/color/src/%.c | $(OBJ_DIRS)
	@echo "[CC] $<"
	$(CC) $(CFLAGS_LIB) -c $< -o $@

# Toolkit C++ (libs/gui/widget/src/<layer>/*.cpp + libs/gui/widget/abi/libui_abi.cpp)
$(OBJ_DIR)/libs/gui/widget/%.o: $(WIDGET_DIR)/%.cpp | $(OBJ_DIRS)
	@echo "[CXX] $<"
	$(CXX) $(CXXFLAGS_LIB) -I$(WIDGET_DIR)/include -I$(INC_DIR) -c $< -o $@

# App C++ (apps/gallery/*.cpp, apps/imageview/*.cpp).
# Flag = CXXFLAGS_LIB (freestanding, -fno-exceptions/-fno-rtti, tanpa libc++) +
# -I libs/gui/widget/include (hanya bila app menyentuh header toolkit; app baru ini
# memakai ABI C libui.h, jadi include path itu opsional — dibiarkan agar app
# C++ berikutnya tidak perlu mengubah Makefile).
# CATATAN PENTING: tidak ada libc++ dan .init_array TIDAK dijalankan loader
# (ENTRY main di app.ld) → jangan ada objek global ber-konstruktor; app baru
# membuat objeknya di dalam main().
$(OBJ_DIR)/apps/gallery/%.o: gallery/%.cpp | $(OBJ_DIRS)
	@echo "[CXX] $<"
	$(CXX) $(CXXFLAGS_LIB) -I$(WIDGET_DIR)/include -I$(INC_DIR) -c $< -o $@

$(OBJ_DIR)/apps/imageview/%.o: imageview/%.cpp | $(OBJ_DIRS)
	@echo "[CXX] $<"
	$(CXX) $(CXXFLAGS_LIB) -I$(WIDGET_DIR)/include -I$(INC_DIR) -c $< -o $@

# ==========================================================
# LINK (satu rule per grup; isi link identik dengan sebelumnya)
# ==========================================================
GUI_LIBS     = $(USERLIB_OBJ) $(LIBGUI_OBJ) $(WIDGET_OBJS) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
TERMINAL_LIBS= $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(WIDGET_OBJS) $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
# App media (gallery/imageview): toolkit + png + media (deteksi tipe, scan
# direktori, probe header) — media.o dipakai KEDUA app.
MEDIA_LIBS   = $(GUI_LIBS) $(MEDIA_OBJ)

# Link generic: ELF dibangun dari object app-nya + library grup-nya.
# clock DIKECUALIKAN (di-link via SDK, ENTRY _start); fileman.elf TIDAK di
# sini — ia dibangun root Makefile dari apps/filemanager/ (C++ SDK).
$(filter-out $(ELF_DIR)/clock.elf,$(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(GUI_APPS)))): $(ELF_DIR)/%.elf: $(OBJ_DIR)/apps/%.o $(GUI_LIBS)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] $(notdir $@) (libui toolkit + png)"
	$(LD) $(LDFLAGS) $< $(GUI_LIBS) -o $@

# clock: GUI libs + SDK crt/lib/ld (LLVM libc). libc.a PALING AKHIR
# (archive: simbol diambil kiri-ke-kanan).
$(ELF_DIR)/clock.elf: $(OBJ_DIR)/apps/clock.o $(GUI_LIBS) $(SDK_CRT) $(SDK_LIB)
	@mkdir -p $(ELF_DIR)
	@echo "[LD-SDK] clock.elf (libui toolkit + Kyuzen C SDK)"
	$(LD) -m elf_x86_64 -nostdlib -T $(SDK_LD) $< $(GUI_LIBS) $(SDK_CRT) $(SDK_LIB) -o $@

# Util CLI system/ (echo/cat): sumber di system/
$(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(SYS_APPS))): $(ELF_DIR)/%.elf: $(OBJ_DIR)/system/%.o $(USERLIB_OBJ)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] $(notdir $@) ke $(APP_BASE)"
	$(LD) $(LDFLAGS) $< $(USERLIB_OBJ) -o $@

# ELF uji (tests/): sumber di tests/
$(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(TEST_APPS))): $(ELF_DIR)/%.elf: $(OBJ_DIR)/tests/%.o $(USERLIB_OBJ)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] $(notdir $@) ke $(APP_BASE)"
	$(LD) $(LDFLAGS) $< $(USERLIB_OBJ) -o $@

$(ELF_DIR)/terminal.elf: $(OBJ_DIR)/apps/terminal.o $(TERMINAL_LIBS)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] terminal.elf (libui toolkit + shell engine + png)"
	$(LD) $(LDFLAGS) $< $(TERMINAL_LIBS) -o $@

# App C++ media: objek app pertama (berisi entry `main`), lalu library bersama.
$(ELF_DIR)/gallery.elf: $(GALLERY_OBJS) $(MEDIA_LIBS)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] gallery.elf (C++, libui toolkit + media + png)"
	$(LD) $(LDFLAGS) $(GALLERY_OBJS) $(MEDIA_LIBS) -o $@

$(ELF_DIR)/imageview.elf: $(IMAGEVIEW_OBJS) $(MEDIA_LIBS)
	@mkdir -p $(ELF_DIR)
	@echo "[LD] imageview.elf (C++, libui toolkit + media + png)"
	$(LD) $(LDFLAGS) $(IMAGEVIEW_OBJS) $(MEDIA_LIBS) -o $@

# Alias lama: `make fileman` → ../build/apps/fileman.elf
$(ALL_NAMES): %: $(ELF_DIR)/%.elf

# ==========================================================
# CLEAN
# ==========================================================
clean:
	@echo "[CLEAN] Menghapus object + ELF apps..."
	rm -rf $(OBJ_DIR) $(ELF_DIR)
	rm -rf $(ROOT_DIR)/libs/gui/widget/build
	@echo "[CLEAN] Selesai."

# --- Header deps hasil -MMD (di-include paling akhir) ---
-include $(ALL_OBJS:.o=.d)
