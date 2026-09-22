#include "zen.h"
#include <stdint.h>
#include "userlib.h"
#include "timer.h"  

extern void* sys_alloc(uint32_t size);
extern void sys_free(void* ptr);
extern void* sys_realloc(void* ptr, uint32_t old_size, uint32_t new_size);
extern int sys_file_exists(char* filename);
extern uint32_t sys_file_size(char* filename);
extern int sys_read_file_to_buffer(char* filename, char* buffer, uint32_t buffer_capacity);
extern int sys_create_file(char* filename, char* data, uint32_t size);

static uint32_t zen_strlen(const char* str) {
    uint32_t len = 0;
    while (str[len]) len++;
    return len;
}

static void zen_memset(void* s, int c, uint32_t n) {
    uint8_t* p = (uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)c;
}

void zen_main(char* filename) {
    clear_screen();
    print("--- ZEN EDITOR (Tekan ESC untuk Simpan & Keluar) ---\n");

    uint32_t current_capacity = 32; 
    
    if (sys_file_exists(filename)) {
        uint32_t existing_size = sys_file_size(filename);
        if (existing_size >= current_capacity) {
            current_capacity = existing_size + 32; 
        }
    }

    char* text_buffer = (char*)sys_alloc(current_capacity);
    if (text_buffer == 0) return; 
    zen_memset(text_buffer, 0, current_capacity);
    
    uint32_t cursor = 0;

    if (sys_file_exists(filename)) {
        sys_read_file_to_buffer(filename, text_buffer, current_capacity);
        cursor = zen_strlen(text_buffer);
        print(text_buffer);
    }

    char key[1];
    while(1) {
        if (read_keyboard(key, 1) > 0) {
            char c = key[0];

            if (c == 27) break; 

            if (c == '\b') {
                if (cursor > 0) {
                    cursor--;
                    text_buffer[cursor] = '\0';
                    print("\b"); 
                }
                continue; 
            }

            if (cursor >= current_capacity - 1) { 
                uint32_t new_capacity = current_capacity * 2; 
                char* new_buffer = (char*)sys_realloc(text_buffer, current_capacity, new_capacity);
                
                if (new_buffer == 0) {
                    print("\n[FATAL] RAM Habis, auto-expand gagal!\n");
                    timer_sleep_ms(3000);


                    break; 
                }
                
                text_buffer = new_buffer;       
                current_capacity = new_capacity; 
            }
            
            if (c == '\n') {
                text_buffer[cursor] = '\n';
                cursor++;
                print("\n");
            } else {
                text_buffer[cursor] = c;
                cursor++;
                
                char str_char[2] = {c, '\0'};
                print(str_char); 
            }
        }
        sys_yield(); 
    }

    clear_screen();

    if (sys_file_exists(filename)) { fs_delete(filename); }
    sys_create_file(filename, text_buffer, zen_strlen(text_buffer));

    sys_free(text_buffer); 
    clear_screen();
}