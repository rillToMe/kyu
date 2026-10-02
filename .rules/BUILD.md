# KyuzenOS Build & Testing Rules

Part of the KyuzenOS Development Rules. See `RULES.md` for general principles.

---

# 1. Build Verification

Before considering any task complete:

1. Build the project.

   ```sh
   ./build.sh
   ```

2. Fix every compilation error.

3. Fix every warning whenever possible.

Never consider work finished without a successful build.

`./build.sh` locates MSYS2 and sets the toolchain PATH correctly no matter which
shell you started in — on MSYS2 that matters, because the project needs tools
from two different clang installations. See
[Building](../docs/development/building.md#why-the-script-exists).

Useful variants:

```sh
./build.sh iso          # + Rust apps and the bootable ISO
./build.sh run          # boot in QEMU
./build.sh test         # host-side unit tests
./build.sh clean        # remove build/ entirely
./build.sh <target>     # any Ninja target
```

A build that "succeeds" with the wrong clang (LLVM 22 from `clang64/bin`
instead of LLVM 21 from `usr/bin`) produces a **byte-different kernel** and
breaks parity with the verified baseline. Configure prints a warning when this
happens — do not silence it by setting `KYUZEN_EXPECTED_CLANG_MAJOR`; fix PATH.

---

# 2. Testing

New features should be tested.

Bug fixes should include regression verification.

Never assume code works.

Verify it.
