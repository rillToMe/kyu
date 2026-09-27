# Roadmap Phase 5 — Window Manager (KWM)

> **Status:** 5A ✅ · 5B ✅ · 5C (Focus + Dekorasi) ✅ (2026-08-05) — lanjut 5D (Alt-Tab + polish).
> **Prasyarat:** Phase 3 (Display System) ✅ — Phase 4 (Input Subsystem) ✅
> **Dok induk:** `GUI_ROADMAP.md` (arsitektur & prinsip tetap berlaku di sini).

---

## Visi Akhir (dari GUI_ROADMAP Phase 5)

Struktur target: aplikasi **tidak berkomunikasi langsung dengan kernel** — ada perantara.

```
 Kernel (Ring 0)
 ──────────────
 Scheduler, Memory Manager, VFS, KyuzenFS, Network,
 Input Driver, Framebuffer/DRM, IPC,
 Surface Manager (buffer & event)

 User Space (Ring 3)
 ──────────────
 init, login, desktopd, kwm (WM / Compositor),
 notificationd, themed, launcher

 Toolkit          : libkyuzen, libui, libwidget
 Applications     : File Manager, Browser, Terminal, Editor, Settings, Game
```

Window Manager bertugas: posisi, ukuran, z-order, **focus**, clipping, redraw, dan
**routing InputEvent (Phase 4) ke window yang tepat berdasarkan posisi/focus**.
Window = Surface = satu instance DisplayBuffer (Phase 3A). Belum ada Widget.

## Strategi Dua Langkah

Target akhir (WM di ring 3) butuh **IPC — yang belum ada**. KWM + compositor hari ini
sudah ±90% adalah "Surface Manager (buffer & event)" versi kernel; yang membedakan
target vs sekarang hanya **policy** (drag, focus, dekorasi) masih di ring 0.

- **Phase 5a (roadmap ini, KWM tetap di kernel):** tutup 3 celah struktural
  (spawn, routing, koordinat) + focus + dekorasi WM. Semantik event/surface
  terbukti dulu lewat pemakaian nyata.
- **Phase 5b (nanti, setelah IPC ada):** pindahkan *policy* ke window server
  ring-3. ABI yang distabilkan di 5a menjadi protokol IPC — **bukan kerja terbuang**.

Prinsip: Start Simple, Grow Naturally. Jangan bangun IPC demi WM; WM hari ini
adalah Surface Manager masa depan.

---

## Kondisi Awal — Apa yang Sudah Ada

| Sudah ada | Sumber |
|---|---|
| Surface per window = `DisplayBuffer*` owned | Phase 3A (`kwm_window_t.canvas`) |
| Compositor: z-order, alpha-mask, dirty region, double buffer | Phase 3B/3C |
| Drag window + bring-to-front + hit-test per klik | `kwm_process_mouse` |
| Ownership window ↔ task + destroy per-task | FIX_004 (`owner_task`, `kwm_destroy_windows_of`) |
| InputEvent lengkap: key down/up + modifier, mouse move/click/wheel | Phase 4 |
| Satu app bisa punya banyak window (`MAX_WINDOWS 16`) | KWM era |

## Celah Struktural (inti pekerjaan Phase 5)

1. **Belum ada app GUI yang jalan konkuren.** Model hari ini `sys_exec` = app
   *menggantikan* app. Tidak ada spawn → desktop multi-app mustahil.
   **→ TERTUTUP di 5A** (`sys_spawn` + task ring-3 per app).
2. **Event queue global tunggal.** Dua app yang `sys_get_event` bersamaan akan
   **saling mencuri event**. Perlu queue per-task + routing oleh KWM.
   **→ TERTUTUP di 5B** (queue per-task + KWM dispatcher).
3. **Koordinat global bocor.** App melakukan `sys_get_window_pos` + kurang manual —
   racy saat drag, diulang di setiap app. Harus dikunci: window-local.
   **→ Target 5C.**

---

## Sub-Tahap

### 5A — Spawn: app GUI konkuren ✅ (2026-07-28)

Syscall baru **`sys_spawn(path)` → task_id / -1** (nomor 57 — 1..56 terpakai).
Berbeda dari `sys_exec` (replace): membuat **task ring-3 BARU** dengan address
space baru, load ELF, lompat ke entry — caller tetap jalan (fire-and-forget;
belum ada `wait()`/exit code).

Checklist:
- [x] `kernel/elf.c` — pisahkan "load ELF ke AS target" dari teardown AS lama
      (exec = load + replace; spawn = load ke task baru). Reuse satu loader.
      **Sudah terpenuhi sejak FIX_002** (`target_pml4` eksplisit, teardown di
      caller) — tidak butuh perubahan elf.c.
