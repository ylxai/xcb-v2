/*
 * payload.c — payload posisi-independen (shellcode) untuk lab polymorphic.
 *
 * Tidak pakai libc sama sekali: langsung syscall write(1, msg, len) lalu
 * exit(0). Output deterministik "POLYMORPHIC-DEMO-OK\n" — ini yang dipakai
 * verifikasi: SEMUA varian polymorphic harus mencetak string yang sama.
 *
 * Build: cc -nostdlib -fPIC -c payload.c && objcopy -O binary -j .text
 */
__attribute__((naked, used, section(".text")))
void _start(void) {
    asm volatile(
        "mov $1, %rax\n"        /* syscall write */
        "mov $1, %rdi\n"        /* fd = stdout */
        "lea msg(%rip), %rsi\n" /* buffer (RIP-relative, PIC) */
        "mov $len, %rdx\n"      /* panjang */
        "syscall\n"
        "mov $60, %rax\n"       /* syscall exit */
        "xor %rdi, %rdi\n"
        "syscall\n"
        "msg: .ascii \"POLYMORPHIC-DEMO-OK\\n\"\n"
        "len = . - msg\n");
}
