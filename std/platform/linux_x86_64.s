.text
.globl lunso_sys_read
.type lunso_sys_read, @function
lunso_sys_read:
    xor %eax, %eax
    syscall
    ret
.size lunso_sys_read, .-lunso_sys_read

.globl lunso_sys_write
.type lunso_sys_write, @function
lunso_sys_write:
    mov $1, %eax
    syscall
    ret
.size lunso_sys_write, .-lunso_sys_write

.section .note.GNU-stack,"",@progbits