- [x] `kernel/sched/lifecycle.c` — varian task ring-3 `create_user_task`
      (frame iretq `cs=0x1B, ss=0x23` seperti path exec), AS per-proses;
      `task_t.kind` (`TASK_KIND_KERNEL/SPAWNED`); pml4/cookie/kind di-set di
      dalam scheduler_lock SEBELUM READY (anti race CR3 di AP).
- [x] `kernel/syscall.c` — syscall 57; copy-in path SEBELUM apa pun
      (pola boundary ring3 tahap 2). Selama ELF load, `pml4_phys` caller
      dipinjamkan ke AS child (pola sys_exec) agar resume pasca-preempt
      me-restore CR3 yang benar.
- [x] Semantik `sys_exit` (34) untuk spawned task = **terminate task**:
      `task_exit()` membereskan AS (dead_pml4), uheap, fd, stack kernel;
      window dihancurkan duluan via `kwm_destroy_windows_of`.
      (Exec-chain lama "kembali ke shell" tetap — pembeda via `task_t.kind`.)
- [x] Launcher minimal: perintah shell **`start <app>`** (`apps/shell.c`,
      auto-suffix `.elf`) + wrapper `sys_spawn` di `userlib.h`,
      `apps/userlib.c` (ring 3), `apps/kernel_userlib.c` (shell).

**Verifikasi 5A (QEMU, 2026-07-27):** `start clock` + `start fileman` → dua
task RUNNING di CPU berbeda (sched: `tasks=3/3`, cpu1=clock.elf, cpu2=fileman.elf),
kedua window render konkuren, shell tetap responsif; ESC → exit →
`tasks=1/2` (slot DEAD, AS hancur); spawn ulang → slot dipakai ulang dengan
cookie AS baru.

**Temuan & fix bonus (2026-07-27):** Makefile tidak punya header dependency
tracking — menambah field ke `task_t` membiarkan object lain basi (scheduler
membaca `tasks[]` dengan stride struct lama → CR3 task spawned tak pernah
terpasang). Fix: `-MMD -MP` di CFLAGS + `-include` file `.d`.

**Wart diketahui (target 5B):** ketikan terkirim GANDA ke TTY buffer (shell)
dan event queue (app) — ESC menutup app tapi juga meninggalkan glyph `←` di
baris perintah shell. Ini perilaku keputusan #6 yang belum diterapkan.

**Fix lanjutan (2026-07-27):** BOSD deterministik saat ≥2 task ring-3 berbagi
satu CPU (app ke-4) — root cause: RSP0 global menimpa frame preemption.
Diperbaiki via RSP0 per-task di `schedule_on_cpu` + `TASK_STACK_SIZE` 16KB;
6 app konkuren stabil. Detail: `FIX_006_ring3_rsp0_frame_clobber.md`.

### 5B — Routing event per-task ✅ (2026-07-28)

Queue global tunggal diganti **queue per-task**; KWM = dispatcher.

- [x] `kyuzen_event_t` + field **`win_id`** (struct 16 → 20 byte; nilai = id slot
      KWM **+1**, 0 = tidak relevan). `EVENT_WIN_CLOSE` (6) dicadangkan di ABI.
      Semua `user_apps` ter-recompile (lihat temuan F1).
- [x] `kernel/event.c` — queue per-task (`task_queues[MAX_TASKS][64]`, satu
      spinlock; push polos dari IRQ, pop/flush irqsave dari syscall; flush saat
      task exit + defensif saat slot reuse di create_task_prio/create_user_task).
- [x] Routing oleh KWM:
  - **keyboard** → owner task dari **window fokus** (`kwm_route_keyboard`);
  - **mouse move/click** → window **di bawah kursor** (`kwm_route_mouse`; hover tetap jalan);
  - **wheel** → window **di bawah kursor**; area kosong → scrollback terminal.
- [x] Keyboard vs TTY: **ada window fokus → hanya event queue; tidak ada window
      → TTY buffer**. Klik area kosong = fokus -1 = jalan kembali ke shell.
- [x] syscall 29: pop dari queue **task pemanggil**; fallback sintesis
      MOUSE_MOVE **dihapus** (queue kosong → return 0; app tetap polling + yield).
- [x] Tetap polling + `sys_yield`.

**Fokus minimal (ditarik dari 5C):** routing keyboard mustahil tanpa fokus, jadi
`focused_win_id` + click-to-focus sudah diterapkan di 5B (window baru = fokus;
klik window = fokus; klik area kosong = fokus -1; destroy window fokus = refokus
ke topmost tersisa). **Tint titlebar tetap 5C.**

