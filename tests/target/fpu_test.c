// ============================================================
// fpu_test.c - verifikasi state FPU/SSE tidak bocor antar-task.
//
// Launch: start fpu_test
//
// KENAPA TEST INI ADA
//
//   fpu_switch() (kernel/sched/fpu.c) menyimpan x87 + XMM tiap context switch.
//   Tanpa itu, XMM adalah state GLOBAL per-CPU: task berikutnya melihat sisa
//   nilai pendahulunya. Itu kebocoran data antar-proses sekaligus sumber hasil
//   komputasi yang salah secara acak - dan tidak terdeteksi page table.
//
//   Test ini membuktikannya secara langsung: ia menulis pola float yang
//   bergantung pada PID-nya sendiri, lalu MEMERIKSA pola itu masih utuh
//   setelah melakukan kerja yang memaksa banyak context switch (yield +
//   syscall). Kalau save/restore bocor, nilainya berubah.
//
// CARA KERJA
//
//   Untuk mendeteksi kebocoran, sebuah task harus (a) menulis nilai yang
//   berbeda dari task lain, lalu (b) membiarkan CPU dipakai task lain, lalu
//   (c) membaca kembali. Pola diturunkan dari PID supaya dua task tidak
//   pernah menulis nilai yang sama:
//
//       a = pid * 1000 + 1
//       b = pid * 1000 + 2
//
//   Setelah itu test menghitung `a * 1.5 + b` berkali-kali, dengan `sys_yield()`
//   di antaranya, dan memeriksa hasilnya tetap sama setiap kali. `sys_yield`
//   adalah titik context switch yang sesungguhnya - kalau XMM tidak disimpan,
//   task lain (desktop, shell, clock) akan menimpanya.
//
// CATATAN: test ini berjalan di userspace dan karena itu menguji ABI, bukan
//   kernel secara internal. Itu memang yang penting: yang dijanjikan ke
//   aplikasi adalah "float kamu tidak berubah setelah yield".
// ============================================================
#include "userlib.h"

// Nilai pembanding dihitung ulang dari sumber yang sama, jadi tidak ada
// konstanta ajaib yang bisa salah.
static double expected_value(int pid) {
    double a = (double)pid * 1000.0 + 1.0;
    double b = (double)pid * 1000.0 + 2.0;
    return a * 1.5 + b;
}

static int double_equal(double x, double y) {
    // Perbandingan eksak: operasinya identik, jadi hasilnya harus bit-identik.
    // Toleransi justru akan menyembunyikan bug yang test ini cari.
    return x == y;
}

void main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    int pid = (int)sys_getpid();
    print("fpu_test: pid=");
    print_num((uint32_t)pid);
    print("\n");

    // Nilai yang hanya milik task ini.
    double a = (double)pid * 1000.0 + 1.0;
    double b = (double)pid * 1000.0 + 2.0;
    double want = expected_value(pid);

    int fails = 0;

    // --- 1. FP dasar: hasil aritmetika harus benar ---
    double got = a * 1.5 + b;
    if (!double_equal(got, want)) {
        print("FAIL: aritmetika dasar salah\n");
        fails++;
    }

    // --- 2. Pola bertahan melewati banyak context switch ---
    //
    // Setiap iterasi menulis ulang register XMM lewat `a * 1.5 + b`, lalu
    // menyerahkan CPU. Kalau kernel tidak menyimpan XMM, task lain yang
    // memakai float (clock, desktop) akan meninggalkan nilainya di register
    // dan iterasi berikutnya membaca angka yang salah.
    const int ROUNDS = 200;
    for (int i = 0; i < ROUNDS; i++) {
        double v = a * 1.5 + b;
        if (!double_equal(v, want)) {
            print("FAIL: nilai berubah setelah context switch, iterasi ");
            print_num((uint32_t)i);
            print("\n");
            fails++;
            break;
        }
        sys_yield();
    }

    // --- 3. XMM selamat melewati syscall ---
    //
    // Syscall BUKAN context switch, tapi ia masuk kernel - dan kernel dibangun
    // -mno-sse (KYUZEN_KERNEL_FP_BAN), jadi ia tidak boleh menyentuh XMM sama
    // sekali. Ini memverifikasi janji itu.
    for (int i = 0; i < 50; i++) {
        double v = a * 1.5 + b;
        (void)sys_uptime();          // syscall yang selalu ada
        if (!double_equal(v, want)) {
            print("FAIL: XMM rusak setelah syscall\n");
            fails++;
            break;
        }
    }

    // --- 4. x87 juga tersimpan (FXSAVE mencakup keduanya) ---
    //
    // Operasi ini memaksa nilai melewati stack x87. Kalau hanya XMM yang
    // disimpan, ini akan gagal pada task yang memakai x87.
    volatile double s1 = a;
    volatile double s2 = b;
    double sum = 0.0;
    for (int i = 0; i < 20; i++) {
        sum += s1;
        sum -= s1;
        sys_yield();
    }
    if (!double_equal(sum, 0.0)) {
        print("FAIL: akumulasi x87 tidak kembali ke nol\n");
        fails++;
    }

    if (fails == 0) {
        print("fpu_test: PASS (");
        print_num((uint32_t)ROUNDS);
        print(" ronde yield, nilai FP stabil)\n");
        sys_exit_code(0);
    } else {
        print("fpu_test: FAIL\n");
        sys_exit_code(1);
    }
}
