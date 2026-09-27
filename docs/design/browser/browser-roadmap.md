# Browser roadmap (ditunda eksplisit — bukan janji)

1. **TLS/HTTPS** — DONE 2026-09-26 (BearSSL 0.6 subset: TLS1.2,
   ECDHE+AES-128-GCM, X.509 penuh; syscall entropi 87; trust prebuilt).
   Sisa: root tambahan bila situs baru butuh (tambah PEM -> `brssl ta`);
   AAA Certificate Services kedaluwarsa Des 2028 (bukan jangkar aktif,
   hanya cross-sign — tak menghalangi).
2. **Async networking** — butuh primitif konkurensi teraudit (worker +
   ownership-transfer); baru lalu: Loading non-blokir, cancel races penuh.
3. **Keyboard scroll sadar-fokus** — butuh focus-getter ABI; baru lalu:
   Space/PgUp/PgDn/Home/End + shortcut navigasi (Ctrl+B/F/R).
4. **Viewport resize ABI** — ganti h fix 8000 (scrollbar selalu penuh).
5. **Form submit** — butuh model POST + kontrol input terikat DOM.
6. **Cookies** — tolak bicara jujur sampai ada desain store.
7. **Tabs** — butuh model Page per-tab + chrome tab (di luar MVP per §46).
8. **JPEG** — butuh IDCT tanpa float (atau libm soft-float yang diaudit);
   sampai itu placeholder.
9. **Cache Kondisional HTTP** (ETag/Last-Modified) — setelah stabil.
10. **JS** — eksplisit di luar scope sampai fase mesin-JS diaudit (§51).

Non-tujuan tetap (§73): multi-proses, sandbox proses, GPU compositing,
CSS penuh, HTML5 penuh, WebAssembly, workers, WebRTC, IndexedDB, ekstensi,
devtools.
