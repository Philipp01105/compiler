    .section .rdata,"dr"
.LC0:
    .string "================================="
.LC1:
    .string "=== COMPILER TEST SUITE ==="
.LC2:
    .string "Version 1.1 - Vollständiger Feature-Test"
.LC3:
    .string ""
.LC4:
    .string "=== TEST 1: Variablen ==="
.LC5:
    .string "testVar1 = "
.LC6:
    .string "%d"
.LC7:
    .string "testVar2 = "
.LC8:
    .string "%d"
.LC9:
    .string "testVar3 = "
.LC10:
    .string "%d"
.LC11:
    .string "testVar4 = "
.LC12:
    .string "%d"
.LC13:
    .string ""
.LC14:
    .string "=== TEST 2: Arithmetische Operatoren ==="
.LC15:
    .string "a = "
.LC16:
    .string "%d"
.LC17:
    .string ", b = "
.LC18:
    .string "%d"
.LC19:
    .string "a + b = "
.LC20:
    .string "%d"
.LC21:
    .string "a - b = "
.LC22:
    .string "%d"
.LC23:
    .string "a * b = "
.LC24:
    .string "%d"
.LC25:
    .string "a / b = "
.LC26:
    .string "%d"
.LC27:
    .string ""
.LC28:
    .string "=== TEST 3: Compound Assignment ==="
.LC29:
    .string "Start: x = "
.LC30:
    .string "%d"
.LC31:
    .string "Nach x += 5: x = "
.LC32:
    .string "%d"
.LC33:
    .string "Nach x -= 3: x = "
.LC34:
    .string "%d"
.LC35:
    .string "Nach x *= 2: x = "
.LC36:
    .string "%d"
.LC37:
    .string "Nach x /= 4: x = "
.LC38:
    .string "%d"
.LC39:
    .string ""
.LC40:
    .string "=== TEST 4: Vergleichsoperatoren ==="
.LC41:
    .string "Werden in For-Schleifen getestet..."
.LC42:
    .string "Test '<' (kleiner als):"
.LC43:
    .string "  i = "
.LC44:
    .string "%d"
.LC45:
    .string " (läuft 3 Mal)"
.LC46:
    .string "Test '<=' (kleiner oder gleich):"
.LC47:
    .string "  i = "
.LC48:
    .string "%d"
.LC49:
    .string " (läuft 4 Mal)"
.LC50:
    .string "Test '>' (größer als):"
.LC51:
    .string "  i = "
.LC52:
    .string "%d"
.LC53:
    .string " (countdown)"
.LC54:
    .string ""
.LC55:
    .string "=== TEST 5: For-Schleifen ==="
.LC56:
    .string "Variante 1: var in for"
.LC57:
    .string "  i = "
.LC58:
    .string "%d"
.LC59:
    .string "Variante 2: var vorher deklariert"
.LC60:
    .string "  j = "
.LC61:
    .string "%d"
.LC62:
    .string "Variante 3: i++ (Inkrement)"
.LC63:
    .string "  k = "
.LC64:
    .string "%d"
.LC65:
    .string "Variante 4: i-- (Dekrement)"
.LC66:
    .string "  m = "
.LC67:
    .string "%d"
.LC68:
    .string "Variante 5: Schrittweite 2"
.LC69:
    .string "  n = "
.LC70:
    .string "%d"
.LC71:
    .string ""
.LC72:
    .string "=== TEST 6: Verschachtelte Schleifen ==="
.LC73:
    .string "Multiplikationstabelle (Ausschnitt):"
.LC74:
    .string "  "
.LC75:
    .string "%d"
.LC76:
    .string " * "
.LC77:
    .string "%d"
.LC78:
    .string " = "
.LC79:
    .string "%d"
.LC80:
    .string ""
.LC81:
    .string "=== TEST 7: Funktionen ==="
.LC82:
    .string "Funktion ohne Parameter:"
.LC83:
    .string "  getConstant() = "
.LC84:
    .string "%d"
.LC85:
    .string "Funktion mit 2 Parametern:"
.LC86:
    .string "  add(15, 27) = "
.LC87:
    .string "%d"
.LC88:
    .string "Funktion mit void Return:"
.LC89:
    .string "Funktionen mit Berechnungen:"
.LC90:
    .string "  multiply(6, 7) = "
.LC91:
    .string "%d"
