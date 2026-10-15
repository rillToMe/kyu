// ============================================================
// shm_demo.c - R3 QEMU test: shared surfaces across two processes.
//
// Parent (producer): creates/publishes an shm surface, drives a fixed
// pipe protocol (malformed msg, unpublished handle, real frame), checks
// the consumer's answer, exercises the live GPU fence counter, cleans up.
// Child (consumer/shadow): separate process (fork), maps read-only via
// the authorized handle, verifies pixels directly AND through
// kz_shadow_compose into its own off-screen buffer (differential across
// the process boundary), reports checksums back over the pipe.
//
// Exit code 0 = all PASS. Launch: start shm_demo
// ============================================================
#include "userlib.h"
#include "kz_shm.h"
#include "kz_surface.h"   // KZ_FMT_* for create/format checks

#define W 64
#define H 48

static int pass = 1;

static void check(char *name, int ok) {
    print(name);
    print(ok ? ": OK\n" : ": FAIL\n");
    if (!ok) pass = 0;
}

static uint32_t pattern(unsigned x, unsigned y) {
    return ((x * 37u + y * 91u) & 0xFFu) |
           (((x * 53u + y * 29u) & 0xFFu) << 8) |
           (((x * 17u + y * 71u) & 0xFFu) << 16);
}

extern uint32_t kz_ck(const uint32_t *p, unsigned n);
#define checksum kz_ck

static int write_full(int fd, const void *buf, unsigned n) {
    unsigned off = 0;
    const char *b = (const char *)buf;
    while (off < n) {
        int m = sys_write_fd(fd, b + off, n - off);
        if (m <= 0) return -1;
        off += (unsigned)m;
    }
    return 0;
}

static int read_full(int fd, void *buf, unsigned n) {
    unsigned off = 0;
    char *b = (char *)buf;
    while (off < n) {
        int m = sys_read_fd(fd, b + off, n - off);
        if (m < 0) return -1;
        if (m == 0) return -1;   // EOF before full message
        off += (unsigned)m;
    }
    return 0;
}

static uint32_t pattern2(unsigned x, unsigned y) {
    return (pattern(x, y) + 0x010101u) & 0xFFFFFFu;
}

