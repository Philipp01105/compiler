    .section .rdata,"dr"
.LC0:
    .string "=== NEUE SYNTAX TEST ==="
.LC1:
    .string "x = "
.LC2:
    .string "%d"
.LC3:
    .string "y = "
.LC4:
    .string "%d"
.LC5:
    .string "sum = "
.LC6:
    .string "%d"
.LC7:
    .string "\n=== FOR-SCHLEIFE ==="
    .text
    .globl main
main:
    pushq %rbp
    movq %rsp, %rbp
    subq $192, %rsp

    movl $0, -4(%rbp)
    movl $0, -8(%rbp)
    movl $0, -12(%rbp)
    movl $0, -16(%rbp)
    movl $0, -20(%rbp)

    leaq .LC0(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl $10, %ecx
    subq $40, %rsp
    call putchar
    addq $40, %rsp
    pushq $10
    popq %rax
    movl %eax, -4(%rbp)
    pushq $20
    popq %rax
    movl %eax, -8(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -12(%rbp)
    leaq .LC1(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl -4(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC2(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl $10, %ecx
    subq $40, %rsp
    call putchar
    addq $40, %rsp
    leaq .LC3(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl -8(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC4(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl $10, %ecx
    subq $40, %rsp
    call putchar
    addq $40, %rsp
    leaq .LC5(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl -12(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC6(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl $10, %ecx
    subq $40, %rsp
    call putchar
    addq $40, %rsp
    leaq .LC7(%rip), %rcx
    subq $40, %rsp
    call printf
    addq $40, %rsp
    movl $10, %ecx
    subq $40, %rsp
    call putchar
    addq $40, %rsp
    pushq $0
    popq %rax
    movl %eax, -16(%rbp)
    pushq $1
    popq %rax
    movl %eax, -20(%rbp)
.L_for_start_0:
    movl -20(%rbp), %eax
    pushq %rax
    pushq $5
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_0

    movl $0, %eax
    leave
    ret