.LC92:
    .string "  divide(20, 4) = "
.LC93:
    .string "%d"
.LC94:
    .string ""
.LC95:
    .string "=== TEST 8: Komplexe Funktionen ==="
.LC96:
    .string "Fakultät:"
.LC97:
    .string "  "
.LC98:
    .string "%d"
.LC99:
    .string "! = "
.LC100:
    .string "%d"
.LC101:
    .string ""
.LC102:
    .string "Fibonacci:"
.LC103:
    .string "  F("
.LC104:
    .string "%d"
.LC105:
    .string ") = "
.LC106:
    .string "%d"
.LC107:
    .string ""
.LC108:
    .string "Potenz:"
.LC109:
    .string "  2^"
.LC110:
    .string "%d"
.LC111:
    .string " = "
.LC112:
    .string "%d"
.LC113:
    .string ""
.LC114:
    .string "=== TEST 9: Lokale Variablen ==="
.LC115:
    .string "calculate(5, 3) benutzt lokale Variablen:"
.LC116:
    .string "  Ergebnis: "
.LC117:
    .string "%d"
.LC118:
    .string ""
.LC119:
    .string "=== TEST 10: Scope-Test ==="
.LC120:
    .string "Mehrere Schleifen mit var i:"
.LC121:
    .string "  Schleife 1: i = "
.LC122:
    .string "%d"
.LC123:
    .string "  Schleife 2: i = "
.LC124:
    .string "%d"
.LC125:
    .string "  Schleife 3: i = "
.LC126:
    .string "%d"
.LC127:
    .string ""
.LC128:
    .string "=== TEST 11: String-Konkatenation ==="
.LC129:
    .string "Einfach: "
.LC130:
    .string "%d"
.LC131:
    .string "Mehrfach: "
.LC132:
    .string "%d"
.LC133:
    .string " und "
.LC134:
    .string "%d"
.LC135:
    .string "Mit Berechnung: "
.LC136:
    .string "%d"
.LC137:
    .string "Text mit '+' Zeichen: 1 + 1 = "
.LC138:
    .string "%d"
.LC139:
    .string ""
.LC140:
    .string "=== TEST 12: Modulo ==="
.LC141:
    .string "  "
.LC142:
    .string "%d"
.LC143:
    .string " mod 3 = "
.LC144:
    .string "%d"
.LC145:
    .string ""
.LC146:
    .string "=== TEST 13: Gerade/Ungerade ==="
.LC147:
    .string "  "
.LC148:
    .string "%d"
.LC149:
    .string " ist gerade: "
.LC150:
    .string "%d"
.LC151:
    .string ""
.LC152:
    .string "=== TEST 14: Maximum ==="
.LC153:
    .string "  max(10, 20) = "
.LC154:
    .string "%d"
.LC155:
    .string "  max(30, 15) = "
.LC156:
    .string "%d"
.LC157:
    .string "  max(7, 7) = "
.LC158:
    .string "%d"
.LC159:
    .string ""
.LC160:
    .string "=== TEST 15: GGT ==="
.LC161:
    .string "  gcd(48, 18) = "
.LC162:
    .string "%d"
.LC163:
    .string "  gcd(100, 35) = "
.LC164:
    .string "%d"
.LC165:
    .string "  gcd(17, 19) = "
.LC166:
    .string "%d"
.LC167:
    .string ""
.LC168:
    .string "=== TEST 16: Funktionsaufruf in Expressions ==="
.LC169:
    .string "  add(10, multiply(2, 3)) = "
.LC170:
    .string "%d"
.LC171:
    .string "  multiply(add(5, 5), 2) = "
.LC172:
    .string "%d"
.LC173:
    .string ""
.LC174:
    .string "=== TEST 17: Stress-Test ==="
.LC175:
    .string "  Summe von 1-10: "
.LC176:
    .string "%d"
.LC177:
    .string ""
.LC178:
    .string "=== TEST 18: Tiefe Verschachtelung ==="
.LC179:
    .string "  i="
.LC180:
    .string "%d"
.LC181:
    .string " j="
.LC182:
    .string "%d"
.LC183:
    .string " k="
.LC184:
    .string "%d"
.LC185:
    .string " sum="
.LC186:
    .string "%d"
.LC187:
    .string ""
.LC188:
    .string "=== ALLE TESTS ABGESCHLOSSEN ==="