**Deviasi dari rencana awal:** TIDAK ada flush saat transisi fokus — routing
sudah struktural mencegah bocor; flush kbd_buffer saat transisi justru menghapus
input shell setengah-ketik yang sah. Malah sebaliknya: flush di **syscall 3**
(`sys_read_keyboard`) **dihapus** — flush-per-baca menelan ketikan yang tiba di
antara dua panggilan (terbukti memutilasi "start notepad" → "start notepa"
pada QA run pertama).

**Verifikasi 5B (QEMU headless, 2026-07-28):**
- Tanpa window: `sched` jalan normal di shell (TTY) ✓
- `start notepad` → ketik "abc"+"sched\n" → semua masuk notepad; prompt shell
  bersih tanpa glyph bocor (wart 5A tertutup) ✓
- Klik area kosong → `sched` jalan di shell; notepad tetap RUNNING ✓
- `start notepad` ×2 → dua window hidup; "222" → notepad2 ✓; drag notepad2 ✓;
  klik notepad1 → front + fokus; "111" → notepad1 (konten "...aaaa111") ✓
- ESC → menutup window yang DIKLIK (fokus); window lain tetap hidup ✓
- Kedua app exit → task ter-reap (`tasks=1/3`, pid/CR3 unik per spawn), tanpa BOSD ✓

**Temuan & fix ikutan:**
- **F1 (build):** `user_apps/Makefile` tidak punya header dependency tracking —
  app di-link dari object basi (ABI 16 byte) → BOSD saat kernel menyalin event
  20 byte. Fix: `-MMD -MP` + `-include` (pola Makefile top-level); `clean` kini
  juga menghapus `notepad.o` + file `.d`.
- **F2 (input race):** flush kbd_buffer di syscall 3 dihapus (di atas).
- **F3 (app chain, pra-5B):** notepad keluar via `sys_exec("fileman.elf")` —
  fileman hasil exec tidak bertahan (task mati senyap, tanpa window, tanpa BOSD).
  Bukan regresi 5B — perlu investigasi terpisah (calon FIX_007).

### 5C — Focus + dekorasi milik WM ✅ (2026-08-05)

- [x] `focused_win_id` di KWM; **click-to-focus** di hit-test
      (`kwm_process_mouse` sudah menemukan topmost window — tinggal set fokus).
- [x] **Dekorasi digambar compositor**: titlebar (`KWM_TITLEBAR_H` = 24 —
      memformalkan hardcode 24px/40px yang dulu duplikat di `kwm_process_mouse`
      vs libgui) + tombol close (`KWM_CLOSE_BTN_W` = 40); **tint berbeda untuk
      window fokus** (`KWM_TITLEBAR_COLOR` biru vs `KWM_TITLEBAR_INACT` abu).
- [x] **Canvas = konten murni.** `sys_kwm_create_window(w, h)` = ukuran KONTEN
      (makna parameter tidak berubah); frame = konten + titlebar, dihitung WM;
      drag/clamp terhadap frame (`height + KWM_TITLEBAR_H`).
- [x] **Koordinat event ke app = window-local** (diterjemahkan KWM saat routing
      di `kwm_route_mouse` → out_lx/out_ly). `sys_get_window_pos` (40) **dihapus**
      (handler syscall, wrapper userlib, dan semua pemakai di migrasi).
- [x] Close: WM mengirim **`EVENT_WIN_CLOSE` (6)** ke owner task — app cleanup
      lalu `sys_exit`. Bukan destroy paksa (app berhak konfirmasi/menolak nanti).
- [x] `apps/libgui.c` + semua user_apps: buang titlebar/close sendiri; konten
      digambar dari `(0,0)`.
- [x] Judul window: **belum** — titlebar polos + tint + close.
      `sys_set_window_title` ditambah saat dibutuhkan (Grow Naturally).

**Verifikasi 5C (QEMU, 2026-08-05):** build kernel + apps bersih; window app
render dengan titlebar biru milik WM + tombol close merah di kanan, konten di
bawahnya (dikonfirmasi via screendump QEMU).

**Fix bonus:** Makefile — `-include $(OBJS:.o=.d)` sebelum target `all` membuat
target pertama file `.d` (arch/x86/gdt.o) menjadi default goal → `make` hanya
membangun gdt.o. Fix `.DEFAULT_GOAL := all` di atas `-include`.

### 5D — Polish & verifikasi

