.section .text
.align 2
.global benchmark_marker
.type benchmark_marker, @function
benchmark_marker:
    li t0, 0x10010000
    sw a0, 0x28(t0)
    sw zero, 0x2c(t0)
    sw zero, 0x30(t0)
    fence iorw, iorw
    li t1, 2
    sub t1, t1, a1
    sw t1, 0x34(t0)
    fence iorw, iorw
    ret
.size benchmark_marker, .-benchmark_marker