.LC189:
    .string "Status: ERFOLGREICH"
    .text

.globl add
add:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl subtract
subtract:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    subl %ebx, %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl multiply
multiply:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl divide
divide:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cltd
    idivl %ebx
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl calculate
calculate:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
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
    movl -4(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -16(%rbp)
    movl -12(%rbp), %eax
    pushq %rax
    movl -16(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -20(%rbp)
    movl -20(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl getConstant
getConstant:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    pushq $42
    popq %rax
    leave
    ret
    leave
    ret


.globl printSeparator
printSeparator:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    leaq .LC0(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl $0, %eax
    leave
    ret


.globl factorial
factorial:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    pushq $1
    popq %rax
    movl %eax, -8(%rbp)
    pushq $1
    popq %rax
    movl %eax, -12(%rbp)
.L_for_start_0:
    movl -12(%rbp), %eax
    pushq %rax
    movl -4(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_0
    movl -8(%rbp), %eax
    pushq %rax
    movl -12(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    movl %eax, -8(%rbp)
    movl -12(%rbp), %eax
    addl $1, %eax
    movl %eax, -12(%rbp)
    jmp .L_for_start_0
.L_for_end_0:
    movl -8(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl fibonacci
fibonacci:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    pushq $0
    popq %rax
    movl %eax, -8(%rbp)
    pushq $1
    popq %rax
    movl %eax, -12(%rbp)
    pushq $0
    popq %rax
    movl %eax, -16(%rbp)
.L_for_start_1:
    movl -16(%rbp), %eax
    pushq %rax
    movl -4(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_1
    movl -8(%rbp), %eax
    pushq %rax
    movl -12(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -20(%rbp)
    movl -12(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, -8(%rbp)
    movl -20(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, -12(%rbp)
    movl -16(%rbp), %eax
    addl $1, %eax
    movl %eax, -16(%rbp)
    jmp .L_for_start_1
.L_for_end_1:
    movl -8(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl power
power:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    pushq $1
    popq %rax
    movl %eax, -12(%rbp)
    pushq $0
    popq %rax
    movl %eax, -16(%rbp)
.L_for_start_2:
    movl -16(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_2
    movl -12(%rbp), %eax
    pushq %rax
    movl -4(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    movl %eax, -12(%rbp)
    movl -16(%rbp), %eax
    addl $1, %eax
    movl %eax, -16(%rbp)
    jmp .L_for_start_2
.L_for_end_2:
    movl -12(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl max
max:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, -12(%rbp)
    movl -8(%rbp), %eax
    pushq %rax
    movl -4(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    subl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -16(%rbp)
    pushq $0
    popq %rax
    movl %eax, -20(%rbp)
.L_for_start_3:
    movl -20(%rbp), %eax
    pushq %rax
    pushq $1
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_3
    pushq $0
    popq %rax
    movl %eax, -24(%rbp)
    pushq $0
    popq %rax
    movl %eax, -28(%rbp)
.L_for_start_4:
    movl -28(%rbp), %eax
    pushq %rax
    movl -16(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_4
    pushq $1
    popq %rax
    movl %eax, -24(%rbp)
    movl -28(%rbp), %eax
    addl $1, %eax
    movl %eax, -28(%rbp)
    jmp .L_for_start_4
.L_for_end_4:
    movl -8(%rbp), %eax
    pushq %rax
    movl -24(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -32(%rbp)
    movl -12(%rbp), %eax
    pushq %rax
    pushq $1
    movl -24(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    subl %ebx, %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -36(%rbp)
    movl -32(%rbp), %eax
    pushq %rax
    movl -36(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -12(%rbp)
    movl -20(%rbp), %eax
    addl $1, %eax
    movl %eax, -20(%rbp)
    jmp .L_for_start_3
.L_for_end_3:
    movl -12(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl modulo
modulo:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cltd
    idivl %ebx
    pushq %rax
    popq %rax
    movl %eax, -12(%rbp)
    movl -12(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -16(%rbp)
    movl -4(%rbp), %eax
    pushq %rax
    movl -16(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    subl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -20(%rbp)
    movl -20(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl isEven
isEven:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    pushq $2
    movl -4(%rbp), %eax
    pushq %rax
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call modulo
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -8(%rbp)
    pushq $1
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    subl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -12(%rbp)
    movl -12(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl gcd
gcd:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)
    movl %edx, -8(%rbp)
    pushq $0
    popq %rax
    movl %eax, -12(%rbp)
.L_for_start_5:
    movl -12(%rbp), %eax
    pushq %rax
    pushq $100
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_5
    pushq $1
    popq %rax
    movl %eax, -16(%rbp)
    pushq $0
    popq %rax
    movl %eax, -20(%rbp)
.L_for_start_6:
    movl -20(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_6
    pushq $0
    popq %rax
    movl %eax, -16(%rbp)
    movl -20(%rbp), %eax
    addl $1, %eax
    movl %eax, -20(%rbp)
    jmp .L_for_start_6
.L_for_end_6:
    pushq $0
    popq %rax
    movl %eax, -24(%rbp)
    pushq $0
    popq %rax
    movl %eax, -28(%rbp)
.L_for_start_7:
    movl -28(%rbp), %eax
    pushq %rax
    movl -8(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_7
    pushq $1
    popq %rax
    movl %eax, -24(%rbp)
    movl -28(%rbp), %eax
    addl $1, %eax
    movl %eax, -28(%rbp)
    jmp .L_for_start_7
.L_for_end_7:
    pushq $0
    popq %rax
    movl %eax, -32(%rbp)
.L_for_start_8:
    movl -32(%rbp), %eax
    pushq %rax
    movl -24(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_8
    movl -8(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, -36(%rbp)
    movl -8(%rbp), %eax
    pushq %rax
    movl -4(%rbp), %eax
    pushq %rax
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call modulo
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -8(%rbp)
    movl -36(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, -4(%rbp)
    movl -32(%rbp), %eax
    addl $1, %eax
    movl %eax, -32(%rbp)
    jmp .L_for_start_8
.L_for_end_8:
    movl -12(%rbp), %eax
    addl $1, %eax
    movl %eax, -12(%rbp)
    jmp .L_for_start_5
.L_for_end_5:
    movl -4(%rbp), %eax
    pushq %rax
    popq %rax
    leave
    ret
    leave
    ret


.globl main
main:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC1(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC2(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC3(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC4(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $10
    popq %rax
    movl %eax, -4(%rbp)
    pushq $20
    popq %rax
    movl %eax, -8(%rbp)
    pushq $-5
    popq %rax
    movl %eax, -12(%rbp)
    pushq $0
    popq %rax
    movl %eax, -16(%rbp)
    leaq .LC5(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -4(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC6(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC7(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -8(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC8(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC9(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -12(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC10(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC11(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -16(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC12(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC13(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC14(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $10
    popq %rax
    movl %eax, -20(%rbp)
    pushq $3
    popq %rax
    movl %eax, -24(%rbp)
    movl -20(%rbp), %eax
    pushq %rax
    movl -24(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -28(%rbp)
    movl -20(%rbp), %eax
    pushq %rax
    movl -24(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    subl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -32(%rbp)
    movl -20(%rbp), %eax
    pushq %rax
    movl -24(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -36(%rbp)
    movl -20(%rbp), %eax
    pushq %rax
    movl -24(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    cltd
    idivl %ebx
    pushq %rax
    popq %rax
    movl %eax, -40(%rbp)
    leaq .LC15(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -20(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC16(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC17(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -24(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC18(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC19(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -28(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC20(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC21(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -32(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC22(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC23(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -36(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC24(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC25(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -40(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC26(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC27(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC28(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $10
    popq %rax
    movl %eax, -44(%rbp)
    leaq .LC29(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC30(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    pushq $5
    popq %rbx
    popq %rax
    addl %ebx, %eax
    movl %eax, -44(%rbp)
    leaq .LC31(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC32(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    pushq $3
    popq %rbx
    popq %rax
    subl %ebx, %eax
    movl %eax, -44(%rbp)
    leaq .LC33(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC34(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    pushq $2
    popq %rbx
    popq %rax
    imull %ebx, %eax
    movl %eax, -44(%rbp)
    leaq .LC35(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC36(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    pushq $4
    popq %rbx
    popq %rax
    cltd
    idivl %ebx
    movl %eax, -44(%rbp)
    leaq .LC37(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -44(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC38(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC39(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC40(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC41(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC42(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -48(%rbp)
.L_for_start_9:
    movl -48(%rbp), %eax
    pushq %rax
    pushq $3
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_9
    leaq .LC43(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -48(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC44(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC45(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -48(%rbp), %eax
    addl $1, %eax
    movl %eax, -48(%rbp)
    jmp .L_for_start_9
.L_for_end_9:
    leaq .LC46(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -52(%rbp)
.L_for_start_10:
    movl -52(%rbp), %eax
    pushq %rax
    pushq $3
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_10
    leaq .LC47(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -52(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC48(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC49(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -52(%rbp), %eax
    addl $1, %eax
    movl %eax, -52(%rbp)
    jmp .L_for_start_10
.L_for_end_10:
    leaq .LC50(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $5
    popq %rax
    movl %eax, -56(%rbp)
.L_for_start_11:
    movl -56(%rbp), %eax
    pushq %rax
    pushq $2
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setg %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_11
    leaq .LC51(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -56(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC52(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC53(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -56(%rbp), %eax
    subl $1, %eax
    movl %eax, -56(%rbp)
    jmp .L_for_start_11
.L_for_end_11:
    leaq .LC54(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC55(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC56(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -60(%rbp)
.L_for_start_12:
    movl -60(%rbp), %eax
    pushq %rax
    pushq $5
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_12
    leaq .LC57(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -60(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC58(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -60(%rbp), %eax
    addl $1, %eax
    movl %eax, -60(%rbp)
    jmp .L_for_start_12
.L_for_end_12:
    leaq .LC59(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -64(%rbp)
    pushq $0
    popq %rax
    movl %eax, -64(%rbp)
.L_for_start_13:
    movl -64(%rbp), %eax
    pushq %rax
    pushq $3
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_13
    leaq .LC60(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -64(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC61(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -64(%rbp), %eax
    addl $1, %eax
    movl %eax, -64(%rbp)
    jmp .L_for_start_13
.L_for_end_13:
    leaq .LC62(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -68(%rbp)
.L_for_start_14:
    movl -68(%rbp), %eax
    pushq %rax
    pushq $5
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_14
    leaq .LC63(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -68(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC64(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -68(%rbp), %eax
    addl $1, %eax
    movl %eax, -68(%rbp)
    jmp .L_for_start_14
.L_for_end_14:
    leaq .LC65(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $5
    popq %rax
    movl %eax, -72(%rbp)
.L_for_start_15:
    movl -72(%rbp), %eax
    pushq %rax
    pushq $0
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setg %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_15
    leaq .LC66(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -72(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC67(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -72(%rbp), %eax
    subl $1, %eax
    movl %eax, -72(%rbp)
    jmp .L_for_start_15
.L_for_end_15:
    leaq .LC68(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -76(%rbp)
.L_for_start_16:
    movl -76(%rbp), %eax
    pushq %rax
    pushq $10
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_16
    leaq .LC69(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -76(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC70(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -76(%rbp), %eax
    pushq %rax
    pushq $2
    popq %rbx
    popq %rax
    addl %ebx, %eax
    movl %eax, -76(%rbp)
    jmp .L_for_start_16
.L_for_end_16:
    leaq .LC71(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC72(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC73(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $1
    popq %rax
    movl %eax, -80(%rbp)
.L_for_start_17:
    movl -80(%rbp), %eax
    pushq %rax
    pushq $3
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_17
    pushq $1
    popq %rax
    movl %eax, -84(%rbp)
.L_for_start_18:
    movl -84(%rbp), %eax
    pushq %rax
    pushq $3
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_18
    movl -80(%rbp), %eax
    pushq %rax
    movl -84(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    imull %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -88(%rbp)
    leaq .LC74(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -80(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC75(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC76(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -84(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC77(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC78(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -88(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC79(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -84(%rbp), %eax
    addl $1, %eax
    movl %eax, -84(%rbp)
    jmp .L_for_start_18
.L_for_end_18:
    movl -80(%rbp), %eax
    addl $1, %eax
    movl %eax, -80(%rbp)
    jmp .L_for_start_17
.L_for_end_17:
    leaq .LC80(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC81(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC82(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    subq $32, %rsp
    call getConstant
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -92(%rbp)
    leaq .LC83(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -92(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC84(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC85(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $27
    pushq $15
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call add
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -96(%rbp)
    leaq .LC86(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -96(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC87(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC88(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC89(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $7
    pushq $6
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call multiply
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -100(%rbp)
    pushq $4
    pushq $20
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call divide
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -104(%rbp)
    leaq .LC90(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -100(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC91(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC92(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -104(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC93(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC94(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC95(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC96(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $1
    popq %rax
    movl %eax, -108(%rbp)
.L_for_start_19:
    movl -108(%rbp), %eax
    pushq %rax
    pushq $10
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_19
    movl -108(%rbp), %eax
    pushq %rax
    popq %rcx
    subq $32, %rsp
    call factorial
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -112(%rbp)
    leaq .LC97(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -108(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC98(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC99(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -112(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC100(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -108(%rbp), %eax
    addl $1, %eax
    movl %eax, -108(%rbp)
    jmp .L_for_start_19
.L_for_end_19:
    leaq .LC101(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC102(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -116(%rbp)
.L_for_start_20:
    movl -116(%rbp), %eax
    pushq %rax
    pushq $10
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_20
    movl -116(%rbp), %eax
    pushq %rax
    popq %rcx
    subq $32, %rsp
    call fibonacci
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -120(%rbp)
    leaq .LC103(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -116(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC104(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC105(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -120(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC106(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -116(%rbp), %eax
    addl $1, %eax
    movl %eax, -116(%rbp)
    jmp .L_for_start_20
.L_for_end_20:
    leaq .LC107(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC108(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -124(%rbp)
.L_for_start_21:
    movl -124(%rbp), %eax
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
    je .L_for_end_21
    movl -124(%rbp), %eax
    pushq %rax
    pushq $2
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call power
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -128(%rbp)
    leaq .LC109(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -124(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC110(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC111(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -128(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC112(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -124(%rbp), %eax
    addl $1, %eax
    movl %eax, -124(%rbp)
    jmp .L_for_start_21
.L_for_end_21:
    leaq .LC113(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC114(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $3
    pushq $5
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call calculate
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -132(%rbp)
    leaq .LC115(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC116(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -132(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC117(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC118(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC119(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC120(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -136(%rbp)
.L_for_start_22:
    movl -136(%rbp), %eax
    pushq %rax
    pushq $3
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_22
    leaq .LC121(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -136(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC122(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -136(%rbp), %eax
    addl $1, %eax
    movl %eax, -136(%rbp)
    jmp .L_for_start_22
.L_for_end_22:
    pushq $10
    popq %rax
    movl %eax, -140(%rbp)
.L_for_start_23:
    movl -140(%rbp), %eax
    pushq %rax
    pushq $13
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_23
    leaq .LC123(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -140(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC124(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -140(%rbp), %eax
    addl $1, %eax
    movl %eax, -140(%rbp)
    jmp .L_for_start_23
.L_for_end_23:
    pushq $20
    popq %rax
    movl %eax, -144(%rbp)
.L_for_start_24:
    movl -144(%rbp), %eax
    pushq %rax
    pushq $23
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_24
    leaq .LC125(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -144(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC126(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -144(%rbp), %eax
    addl $1, %eax
    movl %eax, -144(%rbp)
    jmp .L_for_start_24
.L_for_end_24:
    leaq .LC127(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC128(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $42
    popq %rax
    movl %eax, -148(%rbp)
    pushq $58
    popq %rax
    movl %eax, -152(%rbp)
    leaq .LC129(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -148(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC130(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC131(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -148(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC132(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC133(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -152(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC134(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC135(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -148(%rbp), %eax
    pushq %rax
    movl -152(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC136(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC137(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    pushq $2
    popq %rax
    movl %eax, %edx
    leaq .LC138(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC139(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC140(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $10
    popq %rax
    movl %eax, -156(%rbp)
.L_for_start_25:
    movl -156(%rbp), %eax
    pushq %rax
    pushq $15
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_25
    pushq $3
    movl -156(%rbp), %eax
    pushq %rax
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call modulo
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -160(%rbp)
    leaq .LC141(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -156(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC142(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC143(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -160(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC144(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -156(%rbp), %eax
    addl $1, %eax
    movl %eax, -156(%rbp)
    jmp .L_for_start_25
.L_for_end_25:
    leaq .LC145(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC146(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -164(%rbp)
.L_for_start_26:
    movl -164(%rbp), %eax
    pushq %rax
    pushq $10
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setle %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_26
    movl -164(%rbp), %eax
    pushq %rax
    popq %rcx
    subq $32, %rsp
    call isEven
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -168(%rbp)
    leaq .LC147(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -164(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC148(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC149(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -168(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC150(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -164(%rbp), %eax
    addl $1, %eax
    movl %eax, -164(%rbp)
    jmp .L_for_start_26
.L_for_end_26:
    leaq .LC151(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC152(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $20
    pushq $10
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call max
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -172(%rbp)
    pushq $15
    pushq $30
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call max
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -176(%rbp)
    pushq $7
    pushq $7
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call max
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -180(%rbp)
    leaq .LC153(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -172(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC154(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC155(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -176(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC156(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC157(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -180(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC158(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC159(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC160(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $18
    pushq $48
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call gcd
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -184(%rbp)
    pushq $35
    pushq $100
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call gcd
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -188(%rbp)
    pushq $19
    pushq $17
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call gcd
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -192(%rbp)
    leaq .LC161(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -184(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC162(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC163(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -188(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC164(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC165(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -192(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC166(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC167(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC168(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $3
    pushq $2
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call multiply
    addq $32, %rsp
    pushq %rax
    pushq $10
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call add
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -196(%rbp)
    pushq $2
    pushq $5
    pushq $5
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call add
    addq $32, %rsp
    pushq %rax
    popq %rcx
    popq %rdx
    subq $32, %rsp
    call multiply
    addq $32, %rsp
    pushq %rax
    popq %rax
    movl %eax, -200(%rbp)
    leaq .LC169(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -196(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC170(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC171(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -200(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC172(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC173(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC174(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $1
    popq %rax
    movl %eax, -204(%rbp)
    pushq $2
    popq %rax
    movl %eax, -208(%rbp)
    pushq $3
    popq %rax
    movl %eax, -212(%rbp)
    pushq $4
    popq %rax
    movl %eax, -216(%rbp)
    pushq $5
    popq %rax
    movl %eax, -220(%rbp)
    pushq $6
    popq %rax
    movl %eax, -224(%rbp)
    pushq $7
    popq %rax
    movl %eax, -228(%rbp)
    pushq $8
    popq %rax
    movl %eax, -232(%rbp)
    pushq $9
    popq %rax
    movl %eax, -236(%rbp)
    pushq $10
    popq %rax
    movl %eax, -240(%rbp)
    movl -204(%rbp), %eax
    pushq %rax
    movl -208(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -212(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -216(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -220(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -224(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -228(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -232(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -236(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -240(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -244(%rbp)
    leaq .LC175(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -244(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC176(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC177(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC178(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    pushq $0
    popq %rax
    movl %eax, -248(%rbp)
.L_for_start_27:
    movl -248(%rbp), %eax
    pushq %rax
    pushq $2
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_27
    pushq $0
    popq %rax
    movl %eax, -252(%rbp)
.L_for_start_28:
    movl -252(%rbp), %eax
    pushq %rax
    pushq $2
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_28
    pushq $0
    popq %rax
    movl %eax, -256(%rbp)
.L_for_start_29:
    movl -256(%rbp), %eax
    pushq %rax
    pushq $2
    popq %rbx
    popq %rax
    cmpl %ebx, %eax
    setl %al
    movzbl %al, %eax
    pushq %rax
    popq %rax
    testl %eax, %eax
    je .L_for_end_29
    movl -248(%rbp), %eax
    pushq %rax
    movl -252(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    movl -256(%rbp), %eax
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax
    pushq %rax
    popq %rax
    movl %eax, -260(%rbp)
    leaq .LC179(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -248(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC180(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC181(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -252(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC182(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC183(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -256(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC184(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    leaq .LC185(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl -260(%rbp), %eax
    pushq %rax
    popq %rax
    movl %eax, %edx
    leaq .LC186(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl -256(%rbp), %eax
    addl $1, %eax
    movl %eax, -256(%rbp)
    jmp .L_for_start_29
.L_for_end_29:
    movl -252(%rbp), %eax
    addl $1, %eax
    movl %eax, -252(%rbp)
    jmp .L_for_start_28
.L_for_end_28:
    movl -248(%rbp), %eax
    addl $1, %eax
    movl %eax, -248(%rbp)
    jmp .L_for_start_27
.L_for_end_27:
    leaq .LC187(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC188(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    leaq .LC189(%rip), %rcx
    subq $32, %rsp
    call printf
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl $10, %ecx
    subq $32, %rsp
    call putchar
    addq $32, %rsp
    movl $0, %eax
    leave
    ret