- [x] **Alt-Tab**: KWM intercept keyboard sebelum routing (shortcut WM) → cycle
      fokus + bring-to-front. Hampir gratis berkat modifier Phase 4; sekaligus
      showcase key up/modifier. (2026-08-05 — `kwm_handle_shortcut` +
      `kwm_cycle_focus_locked`; build clean. Verifikasi interaktif menyusul.)
- [x] **EVENT_EXPOSE: DITUNDA** — canvas window persisten, compositor selalu bisa
      repaint dari canvas; konten tidak pernah rusak oleh oklusi/drag. Baru
      dibutuhkan saat resize/minimize ada.
- [x] Design doc `DOCUMENTATION/design/gui-phase5-wm.md` (pola doc Phase 3/4).
- [x] Update checklist ini + `GUI_ROADMAP.md`.

---

## Keputusan Terkunci (2026-07-27)

| # | Keputusan | Pilihan |
|---|---|---|
| 1 | Spawn masuk scope Phase 5 | Ya — `sys_spawn` (57), fire-and-forget, tanpa `wait()` |
| 2 | Koordinat event ke app | **Window-local**, diterjemahkan KWM saat routing |
| 3 | Model fokus | **Click-to-focus** + tint titlebar; Alt-Tab cycle |
| 4 | Dekorasi (titlebar/close) | **Milik WM**, digambar compositor; canvas = konten murni |
| 5 | Routing | Queue per-task; keyboard→fokus, mouse/wheel→di bawah kursor |
| 6 | Keyboard vs TTY | Ada window fokus → event only; tanpa window → TTY |
| 7 | ABI event | + `win_id` (struct 20 byte); `EVENT_WIN_CLOSE` = 6 |
| 8 | Close window | WM kirim `EVENT_WIN_CLOSE`; bukan destroy paksa |
| 9 | EVENT_EXPOSE | **Ditunda** — canvas persisten membuatnya belum perlu |
| 10 | Model tunggu event | Tetap polling + yield (blocking = pasca-Phase 5) |
| 11 | Ownership DisplayBuffer | (dari 3A, ditegaskan) window-owned; create/destroy oleh WM |
| 12 | Model event queue | (dari Phase 4, ditegaskan) interrupt-driven, ISR push → syscall pop |

## Definition of Done

Dari shell:

```
> start fileman.elf
> start clock.elf
```

- [x] Dua window hidup **bersamaan** di layar, keduanya berjalan konkuren.
- [x] Klik window → fokus + naik z-order + tint titlebar berubah.
- [x] Mengetik hanya diterima app fokus (demo: notepad + calc berdampingan).
- [x] Alt-Tab memutar fokus. *(diimplementasikan + build clean; verifikasi
      interaktif menyusul — focus cycle + bring-to-front)*
- [x] Drag window mulus, tanpa flicker (dirty region tetap berlaku).
- [x] Wheel di atas window → `EVENT_SCROLL` ke app itu.
- [x] Tombol close (milik WM) menutup app bersih: window hancur, task di-reap,
      heap bebas (cek dengan build `heap-watch`).
- [x] Verifikasi visual QEMU (teknik sendkey + screendump).
- [x] Build clean; design doc ditulis.

> **Phase 5 SELESAI (2026-08-05).** Langkah 5b (pindahkan policy ke window
> server ring-3) menunggu subsistem IPC — lihat keputusan #1/#13 dan visi
> `GUI_ROADMAP.md`.

## Di Luar Scope Phase 5

- Terminal-sebagai-window (shell di dalam window; butuh TTY per-task) → Phase 10.
- IPC + window server ring-3 (langkah 5b) → setelah subsistem IPC ada.
- Resize / minimize / maximize window; judul window.
- Widget / toolkit (Phase 6+); desktopd/notificationd/themed (Phase 9+).
- Blocking `sys_get_event` (wait queue) — optimisasi pasca-Phase 5.

## Risiko & Catatan Implementasi

- **ABI event 20 byte** — semua user_apps wajib recompile; `badptr` menyesuaikan.
- **Dua semantik `sys_exit`** (spawned task vs exec-chain) — pembeda via flag di
  task; jangan sampai spawned app "kembali ke shell" atau sebaliknya.
- **SMP** — task bisa migrasi CPU; queue per-task di-push dari IRQ di CPU mana
  pun → disiplin `spinlock_lock_irqsave` seperti `event.c` hari ini.
- **Refactor libgui** menyentuh semua app GUI — kerjakan dalam satu tema perubahan
  (5C), jangan dicampur dengan 5A/5B.
- **Memori** — queue 64 event × 20 byte × MAX_TASKS: kecil; canvas window tetap
  dibatasi `KWM_MAX_CANVAS_BYTES`.