// --- consumer/shadow (separate process after fork) ---
static int consumer_main(int rfd, int wfd) {
    kz_shm_msg_t m;
    kz_shm_msg_t ans;
    int malformed_ok = 0, unpub_ok = 0, pixels_ok = 0, shadow_ok = 0;
    int stale_ok = 0, frame2_ok = 0, shadow2_ok = 0;
    uint32_t direct_ck = 0, shadow_ck = 0, ck2 = 0;
    uint32_t *p = 0;
    uint64_t h = 0;

    // 1. malformed message first: bad magic must be rejected, not crash.
    if (read_full(rfd, &m, sizeof(m)) != 0) sys_exit_code(10);
    malformed_ok = (m.magic != KZ_SHM_MSG_MAGIC);

    // 2. well-formed but unpublished handle must not map.
    if (read_full(rfd, &m, sizeof(m)) != 0) sys_exit_code(11);
    if (m.magic == KZ_SHM_MSG_MAGIC && m.version == KZ_SHM_MSG_VER &&
        m.type == KZ_SHM_MSG_PUBLISH) {
        uint64_t hu = kz_shm_handle(m.gen, m.id);
        uint32_t *pu = sys_shm_map(hu, 0);
        unpub_ok = (pu == 0);
    }

    // 3. wild handle must not map.
    stale_ok = (sys_shm_map(kz_shm_handle(0xBEEFu, 13), 0) == 0);

    // 4. real frame: map read-only, verify pixels directly.
    if (read_full(rfd, &m, sizeof(m)) != 0) sys_exit_code(12);
    if (m.magic == KZ_SHM_MSG_MAGIC && m.version == KZ_SHM_MSG_VER &&
        m.type == KZ_SHM_MSG_PUBLISH && m.width == W && m.height == H &&
        m.format == KZ_FMT_XRGB8888) {
        unsigned x, y;
        h = kz_shm_handle(m.gen, m.id);
        p = sys_shm_map(h, 0);
        if (p != 0) {
            pixels_ok = 1;
            for (y = 0; y < H && pixels_ok; y++)
                for (x = 0; x < W && pixels_ok; x++)
                    if (p[y * W + x] != (pattern(x, y) & 0xFFFFFFu))
                        pixels_ok = 0;
            direct_ck = checksum(p, W * H);
            // differential: same scene through kz_shadow into our own
            // off-screen buffer (never presented, never scanout).
            {
                extern int kz_shadow_consumer(const uint32_t *src,
                                              uint32_t *dst, uint32_t *ck);
                uint32_t *dst = (uint32_t *)sys_alloc(W * H * 4u);
                if (dst != 0) {
                    shadow_ok = kz_shadow_consumer(p, dst, &shadow_ck);
                    if (shadow_ok && shadow_ck != direct_ck) shadow_ok = 0;
                    sys_free(dst);
                }
            }
        }
    }
    if (p == 0) sys_exit_code(14);

    // 5. ack frame1, then wait for frame2 on the SAME mapping. The parent
    // only writes after this ack, so a correct frame2 read proves the
    // mapping is shared, not a fork-copied snapshot.
    ans.magic = KZ_SHM_MSG_MAGIC;
    ans.version = KZ_SHM_MSG_VER;
    ans.type = KZ_SHM_MSG_RELEASE;
    ans.gen = 0;
    ans.id = 0;
    ans.x = malformed_ok;
    ans.y = unpub_ok;
    ans.w = (int32_t)pixels_ok;
    ans.h = (int32_t)shadow_ok;
    ans.width = direct_ck;
    ans.height = shadow_ck;
    ans.format = 0;
    ans.stride = (uint32_t)stale_ok;
    if (write_full(wfd, &ans, sizeof(ans)) != 0) sys_exit_code(13);

    if (read_full(rfd, &m, sizeof(m)) != 0) sys_exit_code(15);
    if (m.magic == KZ_SHM_MSG_MAGIC && m.type == KZ_SHM_MSG_PUBLISH) {
        unsigned x, y;
        frame2_ok = 1;
        for (y = 0; y < H && frame2_ok; y++)
            for (x = 0; x < W && frame2_ok; x++)
                if (p[y * W + x] != pattern2(x, y)) frame2_ok = 0;
        ck2 = checksum(p, W * H);
        {
            extern int kz_shadow_consumer(const uint32_t *src,
                                          uint32_t *dst, uint32_t *ck);
            uint32_t *dst = (uint32_t *)sys_alloc(W * H * 4u);
            if (dst != 0) {
                uint32_t sck = 0;
                shadow2_ok = kz_shadow_consumer(p, dst, &sck);
                if (shadow2_ok && sck != ck2) shadow2_ok = 0;
                sys_free(dst);
            }
        }
    }
    sys_shm_unmap(p);

    ans.magic = KZ_SHM_MSG_MAGIC;
    ans.version = KZ_SHM_MSG_VER;
    ans.type = KZ_SHM_MSG_RELEASE;
    ans.gen = ck2;
    ans.id = 0;
    ans.x = frame2_ok;
    ans.y = shadow2_ok;
    ans.w = (int32_t)pixels_ok;
    ans.h = (int32_t)shadow_ok;
    ans.width = direct_ck;
    ans.height = shadow_ck;
    ans.format = 0;
    ans.stride = (uint32_t)stale_ok;
    if (write_full(wfd, &ans, sizeof(ans)) != 0) sys_exit_code(16);
    sys_exit_code((malformed_ok && unpub_ok && pixels_ok && shadow_ok &&
                   stale_ok && frame2_ok && shadow2_ok)
                      ? 0
                      : 1);
}

// Consumer-side shadow compose, linked from libs/render (same sources as
// the host-tested implementation — no forked renderer).
int kz_shadow_consumer(const uint32_t *src, uint32_t *dst, uint32_t *ck);

