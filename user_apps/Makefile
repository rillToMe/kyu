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
OUT_DIR   = $(ROOT_DIR)

# --- Flags Compiler 64-bit ---
# -MMD -MP: header dependency tracking (pola Makefile top-level) — perubahan
# include/*.h (mis. kyuzen_event_t di userlib.h) memaksa recompile semua app.
CFLAGS_COMMON = --target=x86_64-pc-none-elf -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -MMD -MP -I$(INC_DIR)
CFLAGS_APP    = $(CFLAGS_COMMON) -O0
CFLAGS_LIB    = $(CFLAGS_COMMON) -O2

# --- Flags Compiler C++ (libui toolkit, Phase 6) ---
# Mewarisi CFLAGS_COMMON (freestanding, -mno-*) + mematikan fitur C++ yang
# butuh runtime yang tidak ada di bare-metal:
#   -fno-exceptions         (tidak ada unwinding)
#   -fno-rtti               (tidak ada typeid/typeinfo)
#   -fno-use-cxa-atexit     (tidak ada __cxa_atexit — global dtors)
#   -fno-threadsafe-statics (tidak ada __cxa_guard_* — static locals)
# operator new/delete + __cxa_pure_virtual di-stub di apps/libui.cpp.
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
LIBUI_OBJ   = libui.o   # Widget Toolkit Modern C++ (apps/libui.cpp)
PNG_OBJ     = png.o     # Dekoder PNG bersama (apps/png.c, stb_image)

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

# ==========================================================
# TARGET UTAMA
# ==========================================================
.PHONY: all fileman viewer clock calc taskmgr notepad badptr widget_demo desktop terminal settings clean

all: $(FILEMAN_ELF) $(VIEWER_ELF) $(CLOCK_ELF) $(CALC_ELF) $(TASKMGR_ELF) $(NOTEPAD_ELF) $(BADPTR_ELF) $(WIDGET_ELF) $(DESKTOP_ELF) $(TERMINAL_ELF) $(SETTINGS_ELF)
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
# COMPILE LIBUI — Widget Toolkit (Modern C++, apps/libui.cpp)
# ==========================================================
$(LIBUI_OBJ): $(APPS_DIR)/libui.cpp
	@echo "[CXX] Mengompilasi libui (Widget Toolkit, C++ freestanding)..."
	$(CXX) $(CXXFLAGS_LIB) -I$(INC_DIR) -c $< -o $@

# ==========================================================
# COMPILE PNG — dekoder bersama (apps/png.c, stb_image)
# Dipakai Image widget (libui) — dilink ke app yang memakainya.
# ==========================================================
$(PNG_OBJ): $(APPS_DIR)/png.c
	@echo "[CC] Mengompilasi png.c (stb_image decode)..."
	$(CC) $(CFLAGS_LIB) -I$(INC_DIR) -c $< -o $@

# ==========================================================
# FILEMAN — File Manager (virtual 0x4000000, phys by OS)
# ==========================================================
fileman.o: fileman.c
	@echo "[CC] Mengompilasi fileman.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(FILEMAN_ELF): fileman.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking fileman.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) fileman.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] fileman.elf siap!"

fileman: $(FILEMAN_ELF)

# ==========================================================
# VIEWER — Image Viewer PNG (virtual 0x4000000, phys by OS)
# ==========================================================
viewer.o: viewer.c
	@echo "[CC] Mengompilasi viewer.c (O2)..."
	$(CC) $(CFLAGS_LIB) -c $< -o $@

$(VIEWER_ELF): viewer.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking viewer.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) viewer.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] viewer.elf siap!"

viewer: $(VIEWER_ELF)

# ==========================================================
# CLOCK — Jam Digital Real-Time (virtual 0x4000000, phys by OS)
# ==========================================================
clock.o: clock.c
	@echo "[CC] Mengompilasi clock.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(CLOCK_ELF): clock.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking clock.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) clock.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] clock.elf siap!"

clock: $(CLOCK_ELF)

# ==========================================================
# CALC — Kalkulator GUI (virtual 0x4000000, phys by OS)
# ==========================================================
calc.o: calc.c
	@echo "[CC] Mengompilasi calc.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(CALC_ELF): calc.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking calc.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) calc.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] calc.elf siap!"

calc: $(CALC_ELF)

# ==========================================================
# TaskManager — Task Manager GUI (virtual 0x4000000, phys by OS)
# ==========================================================
taskmgr.o: taskmgr.c
	@echo "[CC] Mengompilasi taskmgr.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(TASKMGR_ELF): taskmgr.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking taskmgr.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) taskmgr.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] taskmgr.elf siap!"

taskmgr: $(TASKMGR_ELF)

# ==========================================================
# NOTEPAD — Text Editor GUI (virtual 0x4000000, phys by OS)
# ==========================================================
notepad.o: notepad.c
	@echo "[CC] Mengompilasi notepad.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(NOTEPAD_ELF): notepad.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking notepad.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) notepad.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
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

$(TERMINAL_ELF): terminal.o $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking terminal.elf (libui toolkit + shell engine + png)..."
	$(LD) $(LDFLAGS) terminal.o $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] terminal.elf siap!"

terminal: $(TERMINAL_ELF)

# ==========================================================
# SETTINGS — Tema + info sistem (Phase 10, libui)
# ==========================================================
settings.o: settings.c
	@echo "[CC] Mengompilasi settings.c..."
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(SETTINGS_ELF): settings.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking settings.elf (libui toolkit + png)..."
	$(LD) $(LDFLAGS) settings.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] settings.elf siap!"

settings: $(SETTINGS_ELF)

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

$(WIDGET_ELF): widget_demo.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ)
	@echo "[LD] Linking widget_demo.elf (app C + libui C++ toolkit + png)..."
	$(LD) $(LDFLAGS) widget_demo.o $(USERLIB_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) -o $@
	@echo "[OK] widget_demo.elf siap!"

widget_demo: $(WIDGET_ELF)

# ==========================================================
# CLEAN
# ==========================================================
clean:
	@echo "[CLEAN] Membersihkan file objek user_apps..."
	rm -f $(APP_OBJS) $(APP_OBJS:.o=.d)
	@echo "[CLEAN] Selesai."

# --- Semua objek + header deps (harus sebelum -include) ---
APP_OBJS = $(USERLIB_OBJ) $(USERUTIL_OBJ) $(SHELLCORE_OBJ) $(LIBGUI_OBJ) $(LIBUI_OBJ) $(PNG_OBJ) fileman.o viewer.o clock.o calc.o taskmgr.o notepad.o badptr.o widget_demo.o desktop.o terminal.o settings.o
-include $(APP_OBJS:.o=.d)