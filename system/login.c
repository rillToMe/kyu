// system/login.c — login CLI + peluncur shell.
//
// Dibangun sebagai ELF user-space (Ring 3), sumber di system/ karena historis
// dulu bagian kernel image. Sama seperti system/cat.c, echo.c, zen.c, init.c:
// seluruh isinya lewat API userlib (include/userlib.h), jadi tidak butuh
// simbol kernel sama sekali.
//
// Perubahan dari versi Ring 0:
//   - user_login() → main() (ENTRY main di apps/app.ld)
//   - timer_sleep_ms(ms) → sys_sleep(ms) (syscall 46)
//   - user_shell() (panggilan fungsi langsung) → sys_spawn("/apps/shell.elf"),
//     lalu sys_waitpid supaya login menunggu shell selesai (logout) dan
//     menampilkan layar login lagi. Dulu user_shell() adalah loop di kernel
//     yang tidak pernah kembali.
#include "userlib.h"

// Fungsi bantuan manipulasi string
static int str_match(const char* s1, const char* s2) {
    while (*s1 != '\0' && *s1 == *s2) { s1++; s2++; }
    return (*s1 == *s2);
}

static void str_concat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

// Fungsi Parser untuk membaca /etc/shadow versi Kyuzen
int parse_auth(char* input_user, char* input_pass, uint32_t* out_uid) {
    if (!sys_file_exists("users.sys")) return 0;

    uint32_t fsize = sys_file_size("users.sys");
    char buffer[1024];
    // UP-2: fsize berasal dari file dan bisa >= sizeof(buffer). Tanpa clamp,
    // buffer[fsize] menulis lewat stack frame (users.sys >=1024 byte). Batasi
    // ke kapasitas buffer dan selalu sisakan satu byte untuk terminator.
    if (fsize > sizeof(buffer) - 1) fsize = sizeof(buffer) - 1;
    sys_read_file_to_buffer("users.sys", buffer, sizeof(buffer));
    buffer[fsize] = '\0'; // Kunci string agar tidak ada memori sampah

    int i = 0;
    while (buffer[i] != '\0') {
        // UP-3: f_user/f_pass/f_uid diisi dari file tanpa batas. Field yang
        // panjang (file rusak/dibuat host) menulis lewat buffer stack. Loop di
        // bawah sekarang berhenti di kapasitas-1 dan selalu NUL-terminate;
        // sisa field dibuang agar parsing tetap sinkron dengan ':' / '\n'.
        char f_user[32], f_pass[32], f_uid[16];
        int j = 0;

        // Ambil Username
        while(buffer[i] != ':' && buffer[i] != '\0') {
            if (j < (int)sizeof(f_user) - 1) f_user[j++] = buffer[i];
            i++;
        }
        f_user[j] = '\0';
        if(buffer[i] == ':') i++;

        // Ambil Password
        j = 0;
        while(buffer[i] != ':' && buffer[i] != '\0') {
            if (j < (int)sizeof(f_pass) - 1) f_pass[j++] = buffer[i];
            i++;
        }
        f_pass[j] = '\0';
        if(buffer[i] == ':') i++;

        // Ambil UID
        j = 0;
        while(buffer[i] != '\n' && buffer[i] != '\0') {
            if (j < (int)sizeof(f_uid) - 1) f_uid[j++] = buffer[i];
            i++;
        }
        f_uid[j] = '\0';
        if(buffer[i] == '\n') i++;

        // Cek Kecocokan
        if (str_match(input_user, f_user) && str_match(input_pass, f_pass)) {
            uint32_t parsed_uid = 0;
            for(int k=0; f_uid[k]!='\0'; k++) parsed_uid = parsed_uid * 10 + (f_uid[k] - '0');
            *out_uid = parsed_uid;
            return 1;
        }
    }
    return 0; // Gagal login
}

// Layar Setup Instalasi Baru
void first_time_setup() {
    char password[32];
    char c;
    int p_idx = 0;

    clear_screen();
    print("INSTALASI KYUZEN OS (FIRST BOOT)\n");
    print("Sistem mendeteksi instalasi Hard Disk baru.\n");
    print("Silakan buat password untuk akun 'root'.\n\n");
    print("Password Root Baru : ");

    while (1) {
        if (read_keyboard(&c, 1) > 0) {
            if (c == '\n') {
                password[p_idx] = '\0';
                break;
            } else if (c == '\b') {
                if (p_idx > 0) { print("\b"); p_idx--; }
            } else if (p_idx < 31) {
                password[p_idx++] = c;
                print("*");
            }
        }
        sys_yield();
    }

    // Bangun struktur format "root:password_baru:0\n"
    char data[128];
    data[0] = '\0';
    str_concat(data, "root:");
    str_concat(data, password);
    str_concat(data, ":0\n");

    int len = 0;
    while(data[len]) len++;

    sys_create_file("users.sys", data, len);
    
    print("\n\n[OK] File users.sys dibuat! Akun root dikonfigurasi.\n");
    print("  Sistem siap digunakan. Memuat halaman login...\n");
    sys_sleep(2000); // Jeda 2 detik


}

