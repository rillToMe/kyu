#include "userlib.h"
#include "timer.h"   // timer_sleep_ms() — sleep berbasis ms, hardware-agnostic


extern void user_shell();

// Fungsi bantuan manipulasi string
int str_match(const char* s1, const char* s2) {
    while (*s1 != '\0' && *s1 == *s2) { s1++; s2++; }
    return (*s1 == *s2);
}

void str_concat(char* dest, const char* src) {
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
    timer_sleep_ms(2000); // Jeda 2 detik


}

void user_login() {
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
            sys_set_uid(active_uid);
            timer_sleep_ms(1000); // Jeda 1 detik sebelum masuk shell

            // Phase 10: spawn desktop shell (task sendiri, jalan konkuren).
            // Gagal (file tak ada) → fallback natural ke shell CLI.
            char dpath[32];
            build_app_path(dpath, sizeof(dpath), "desktop.elf");
            sys_spawn(dpath);

            clear_screen();
            user_shell();
        } else {
            print("[DENIED] Akses Ditolak: Username atau Password salah!\n");
            timer_sleep_ms(2000); // Jeda 2 detik, tampilkan pesan error


        }
    }
}