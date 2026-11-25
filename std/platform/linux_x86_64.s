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

.globl lunso_sys_file_open
.type lunso_sys_file_open, @function
lunso_sys_file_open:
    mov $02000000, %edx     # O_CLOEXEC | O_RDONLY
    test %sil, %sil
    jz .Lfile_open
    or $01101, %edx         # O_WRONLY | O_CREAT | O_TRUNC
.Lfile_open:
    mov %rdi, %rsi          # path
    mov $-100, %edi         # AT_FDCWD
    mov $0666, %r10d        # creation mode, filtered by umask
    mov $257, %eax          # openat
    syscall
    ret
.size lunso_sys_file_open, .-lunso_sys_file_open

.globl lunso_sys_file_close
.type lunso_sys_file_close, @function
lunso_sys_file_close:
    mov $3, %eax            # close
    syscall
    ret
.size lunso_sys_file_close, .-lunso_sys_file_close

.globl lunso_mem_map
.type lunso_mem_map, @function
lunso_mem_map:
    push %rsi
    mov %rdi, %rsi
    xor %edi, %edi
    mov $3, %edx           # PROT_READ | PROT_WRITE
    mov $0x22, %r10d       # MAP_PRIVATE | MAP_ANONYMOUS
    mov $-1, %r8
    xor %r9d, %r9d
    mov $9, %eax           # mmap
    syscall
    pop %rdx
    cmp $-4095, %rax
    jae .Lmap_done
    mov %rax, (%rdx)
    xor %eax, %eax
.Lmap_done:
    ret
.size lunso_mem_map, .-lunso_mem_map

.globl lunso_mem_unmap
.type lunso_mem_unmap, @function
lunso_mem_unmap:
    mov $11, %eax          # munmap
    syscall
    ret
.size lunso_mem_unmap, .-lunso_mem_unmap

.section .note.GNU-stack,"",@progbits