void main(int argc, char** argv) {
    (void)argc; (void)argv;
    // 1. Cek apakah ini instalasi baru?
    if (!sys_file_exists("users.sys")) {
        first_time_setup();
    }

    // 2. Loop Layar Login Utama
    char username[32];
    char password[32];
    char c;
    int u_idx, p_idx;

    while (1) {
        clear_screen();
        print("SISTEM KEAMANAN KYUZEN OS\n");

        print("Username : ");
        u_idx = 0;
        while (1) {
            if (read_keyboard(&c, 1) > 0) {
                if (c == '\n') { username[u_idx] = '\0'; break; }
                else if (c == '\b') { if (u_idx > 0) { print("\b"); u_idx--; } } 
                else if (u_idx < 31) { username[u_idx++] = c; char s[2] = {c, '\0'}; print(s); }
            }
            sys_yield();
        }

        print("\nPassword : ");
        p_idx = 0;
        while (1) {
            if (read_keyboard(&c, 1) > 0) {
                if (c == '\n') { password[p_idx] = '\0'; break; } 
                else if (c == '\b') { if (p_idx > 0) { print("\b"); p_idx--; } } 
                else if (p_idx < 31) { password[p_idx++] = c; print("*"); }
            }
            sys_yield();
        }

        print("\n\nMencocokkan data...\n");

        uint32_t active_uid = 0;
        if (parse_auth(username, password, &active_uid)) {
            sys_sleep(1000); // Jeda 1 detik sebelum masuk shell

            // Desktop shell (GUI) di-spawn DI SINI — setelah autentikasi sukses.
            // Dulu init.elf yang men-spawn-nya bersamaan dengan login, dan
            // hasilnya race layar: prompt login TTY ditimpa GUI sebelum user
            // sempat mengetik (sistem tampak freeze). Sekarang desktop hanya
            // jalan untuk sesi terautentikasi, dan tidak ada dua penulis layar
            // yang berebut di saat yang sama.
            {
                char dpath[32];
                build_app_path(dpath, sizeof(dpath), "desktop.elf");
                if (sys_file_exists(dpath)) {
                    if (sys_spawn(dpath) < 0)
                        print("[login] peringatan: desktop.elf gagal spawn\n");
                }
            }

            // Shell CLI sebagai proses terpisah (ELF ring-3). Dulu user_shell()
            // adalah loop di dalam kernel yang tidak pernah kembali; sekarang
            // login menunggu shell selesai (logout) lalu menampilkan layar
            // login lagi.
            //
            // PENTING — urutan uid: shell di-spawn SEBELUM sys_set_uid(), lalu
            // uid target diteruskan sebagai argv[1] dan shell sendiri yang
            // menurunkannya. Kalau login menurunkan uid lebih dulu, shell
            // mewarisi uid non-root lewat cred_inherit() dan perintah root-only
            // (format/shutdown/reboot, sys_fs.c & sys_system.c) akan ditolak
            // selamanya — `sudo` hanya flag UX di shell, batas aslinya di kernel.
            char spath[32];
            build_app_path(spath, sizeof(spath), "shell.elf");
            char uidarg[16];
            {   // uint32 -> desimal tanpa stdio (freestanding)
                uint32_t v = active_uid; int n = 0;
                char tmp[12];
                if (v == 0) tmp[n++] = '0';
                else { while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } }
                int k = 0; while (n) uidarg[k++] = tmp[--n];
                uidarg[k] = '\0';
            }

            clear_screen();
            char* cargv[1] = { uidarg };
            int sh_pid = sys_spawn_argv(spath, 1, cargv);
            if (sh_pid < 0) {
                print("Gagal menjalankan shell.elf (jalankan `make apps`)\n");
                sys_sleep(3000);
            } else {
                int status = 0;
                sys_waitpid(sh_pid, &status, 0);   // tunggu sampai `logout`
            }
        } else {
            print("[DENIED] Akses Ditolak: Username atau Password salah!\n");
            sys_sleep(2000); // Jeda 2 detik, tampilkan pesan error


        }
    }
}