#include "syscall.h"
#include <stdint.h>
#include <stddef.h>   // offsetof — Phase 24 freeze asserts
#include "task.h"
#include "usercopy.h"
#include "userlib.h"  // gpu_stats_t (ABI user-space syscall 65)
#include "ghal.h"     // Phase 2C §9.6: sys_gpu_stats — lewat kontrak HAL, bukan driver core (rule §5.5)

// Phase 24 API freeze: mirror ghal_gpu_stats_t <-> gpu_stats_t (syscall 65
// copy_to_user sizeof(st)). Drift apa pun = ABI rusak. Kompiler yang menolak.
_Static_assert(sizeof(ghal_gpu_stats_t) == sizeof(gpu_stats_t),
               "ghal/userlib gpu_stats size drift");
_Static_assert(offsetof(ghal_gpu_stats_t, present_count) == offsetof(gpu_stats_t, present_count), "stats off 0");
_Static_assert(offsetof(ghal_gpu_stats_t, cmd_count) == offsetof(gpu_stats_t, cmd_count), "stats off 1");
_Static_assert(offsetof(ghal_gpu_stats_t, cmd_bytes) == offsetof(gpu_stats_t, cmd_bytes), "stats off 2");
_Static_assert(offsetof(ghal_gpu_stats_t, notify_count) == offsetof(gpu_stats_t, notify_count), "stats off 3");
_Static_assert(offsetof(ghal_gpu_stats_t, wait_calls) == offsetof(gpu_stats_t, wait_calls), "stats off 4");
_Static_assert(offsetof(ghal_gpu_stats_t, wait_ticks) == offsetof(gpu_stats_t, wait_ticks), "stats off 5");
_Static_assert(offsetof(ghal_gpu_stats_t, err_count) == offsetof(gpu_stats_t, err_count), "stats off 6");

int sys_gpu_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

    if (syscall_num == 65) { // sys_gpu_stats(buf) — Phase 2C §9.6
        // buf = ghal_gpu_stats_t (7 x uint64), mirror gpu_stats_t di userlib.h.
        // Layout dikunci Phase 24 (assert di atas, file-scope).
        ghal_gpu_stats_t st_gpu;
        if (ghal_gpu_stats(&st_gpu) != 0) *ret = (uint64_t)-1;
        else *ret = (copy_to_user(uc, r->rbx, &st_gpu, sizeof(st_gpu)) == 0) ? 0 : (uint64_t)-1;
    }

    return 0;
}
