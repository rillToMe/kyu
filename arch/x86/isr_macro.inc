; ============================================================
; arch/x86/isr_macro.inc — PUSHA64 / POPA64 untuk Kyuzen OS
;
; STACK FRAME LAYOUT setelah PUSHA64 (RSP → atas stack):
;
;   [RSP+  0]  r15   ← RSP saat ini (pointer ke registers_t)
;   [RSP+  8]  r14
;   [RSP+ 16]  r13
;   [RSP+ 24]  r12
;   [RSP+ 32]  r11
;   [RSP+ 40]  r10
;   [RSP+ 48]  r9
;   [RSP+ 56]  r8
;   [RSP+ 64]  rdi
;   [RSP+ 72]  rsi
;   [RSP+ 80]  rbp
;   [RSP+ 88]  rdx
;   [RSP+ 96]  rcx
;   [RSP+104]  rbx
;   [RSP+112]  rax   ← isr128: mov [rsp+112], rax (return value)
;   [RSP+120]  int_num   (push INT_NUM by stub)
;   [RSP+128]  error_code (push 0 by stub, FIRST push = higher addr)
;   [RSP+136]  rip        (CPU auto-push on interrupt)
;   [RSP+144]  cs
;   [RSP+152]  rflags
;   [RSP+160]  rsp        (user/old RSP)
;   [RSP+168]  ss
;
; Ini cocok 1-to-1 dengan struct registers_t di task.h:
;   typedef struct { r15, r14, ..., rax, int_num, error_code,
;                    rip, cs, rflags, rsp, ss } registers_t;
;
; PENTING: Jangan ubah urutan PUSH/POP ini!
;          Seluruh ISR handler dan Preemptive Scheduler bergantung pada layout ini.
; ============================================================

; ============================================================
; KZ_ENTRY_PROLOGUE — normalisasi RFLAGS di pintu masuk kernel (ARCH-1)
;
; WAJIB dipanggil PALING AWAL di setiap entry stub, sebelum `push` apa pun.
;
;   cld   — DF (Direction Flag) harus 0 di kernel. ABI x86-64 mensyaratkan
;           DF=0 saat masuk fungsi, tapi CPU TIDAK menormalkannya saat
;           interrupt/trap. DF dapat di-set dari CPL3 (`pushfq; or [rsp],0x400;
;           popfq`) tanpa IOPL. Jika bocor ke kernel, `rep movs` di
;           kernel/gfx/kwm.c (blit canvas) menyalin MUNDUR → korupsi heap kernel.
;
;   clac  — AC (Alignment Check) harus 0 di kernel. AC juga dapat di-set dari
;           CPL3. Selama AC=1, SMAP TIDAK AKTIF (gerbang SMAP adalah AC, bukan
;           CR0.AM). Jadi AC yang bocor dari user MEMATIKAN seluruh proteksi
;           SMAP untuk handler berjalan → kernel bebas menyentuh halaman US=1
;           → validasi pointer user di usercopy.c bisa dilewati.
;
; `clac` hanya dieksekusi bila CR4.SMAP benar-benar aktif: pada CPU tanpa SMAP
; instruksi itu #UD. g_smap_enabled konstan setelah boot (di-set di
; cpu_enable_smap_smep SEBELUM CR4 ditulis), jadi branch-nya 100% predictable
; dan biayanya ~2 cycle — jauh lebih murah daripada risiko #UD.
;
; CATATAN: `clac` di-encode sebagai opcode asli (bukan .byte) karena NASM 2.16
; sudah mendukungnya. smap.h memakai .byte hanya karena clang < 4.9 tidak
; mengenal stac/clac — keterbatasan itu tidak berlaku untuk NASM.
; ============================================================

extern g_smap_enabled

%macro KZ_ENTRY_PROLOGUE 0
    cld                             ; DF = 0 (selalu, aman di semua CPU)
    cmp     byte [rel g_smap_enabled], 0
    je      %%kz_entry_no_clac      ; CPU tanpa SMAP → clac akan #UD
    clac                            ; AC = 0 → SMAP aktif selama handler
%%kz_entry_no_clac:
%endmacro

; ============================================================
; KZ_VALIDATE_RSP — validasi RSP hasil handler C sebelum dipakai (ARCH-8)
;
; Dipakai oleh stub yang melakukan context switch lewat return value handler:
;   timer_isr (32), lapic_timer_isr (240), reschedule_isr (253).
;
; Kontrak: RAX = RSP yang dikembalikan handler. RSP saat ini = frame ISR
; yang sedang aktif (valid, karena ISR baru saja berjalan di atasnya).
;
; Kalau RAX tidak wajar (bug scheduler / pointer rusak), memuatnya ke RSP
; membuat kernel crash di tempat yang JAUH dari penyebabnya — biasanya #DF
; atau triple fault, karena stack sudah tidak bisa dipakai melaporkan apa pun.
; Validasi di sini mengubahnya menjadi laporan jelas + fallback ke RSP lama.
;
; Argumen: %1 = nomor vektor (untuk pesan diagnostik).
;
; Clobber: RAX (nilai akhir = RSP yang akan dipakai, selalu valid), RDI, RSI,
;          RDX, RCX, R8-R11 (sesuai ABI SysV — pemanggil sudah selesai dengan
;          nilai-nilai itu karena POPA64 memuat ulang semua GP register).
;
; KENAPA r15 DIPAKAI SEBAGAI SCRATCH: setelah blok ini, POPA64 memuat ULANG
; r15 (dan seluruh GP register lain) dari frame task tujuan. Jadi nilai r15
; register saat ini memang akan dibuang — tidak ada yang membacanya lagi.
; Ini satu-satunya register yang aman dipakai tanpa menyentuh frame.
; ============================================================
extern context_rsp_is_valid
extern context_rsp_fallback

%macro KZ_VALIDATE_RSP 1
    mov     r15, rax                ; simpan kandidat RSP (r15 dibuang oleh POPA64)
    mov     rdi, rax                ; arg1 = bad_rsp
    call    context_rsp_is_valid    ; -> eax = 0/1
    test    eax, eax
    jnz     %%kz_rsp_ok
    ; Tidak valid: minta fallback. RSP saat ini = frame ISR yang aktif, dan
    ; itu pasti valid (ISR baru saja berjalan di atasnya).
    mov     rdi, r15                ; arg1 = bad_rsp
    mov     rsi, rsp                ; arg2 = fallback_rsp
    mov     edx, %1                 ; arg3 = vector
    call    context_rsp_fallback    ; -> rax = RSP yang dipakai
    jmp     %%kz_rsp_done
%%kz_rsp_ok:
    ; PENTING: di jalur ini RAX berisi 0/1 (hasil test), BUKAN RSP. Kandidat
    ; harus dipulihkan dari r15 dulu — tanpa ini `mov rsp, rax` menyetel RSP
    ; ke 1 dan kernel langsung mati.
    mov     rax, r15
%%kz_rsp_done:
    mov     rsp, rax
%endmacro

%macro PUSHA64 0
    push rax        ; pushed first → highest address (RSP+112)
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15        ; pushed last → lowest address (RSP+0)
%endmacro

%macro POPA64 0
    pop r15         ; pop from RSP+0 first
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax         ; pop from RSP+112 last
%endmacro