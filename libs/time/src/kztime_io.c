// libs/time/src/kztime_io.c — IO "/time.ui" (butuh syscall userlib).
//
// DIPISAH DARI kztime.c DENGAN SENGAJA:
// kztime.c adalah logika murni tanpa syscall, sehingga bisa di-link ke host
// test (tests/host/unit/time_test.c) dan diuji tanpa boot. Kalau IO ditaruh di
// sana, test host akan gagal link karena sys_open/sys_read_fd tidak ada.
//
// Kernel juga tidak boleh memakai file ini (tidak ada filesystem di jalur itu),
// jadi pemisahan ini sekaligus menjaga batas kernel/user.
#include "kztime.h"
#include "userlib.h"

int kz_time_cfg_load(kz_time_cfg_t* out) {
    kz_time_cfg_default(out);
    char path[] = KZ_TIME_CFG_PATH;
    // Ukuran harus persis: file dengan panjang lain bukan config kita (dan
    // lebih aman ditolak daripada ditafsirkan).
    if ((int)sys_file_size(path) != KZ_TIME_CFG_LEN) return 0;
    char buf[KZ_TIME_CFG_LEN];
    if (sys_read_file_to_buffer(path, buf, KZ_TIME_CFG_LEN) != 1) return 0;
    kz_time_cfg_decode(buf, KZ_TIME_CFG_LEN, out);
    return 1;
}

int kz_time_cfg_save(const kz_time_cfg_t* cfg) {
    char path[] = KZ_TIME_CFG_PATH;
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    char buf[KZ_TIME_CFG_LEN];
    int n = kz_time_cfg_encode(cfg, buf);
    int w = sys_write_fd(fd, buf, (uint32_t)n);
    sys_close(fd);
    return w == n;
}
