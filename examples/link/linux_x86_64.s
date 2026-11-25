.text
.globl lunso_write
.type lunso_write, @function
lunso_write:
    mov $1, %eax
    syscall
    ret
.size lunso_write, .-lunso_write

.section .note.GNU-stack,"",@progbits
