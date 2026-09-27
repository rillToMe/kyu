#ifndef ENTROPY_H
#define ENTROPY_H

#include <stdint.h>

// Syscall 87: sys_entropy(out*, len) -> byte terisi (>=0) / negatif.
// RDRAND-backed. SENGAJA tanpa fallback: randomness tertebak (LCG/PIT)
// tidak boleh menjadi seed kunci TLS — gagal jujur lebih baik.
//   ENTROPY_ERR   (-1): argumen buruk / RDRAND gagal total (retry habis)
//   ENTROPY_ENOHW (-2): CPU tidak punya RDRAND (cek CPUID leaf 1 ECX[30])
#define SYS_ENTROPY 87

#define ENTROPY_ERR   (-1)
#define ENTROPY_ENOHW (-2)
#define ENTROPY_MAX   256u   // cap per panggilan (kebutuhan TLS: 16-48B)

// 1 = RDRAND tersedia (hasil CPUID di-cache, idempoten, tanpa lock).
int entropy_has_rdrand(void);
// out/len = pointer KERNEL. Isi len byte acak. Return len / negatif.
int entropy_fill(uint8_t *out, uint32_t len);

#endif // ENTROPY_H