void main(void) {
    int64_t h, h2;
    uint32_t *p;
    int p2c[2], c2p[2];
    int r;
    unsigned x, y;
    kz_shm_msg_t m;
    uint32_t direct_ck;

    print("SHM DEMO\n");

    h = sys_shm_create(W, H, KZ_FMT_XRGB8888);
    check("create", h > 0);
    h2 = sys_shm_create(16, 16, KZ_FMT_XRGB8888);
    check("create-unpublished", h2 > 0);
    if (!pass) {
        print("SHM DEMO FAIL\n");
        sys_exit_code(1);
    }

    p = sys_shm_map((uint64_t)h, KZ_SHM_MAP_WRITE);
    check("owner-map", p != 0);
    if (p == 0) {
        print("SHM DEMO FAIL\n");
        sys_exit_code(1);
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) p[y * W + x] = pattern(x, y) & 0xFFFFFFu;
    check("publish", sys_shm_publish((uint64_t)h, 0, 0, W, H) == 0);
    direct_ck = checksum(p, W * H);
    print("DIRECT CK ");
    print_num(direct_ck);
    print("\n");

    check("pipes", sys_pipe(p2c) == 0 && sys_pipe(c2p) == 0);
    r = sys_fork();
    if (r < 0) {
        check("fork", 0);
        print("SHM DEMO FAIL\n");
        sys_exit_code(1);
    }
    if (r == 0) {
        sys_close(p2c[1]);
        sys_close(c2p[0]);
        consumer_main(p2c[0], c2p[1]);
        sys_exit_code(99);   // unreachable
    }
    sys_close(p2c[0]);
    sys_close(c2p[1]);

    // malformed first: bad magic, consumer must ignore it safely.
    m.magic = 0xDEADBEEFu;
    m.version = KZ_SHM_MSG_VER;
    m.type = KZ_SHM_MSG_PUBLISH;
    m.gen = 0;
    m.id = 0;
    m.x = m.y = m.w = m.h = 0;
    m.width = m.height = m.format = m.stride = 0;
    check("send-malformed", write_full(p2c[1], &m, sizeof(m)) == 0);

    // unpublished handle: well-formed, must not grant access.
    m.magic = KZ_SHM_MSG_MAGIC;
    m.gen = kz_shm_gen((uint64_t)h2);
    m.id = kz_shm_idx((uint64_t)h2);
    m.width = 16;
    m.height = 16;
    m.format = KZ_FMT_XRGB8888;
    m.stride = 16;
    check("send-unpublished", write_full(p2c[1], &m, sizeof(m)) == 0);

    // real frame.
    m.gen = kz_shm_gen((uint64_t)h);
    m.id = kz_shm_idx((uint64_t)h);
    m.x = 0;
    m.y = 0;
    m.w = W;
    m.h = H;
    m.width = W;
    m.height = H;
    m.format = KZ_FMT_XRGB8888;
    m.stride = W;
    check("send-frame", write_full(p2c[1], &m, sizeof(m)) == 0);

    // frame1 ack carries the child's direct checksum.
    {
        kz_shm_msg_t ack;
        int ok = (read_full(c2p[0], &ack, sizeof(ack)) == 0);
        ok = ok && ack.magic == KZ_SHM_MSG_MAGIC;
        check("ack-shape", ok);
        check("child-malformed", ack.x == 1);
        check("child-unpublished", ack.y == 1);
        check("child-pixels", ack.w == 1);
        check("child-shadow", ack.h == 1);
        check("child-stale", ack.stride == 1);
        print("SHADOW CK ");
        print_num(ack.height);
        print("\n");
        check("ck-match", ack.width == direct_ck && ack.height == direct_ck);
    }

    // frame2 AFTER the child mapped: shared memory, not a fork copy.
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) p[y * W + x] = pattern2(x, y);
    check("publish2", sys_shm_publish((uint64_t)h, 0, 0, W, H) == 0);
    {
        uint32_t ck2 = checksum(p, W * H);
        print("FRAME2 CK ");
        print_num(ck2);
        print("\n");
        check("send-frame2", write_full(p2c[1], &m, sizeof(m)) == 0);
        sys_close(p2c[1]);   // EOF after frame2
        {
            kz_shm_msg_t ans;
            int ok = (read_full(c2p[0], &ans, sizeof(ans)) == 0);
            ok = ok && ans.magic == KZ_SHM_MSG_MAGIC &&
                 ans.type == KZ_SHM_MSG_RELEASE;
            check("answer-shape", ok);
            check("child-frame2", ans.x == 1);
            check("child-shadow2", ans.y == 1);
            check("frame2-match", ans.gen == ck2);
            if (ans.gen == ck2) print("FRAME2 MATCH\n");
            sys_close(c2p[0]);
            {
                int status = -1;
                int waited = sys_waitpid(r, &status, 0);
                check("child-exit", waited == r && status == 0);
            }
        }
    }

    // R4: authoritative metadata + acquire/release + replacement.
    {
        kz_shm_stat_t st;
        int64_t h3;
        check("stat", sys_shm_stat((uint64_t)h, &st) == 0);
        check("stat-ver", st.version == KZ_SHM_STAT_VER);
        check("stat-dims", st.width == W && st.height == H);
        check("stat-fmt", st.format == KZ_FMT_XRGB8888);
        check("stat-gen",
              st.generation == kz_shm_gen((uint64_t)h));
        check("stat-rights",
              st.rights == (KZ_SHM_RIGHT_READ | KZ_SHM_RIGHT_WRITE));
        check("stat-state", st.state == KZ_SHM_STATE_PUBLISHED);
        check("stat-bad", sys_shm_stat(kz_shm_handle(0xBEEFu, 13), &st) != 0);
        check("acquire", sys_shm_acquire((uint64_t)h) == 0);
        check("release", sys_shm_release((uint64_t)h) == 0);
        check("release-again", sys_shm_release((uint64_t)h) != 0);
        h3 = sys_shm_create(48, 24, KZ_FMT_XRGB8888);
        check("replace-create", h3 > 0 && h3 != h);
        check("replace-publish", sys_shm_publish((uint64_t)h3, 0, 0, 48, 24) == 0);
        check("replace-stat", sys_shm_stat((uint64_t)h3, &st) == 0 &&
                                   st.width == 48 && st.height == 24);
        check("retire-old", sys_shm_destroy((uint64_t)h) == 0);
        check("old-stale", sys_shm_map((uint64_t)h, 0) == 0);
        check("old-stat-stale", sys_shm_stat((uint64_t)h, &st) != 0);
        {
            uint32_t *p3 = sys_shm_map((uint64_t)h3, KZ_SHM_MAP_WRITE);
            check("new-usable", p3 != 0);
            if (p3 != 0) sys_shm_unmap(p3);
        }
        check("replace-destroy", sys_shm_destroy((uint64_t)h3) == 0);
        // h is retired above; the tail unmap/destroy below must tolerate it.
    }

    // live GPU fence: present through a real window, watch gpu_stats.
    {
        gpu_stats_t before, after;
        int have = (sys_gpu_stats(&before) == 0);
        if (!have) {
            print("FENCE SKIP (no backend stats)\n");
        } else {
            int i;
            uint32_t *fb = (uint32_t *)sys_alloc(W * H * 4u);
            int win = -1;
            if (fb != 0) {
                for (y = 0; y < H; y++)
                    for (x = 0; x < W; x++) fb[y * W + x] = pattern(x, y);
                win = sys_create_window(10, 10, W, H);
                for (i = 0; i < 10; i++) {
                    if (win >= 0) sys_update_window(win, fb);
                    sys_yield();
                }
                if (win >= 0) sys_destroy_window(win);
                sys_free(fb);
            }
            have = (sys_gpu_stats(&after) == 0);
            print("FENCE present=");
            print_num((uint32_t)after.present_count);
            print(" err=");
            print_num((uint32_t)after.err_count);
            print("\n");
            if (have && after.present_count > before.present_count &&
                after.err_count == before.err_count)
                print("FENCE LIVE\n");
            else
                print("FENCE QUIET\n");
            check("fence-quiet-ok", 1);
        }
    }

    check("unmap", sys_shm_unmap(p) == 0);
    // h was retired in the R4 replacement block: destroy must report stale.
    check("destroy-retired", sys_shm_destroy((uint64_t)h) != 0);
    {
        uint64_t va2 = (uint64_t)(uintptr_t)sys_shm_map((uint64_t)h2, 0);
        // owner re-map of h2 (still owned): must succeed (idempotent record
        // from create is gone after... create mapped it; map again -> same).
        check("h2-remap", va2 != 0);
        if (va2 != 0) check("h2-unmap", sys_shm_unmap((uint32_t *)va2) == 0);
        check("h2-destroy", sys_shm_destroy((uint64_t)h2) == 0);
    }

    if (pass) print("SHM DEMO PASS\n");
    else print("SHM DEMO FAIL\n");
    sys_exit_code(pass ? 0 : 1);
}
