; ---------------------------------------------------------------------------
; core.inc - calculator logic and screen drawing, shared by every target.
;
; The same source is assembled as 16-bit real mode (BIOS), 32-bit protected
; mode (IA32 UEFI) and 64-bit long mode (x64 UEFI).  To make that possible:
;
;   * xax, xbx, xcx, xdx, xsi, xdi are the "pointer sized" registers
;     (eax.. in 16/32-bit builds, rax.. in the 64-bit build).
;   * every memory reference to a variable is written [M(name)], which
;     expands to the right addressing form for the target.
;   * xbp is reserved (IA32 UEFI keeps the image load address there).
;   * numbers are 80-bit x87 values, so the maths is identical everywhere.
;
; The platform layer must provide (all registers preserved unless noted):
;   plat_setattr   al = colour attribute (VGA style: bg<<4 | fg)
;   plat_gotoxy    dl = column, dh = row   (80x25 logical screen)
;   plat_putc      al = CP437 character, written at the cursor, cursor moves
;   plat_getkey    returns eax = ASCII code or one of the KEY_* codes
;   plat_exit      never returns
;   plat_exit_text zero terminated word shown after "Esc: " in the help bar
; ---------------------------------------------------------------------------

KEY_ESC     equ 27
KEY_UP      equ 0x100
KEY_DOWN    equ 0x101
KEY_LEFT    equ 0x102
KEY_RIGHT   equ 0x103
KEY_DEL     equ 0x104

ATTR_BG     equ 0x17            ; screen background: grey on blue
ATTR_TITLE  equ 0x3F            ; title bar: white on cyan
ATTR_FOOT   equ 0x30            ; help bar: black on cyan
ATTR_PANEL  equ 0x70            ; both panels and key outlines: black on grey
ATTR_EXPR   equ 0x02            ; display, expression line: green on black
ATTR_VAL    equ 0x0A            ; display, value line: bright green on black
ATTR_DIGIT  equ 0x70            ; key labels are coloured text on the panel
ATTR_OPER   equ 0x71            ; background: digits black, operators blue,
ATTR_MEM    equ 0x75            ; memory magenta, DEL and C red, = green
ATTR_CLR    equ 0x74
ATTR_EQ     equ 0x72
ATTR_SEL    equ 0x7F            ; selected key: white, double outline
ATTR_TAPE   equ 0x70            ; history, expression lines
ATTR_RESULT equ 0x71            ; history, result lines: blue on grey

; --- screen layout (80x25) ---
; One frame, split by a vertical line: display and history on the left,
; the keys on the right.
FX          equ 3               ; frame position and size
PY          equ 2
FW          equ 73
PH          equ 22
DIV_X       equ FX + 36         ; column of the dividing line
DISP_X      equ FX + 2          ; display: 4 rows tall, text on the middle
DISP_Y      equ PY + 3          ; two (this is the expression row)
DISP_W      equ DIV_X - FX - 3
TAPE_Y      equ PY + 8          ; first history row (below the separator)
TAPE_W      equ DISP_W          ; characters per history line
TAPE_LINES  equ 12              ; 6 calculations, newest on top
BTN_COLS    equ 4
BTN_ROWS    equ 6
BTN_COUNT   equ BTN_COLS * BTN_ROWS
BTN_EQUALS  equ BTN_COUNT - 1   ; index of the "=" key
BTN_X       equ DIV_X + 3       ; first key position; keys are 7x3 cells,
BTN_Y       equ PY + 2          ; 8 columns and 3 rows apart

%ifndef FOOT_W                  ; help bar width; a platform may shorten it
  %define FOOT_W 80
%endif

; --- limits ---
MAX_INT     equ 12              ; digits allowed before the decimal point
MAX_FRAC    equ 6               ; digits allowed after it
MAX_DEPTH   equ 8               ; nested brackets
VS_MAX      equ 32              ; value stack entries
OS_MAX      equ 40              ; operator stack entries
XMAX        equ 48              ; expression text kept for display

; --- what the previous key press left behind ---
L_START     equ 0               ; nothing pending; cur holds the last result
L_OPERAND   equ 1               ; cur holds an operand (typed, MR or +/-)
L_OP        equ 2               ; a binary operator is waiting for its operand
L_OPEN      equ 3               ; just after "("
L_CLOSED    equ 4               ; cur holds the value of a bracket group

; ---------------------------------------------------------------------------
; Main loop
; ---------------------------------------------------------------------------
calc_main:
        call    calc_init
        mov     byte [M(sel)], BTN_EQUALS
        call    draw_screen
.loop:
        call    draw_display
        cmp     byte [M(tape_dirty)], 0
        je      .wait
        call    draw_tape
        mov     byte [M(tape_dirty)], 0
.wait:
        call    plat_getkey
        cmp     eax, KEY_ESC
        je      plat_exit
        cmp     eax, KEY_UP
        je      .up
        cmp     eax, KEY_DOWN
        je      .down
        cmp     eax, KEY_LEFT
        je      .left
        cmp     eax, KEY_RIGHT
        je      .right
        cmp     eax, KEY_DEL
        jne     .notdel
        mov     eax, 8                  ; Delete acts like Backspace
.notdel:
        cmp     eax, 13                 ; Enter / Space press the selected key
        je      .press
        cmp     eax, ' '
        jne     .key
.press:
        movzx   ecx, byte [M(sel)]
        lea     xsi, [M(btn_keys)]
        movzx   eax, byte [xsi + xcx]
.key:
        cmp     eax, 0x7F
        jae     .loop
        call    handle_key
        jmp     .loop

; --- move the selection; dl = column, dh = row while adjusting ---
.up:
        call    .split
        add     dh, BTN_ROWS - 1
        jmp     .fixrow
.down:
        call    .split
        inc     dh
.fixrow:
        cmp     dh, BTN_ROWS
        jb      .moved
        sub     dh, BTN_ROWS
        jmp     .moved
.left:
        call    .split
        add     dl, BTN_COLS - 1
        jmp     .fixcol
.right:
        call    .split
        inc     dl
.fixcol:
        cmp     dl, BTN_COLS
        jb      .moved
        sub     dl, BTN_COLS
.moved:
        mov     al, dh
        mov     ah, BTN_COLS
        mul     ah                      ; ax = row * columns
        add     al, dl
        movzx   ecx, byte [M(sel)]      ; old selection
        mov     [M(sel)], al
        movzx   eax, al
        call    draw_button             ; new one, now highlighted
        mov     eax, ecx
        call    draw_button             ; old one, back to normal
        jmp     .loop

.split:                                 ; sel -> dl = column, dh = row
        movzx   eax, byte [M(sel)]
        mov     cl, BTN_COLS
        div     cl                      ; al = row, ah = column
        mov     dl, ah
        mov     dh, al
        ret

; ---------------------------------------------------------------------------
; Key handling.  handle_key: al = key character
;
; Expressions are evaluated with the usual precedence (^ before * / before
; + -, ^ right-associative) using a value stack and an operator stack.  A
; binary operator is not put on the operator stack when pressed but kept in
; "pend" until its right operand arrives, so pressing another operator
; simply replaces it.
; ---------------------------------------------------------------------------
handle_key:
        cmp     byte [M(err)], 0        ; any key clears an error first
        je      .noerr
        push    xax
        call    calc_reset
        pop     xax
.noerr:
        cmp     al, '0'
        jb      .notdigit
        cmp     al, '9'
        jbe     .digit
.notdigit:
        cmp     al, '.'
        je      .point
        cmp     al, 8
        je      .bksp
        cmp     al, '+'
        je      .oper
        cmp     al, '-'
        je      .oper
        cmp     al, '*'
        je      .oper
        cmp     al, '/'
        je      .oper
        cmp     al, '^'
        je      .oper
        cmp     al, '('
        je      .lparen
        cmp     al, ')'
        je      .rparen
        cmp     al, '='
        je      .equals
        or      al, 0x20                ; letters: ignore case
        cmp     al, 'c'
        je      calc_reset
        cmp     al, 'n'
        je      .negate
        cmp     al, 'm'
        je      .madd
        cmp     al, 'r'
        je      .mrecall
.ret:
        ret

; --- typing a number ---
.digit:
        cmp     byte [M(last)], L_CLOSED
        je      .ret
        cmp     byte [M(entering)], 0
        jne     .d1
        call    start_entry
.d1:
        lea     xsi, [M(ebuf)]
        movzx   ecx, byte [M(elen)]
        cmp     ecx, 1                  ; a lone "0" is replaced, not extended
        jne     .d2
        cmp     byte [xsi], '0'
        jne     .d2
        mov     [xsi], al
        ret
.d2:
        xor     edx, edx
.d3:
        cmp     edx, ecx                ; look for a decimal point
        jae     .d5
        cmp     byte [xsi + xdx], '.'
        je      .d4
        inc     edx
        jmp     .d3
.d4:
        mov     ebx, ecx                ; digits after the point so far
        sub     ebx, edx
        dec     ebx
        cmp     ebx, MAX_FRAC
        jae     .ret
        jmp     .append
.d5:
        cmp     ecx, MAX_INT
        jae     .ret
.append:
        mov     [xsi + xcx], al
        inc     byte [M(elen)]
        ret

.point:
        cmp     byte [M(last)], L_CLOSED
        je      .ret
        cmp     byte [M(entering)], 0
        jne     .p1
        call    start_entry
.p1:
        lea     xsi, [M(ebuf)]
        movzx   ecx, byte [M(elen)]
        test    ecx, ecx
        jnz     .p2
        mov     byte [xsi], '0'         ; "." alone becomes "0."
        inc     ecx
        mov     [M(elen)], cl
.p2:
        xor     edx, edx
.p3:
        cmp     edx, ecx
        jae     .p4
        cmp     byte [xsi + xdx], '.'   ; only one point per number
        je      .ret
        inc     edx
        jmp     .p3
.p4:
        mov     byte [xsi + xcx], '.'
        inc     byte [M(elen)]
        ret

.bksp:
        cmp     byte [M(entering)], 0
        je      .ret
        movzx   ecx, byte [M(elen)]
        cmp     ecx, 1
        ja      .b1
        lea     xsi, [M(ebuf)]
        mov     byte [xsi], '0'
        mov     byte [M(elen)], 1
        ret
.b1:
        dec     byte [M(elen)]
        ret

; --- + - * / ^ ---
.oper:
        push    xax
        call    commit
        pop     xax
        cmp     byte [M(last)], L_OPEN  ; "(" needs an operand first
        je      .ret
        cmp     byte [M(last)], L_OP
        jne     .o1
        mov     [M(pend)], al           ; "5 + *": swap the operator
        movzx   ecx, byte [M(xlen)]
        lea     xsi, [M(xbuf)]
        mov     [xsi + xcx - 1], al
        ret
.o1:
        push    xax
        call    push_operand
        pop     xax
        cmp     byte [M(err)], 0
        jne     .ret
        mov     [M(pend)], al
        call    xappend
        mov     byte [M(last)], L_OP
        ret

.lparen:
        mov     al, [M(last)]           ; only where an operand may start
        cmp     al, L_OPERAND
        je      .ret
        cmp     al, L_CLOSED
        je      .ret
        cmp     byte [M(depth)], MAX_DEPTH
        jae     .ret
        mov     al, [M(pend)]
        test    al, al
        jz      .lp1
        call    apply_op
        mov     byte [M(pend)], 0
.lp1:
        mov     al, '('
        call    opush
        mov     al, '('
        call    xappend
        inc     byte [M(depth)]
        mov     byte [M(last)], L_OPEN
        ret

.rparen:
        cmp     byte [M(depth)], 0
        je      .ret
        call    commit
        mov     al, [M(last)]
        cmp     al, L_OP
        je      .ret
        cmp     al, L_OPEN
        je      .ret
        call    push_operand
.rp1:
        movzx   ecx, byte [M(ocount)]      ; evaluate back to the matching "("
        test    ecx, ecx
        jz      .rp3
        lea     xsi, [M(ostk)]
        cmp     byte [xsi + xcx - 1], '('
        je      .rp2
        call    reduce
        jmp     .rp1
.rp2:
        dec     byte [M(ocount)]
.rp3:
        call    vpop
        fstp    tword [M(cur)]
        mov     al, ')'
        call    xappend
        dec     byte [M(depth)]
        mov     byte [M(last)], L_CLOSED
        ret

.equals:
        call    commit
        mov     al, [M(last)]
        cmp     al, L_OPEN
        je      .ret
        cmp     al, L_OP
        jne     .e1
        mov     byte [M(pend)], 0       ; "2 + =": drop the dangling operator
        dec     byte [M(xlen)]
        jmp     .e3
.e1:
        cmp     byte [M(vcount)], 0        ; a lone number: nothing to work out
        jne     .e2
        cmp     byte [M(ocount)], 0
        jne     .e2
        cmp     al, L_CLOSED
        je      .e2
        mov     byte [M(last)], L_START
        ret
.e2:
        call    push_operand
.e3:
        movzx   ecx, byte [M(ocount)]      ; evaluate everything; brackets still
        test    ecx, ecx                ; open are closed automatically
        jz      .e5
        lea     xsi, [M(ostk)]
        cmp     byte [xsi + xcx - 1], '('
        jne     .e4
        dec     byte [M(ocount)]
        mov     al, ')'
        call    xappend
        jmp     .e3
.e4:
        call    reduce
        jmp     .e3
.e5:
        call    vpop
        fstp    tword [M(cur)]
        call    tape_add_result         ; each new line goes on top, so this
        call    tape_add_expr           ; leaves the expression above its result
        xor     eax, eax
        mov     [M(vcount)], al
        mov     [M(xlen)], al
        mov     [M(depth)], al
        mov     byte [M(last)], L_START
        ret

; --- +/- ---
.negate:
        call    commit
        mov     al, [M(last)]
        cmp     al, L_OP
        je      .ret
        cmp     al, L_OPEN
        je      .ret
        cmp     al, L_CLOSED
        je      .ret
        fld     tword [M(cur)]
        fchs
        fstp    tword [M(cur)]
        ret

; --- M+ : add the displayed value to memory ---
.madd:
        call    commit
        fld     tword [M(mem)]
        fld     tword [M(cur)]
        faddp   st1, st0
        call    check_range
        jne     .mfail
        fstp    tword [M(mem)]
        ret
.mfail:
        fstp    st0
        mov     byte [M(err)], 1
        ret

; --- MR : recall memory as the current operand ---
.mrecall:
        cmp     byte [M(last)], L_CLOSED
        je      .ret
        fld     tword [M(mem)]
        fstp    tword [M(cur)]
        mov     byte [M(entering)], 0
        mov     byte [M(last)], L_OPERAND
        ret

; Start typing a new number.  Preserves al.
start_entry:
        mov     byte [M(elen)], 0
        mov     byte [M(entering)], 1
        mov     byte [M(last)], L_OPERAND
        ret

; If a number is being typed, convert it into cur.
commit:
        cmp     byte [M(entering)], 0
        je      .r
        call    parse_entry
        mov     byte [M(entering)], 0
.r:
        ret

; Power-on state: memory and tape empty.
calc_init:
        fldz
        fstp    tword [M(mem)]
        lea     xdi, [M(tape)]
        xor     edx, edx
.t:
        mov     byte [xdi + xdx], ' '
        inc     edx
        cmp     edx, TAPE_LINES * TAPE_W
        jb      .t
        ; fall through

; The C key: forget the current calculation (memory and tape are kept).
calc_reset:
        fldz
        fstp    tword [M(cur)]
        xor     eax, eax
        mov     [M(pend)], al
        mov     [M(entering)], al
        mov     [M(err)], al
        mov     [M(elen)], al
        mov     [M(xlen)], al
        mov     [M(vcount)], al
        mov     [M(ocount)], al
        mov     [M(depth)], al
        mov     [M(last)], al
        ret

; ---------------------------------------------------------------------------
; Expression evaluation
; ---------------------------------------------------------------------------

; cur becomes the next operand: add it to the expression text, put the
; waiting operator (if any) on the operator stack, push the value.
push_operand:
        cmp     byte [M(last)], L_CLOSED ; bracket groups are already in the text
        je      .notext
        call    xappend_cur
.notext:
        mov     al, [M(pend)]
        test    al, al
        jz      .push
        call    apply_op
        mov     byte [M(pend)], 0
.push:
        fld     tword [M(cur)]
        call    vpush
        ret

; Put operator al on the operator stack, first evaluating every operator on
; top of it that binds at least as tightly.
apply_op:
        mov     [M(newop)], al
.l:
        movzx   ecx, byte [M(ocount)]
        test    ecx, ecx
        jz      .push
        lea     xsi, [M(ostk)]
        mov     al, [xsi + xcx - 1]
        cmp     al, '('
        je      .push
        call    prec
        mov     dh, dl                  ; dh = precedence of the stack top
        mov     al, [M(newop)]
        call    prec                    ; dl = precedence of the new operator
        cmp     dh, dl
        ja      .reduce
        jb      .push
        cmp     al, '^'                 ; equal: ^ groups right to left
        je      .push
.reduce:
        call    reduce
        jmp     .l
.push:
        mov     al, [M(newop)]
        call    opush
        ret

; al = operator -> dl = precedence.
prec:
        mov     dl, 3
        cmp     al, '^'
        je      .r
        mov     dl, 2
        cmp     al, '*'
        je      .r
        cmp     al, '/'
        je      .r
        mov     dl, 1
.r:
        ret

; Pop one operator and two values, push the result.
reduce:
        movzx   ecx, byte [M(ocount)]
        test    ecx, ecx
        jz      .r
        dec     ecx
        mov     [M(ocount)], cl
        lea     xsi, [M(ostk)]
        mov     al, [xsi + xcx]
        push    xax
        call    vpop                    ; right operand
        call    vpop                    ; left operand
        fxch                            ; st0 = right, st1 = left
        pop     xax
        call    arith
        call    vpush
.r:
        ret

; Push st0 on the value stack (pops it from the FPU).
vpush:
        movzx   ecx, byte [M(vcount)]
        cmp     ecx, VS_MAX
        jae     .full
        imul    eax, ecx, 10
        lea     xsi, [M(vstk)]
        fstp    tword [xsi + xax]
        inc     byte [M(vcount)]
        ret
.full:
        fstp    st0
        mov     byte [M(err)], 1
        ret

; Pop the value stack into st0.
vpop:
        movzx   ecx, byte [M(vcount)]
        test    ecx, ecx
        jz      .empty
        dec     ecx
        mov     [M(vcount)], cl
        imul    eax, ecx, 10
        lea     xsi, [M(vstk)]
        fld     tword [xsi + xax]
        ret
.empty:
        fldz
        ret

; Push al on the operator stack.
opush:
        movzx   ecx, byte [M(ocount)]
        cmp     ecx, OS_MAX
        jae     .full
        lea     xsi, [M(ostk)]
        mov     [xsi + xcx], al
        inc     byte [M(ocount)]
        ret
.full:
        mov     byte [M(err)], 1
        ret

; st0 = st1 <al> st0 (both popped, result pushed).  On divide by zero, an
; invalid power or overflow: sets err and returns 0.
arith:
        cmp     al, '+'
        je      .add
        cmp     al, '-'
        je      .sub
        cmp     al, '*'
        je      .mul
        cmp     al, '^'
        je      .pow
        ftst
        fnstsw  ax
        test    ah, 0x40                ; C3 set: divisor is zero
        jnz     .fail2
        fdivp   st1, st0
        jmp     .check
.add:
        faddp   st1, st0
        jmp     .check
.sub:
        fsubp   st1, st0
        jmp     .check
.mul:
        fmulp   st1, st0
        jmp     .check
.pow:
        call    fpow
.check:
        call    check_range
        jne     .fail
        ret
.fail2:
        fstp    st0
.fail:
        fstp    st0
        fldz
        mov     byte [M(err)], 1
        ret

; ZF set if |st0| is small enough to display.  st0 is kept.
check_range:
        fld     st0
        fabs
        fcomp   qword [M(c_limit)]
        fnstsw  ax
        and     ah, 0x45
        cmp     ah, 0x01                ; only C0: |st0| < limit
        ret

; st0 = st1 ^ st0 (base in st1, exponent in st0; both popped).
; Invalid cases return a huge number, which the range check turns into Error.
fpow:
        fld     st0
        frndint
        fcomp   st1                     ; is the exponent a whole number?
        fnstsw  ax
        test    ah, 0x40
        jz      .fraction
        fld     st0
        fabs
        ficomp  dword [M(c_64)]
        fnstsw  ax
        test    ah, 0x41                ; |exponent| <= 64 ?
        jz      .bigint

        ; small whole exponent: repeated multiplication, exact for integers
        ; and correct for negative bases
        fistp   dword [M(tmpd)]         ; st0 = base
        fld1                            ; st0 = result, st1 = base
        mov     ecx, [M(tmpd)]
        mov     edx, ecx
        test    ecx, ecx
        jns     .loop
        neg     ecx
.loop:
        test    ecx, ecx
        jz      .looped
        fmul    st0, st1
        dec     ecx
        jmp     .loop
.looped:
        fstp    st1
        test    edx, edx
        jns     .ret
        fld1                            ; negative exponent: 1 / result
        fdivrp  st1, st0
.ret:
        ret

.fraction:
        xor     ebx, ebx                ; bl = 0: exponent is not whole
        jmp     .general
.bigint:
        fld     st0
        fistp   qword [M(tmpq)]
        mov     bl, 1                   ; bl = 1: whole, bh = 1 if odd
        mov     bh, [M(tmpq)]
        and     bh, 1
.general:
        fxch                            ; st0 = base, st1 = exponent
        ftst
        fnstsw  ax
        test    ah, 0x40
        jnz     .zero
        test    ah, 0x01
        jz      .positive
        test    bl, bl                  ; negative base needs a whole exponent
        jz      .bad
        fabs
        jmp     .calc
.positive:
        xor     bh, bh
.calc:
        fyl2x                           ; st0 = exponent * log2(base)
        fld     st0
        frndint
        fsub    st1, st0                ; st1 = fraction, st0 = whole part
        fxch
        f2xm1
        fld1
        faddp   st1, st0                ; 2 ^ fraction
        fscale                          ; * 2 ^ whole
        fstp    st1
        test    bh, bh
        jz      .ret
        fchs
        ret
.zero:
        fstp    st0                     ; 0 ^ x: 0 if x > 0, invalid if x < 0
        ftst
        fnstsw  ax
        fstp    st0
        test    ah, 0x01
        jnz     .bad0
        fldz
        ret
.bad:
        fstp    st0
        fstp    st0
.bad0:
        fld     qword [M(c_huge)]
        ret

; Convert the text in ebuf into cur.
parse_entry:
        fldz
        xor     ecx, ecx                ; index
        xor     edx, edx                ; digits after the point
        xor     ebx, ebx                ; 1 once the point has been seen
        lea     xsi, [M(ebuf)]
        movzx   edi, byte [M(elen)]
.l:
        cmp     ecx, edi
        jae     .end
        movzx   eax, byte [xsi + xcx]
        inc     ecx
        cmp     al, '.'
        jne     .dig
        mov     bl, 1
        jmp     .l
.dig:
        sub     al, '0'
        mov     [M(tmpd)], eax
        fimul   dword [M(c_ten)]
        fiadd   dword [M(tmpd)]
        add     edx, ebx
        jmp     .l
.end:
        test    edx, edx
        jz      .store
        fld1                            ; divide once by 10^edx (exact power)
.p:
        fimul   dword [M(c_ten)]
        dec     edx
        jnz     .p
        fdivp   st1, st0
.store:
        fstp    tword [M(cur)]
        ret

; Format the 80-bit value at xsi as decimal text.
; Returns xsi = numbuf, ecx = length.  Clobbers eax, ebx, edx, xdi.
fmt_num:
        fld     tword [xsi]
        ftst
        fnstsw  ax
        mov     bl, ah                  ; bit 0 = negative
        fabs
        fimul   dword [M(c_million)]    ; 6 decimals, fbstp rounds to nearest
        fbstp   tword [M(bcd)]
        lea     xsi, [M(bcd)]           ; unpack 18 BCD digits, highest first
        lea     xdi, [M(digits)]
        mov     ecx, 9
.unpack:
        dec     ecx
        mov     al, [xsi + xcx]
        mov     ah, al
        shr     al, 4
        and     ah, 0x0F
        add     ax, 0x3030
        mov     [xdi], al
        mov     [xdi + 1], ah
        add     xdi, 2
        test    ecx, ecx
        jnz     .unpack

        lea     xsi, [M(digits)]
        lea     xdi, [M(numbuf)]
        test    bl, 0x01
        jz      .nosign
        xor     ecx, ecx                ; no minus sign in front of zero
.zero:
        cmp     byte [xsi + xcx], '0'
        jne     .minus
        inc     ecx
        cmp     ecx, 18
        jb      .zero
        jmp     .nosign
.minus:
        mov     byte [xdi], '-'
        inc     xdi
.nosign:
        xor     ecx, ecx                ; skip leading zeros, keep one digit
.skip:
        cmp     ecx, 11
        jae     .int
        cmp     byte [xsi + xcx], '0'
        jne     .int
        inc     ecx
        jmp     .skip
.int:
        mov     al, [xsi + xcx]
        mov     [xdi], al
        inc     xdi
        inc     ecx
        cmp     ecx, 12
        jb      .int
        mov     edx, 18                 ; trim trailing zeros of the fraction
.trim:
        cmp     edx, 12
        jbe     .done
        cmp     byte [xsi + xdx - 1], '0'
        jne     .frac
        dec     edx
        jmp     .trim
.frac:
        mov     byte [xdi], '.'
        inc     xdi
.f:
        mov     al, [xsi + xcx]
        mov     [xdi], al
        inc     xdi
        inc     ecx
        cmp     ecx, edx
        jb      .f
.done:
        lea     xsi, [M(numbuf)]
        mov     xcx, xdi
        sub     xcx, xsi
        ret

; ---------------------------------------------------------------------------
; Expression text and tape
; ---------------------------------------------------------------------------

; Append character al to the expression text.  When it is full the oldest
; character is dropped.  Clobbers ah, ecx, edx, xsi.
xappend:
        movzx   ecx, byte [M(xlen)]
        lea     xsi, [M(xbuf)]
        cmp     ecx, XMAX
        jb      .put
        xor     edx, edx
.shift:
        mov     ah, [xsi + xdx + 1]
        mov     [xsi + xdx], ah
        inc     edx
        cmp     edx, XMAX - 1
        jb      .shift
        dec     ecx
.put:
        mov     [xsi + xcx], al
        inc     ecx
        mov     [M(xlen)], cl
        ret

; Append cur, as text, to the expression.
; A negative number is written in brackets, e.g. (-2)^3.
xappend_cur:
        lea     xsi, [M(cur)]
        call    fmt_num
        xor     ebx, ebx
        mov     edi, ecx
        cmp     byte [xsi], '-'
        jne     .l
        mov     al, '('
        call    xappend
.l:
        cmp     ebx, edi
        jae     .d
        lea     xsi, [M(numbuf)]
        mov     al, [xsi + xbx]
        call    xappend
        inc     ebx
        jmp     .l
.d:
        lea     xsi, [M(numbuf)]
        cmp     byte [xsi], '-'
        jne     .r
        mov     al, ')'
        call    xappend
.r:
        ret

; Move every history line down by one and blank the first one.
tape_scroll:
        lea     xdi, [M(tape)]
        mov     edx, TAPE_LINES * TAPE_W
.l:
        dec     edx
        mov     al, [xdi + xdx - TAPE_W]
        mov     [xdi + xdx], al
        cmp     edx, TAPE_W
        ja      .l
.c:
        dec     edx
        mov     byte [xdi + xdx], ' '
        jnz     .c
        lea     xdi, [M(tape_kind)]
        mov     edx, TAPE_LINES - 1
.k:
        mov     al, [xdi + xdx - 1]
        mov     [xdi + xdx], al
        dec     edx
        jnz     .k
        mov     byte [M(tape_dirty)], 1
        ret

; Put the expression text on a new first history line (its tail, if long).
tape_add_expr:
        call    tape_scroll
        mov     byte [M(tape_kind)], 0
        lea     xsi, [M(xbuf)]
        movzx   ecx, byte [M(xlen)]
        cmp     ecx, TAPE_W
        jbe     .fits
        mov     eax, ecx
        sub     eax, TAPE_W
        add     xsi, xax
        mov     ecx, TAPE_W
.fits:
        lea     xdi, [M(tape)]
        xor     edx, edx
.copy:
        cmp     edx, ecx
        jae     .d
        mov     al, [xsi + xdx]
        mov     [xdi + xdx], al
        inc     edx
        jmp     .copy
.d:
        ret

; Put "= <cur>" (or "= Error"), right aligned, on a new first history line.
tape_add_result:
        call    tape_scroll
        mov     byte [M(tape_kind)], 1
        cmp     byte [M(err)], 0
        je      .num
        lea     xsi, [M(s_error)]
        mov     ecx, s_error_len
        jmp     .put
.num:
        lea     xsi, [M(cur)]
        call    fmt_num
.put:
        lea     xdi, [M(tape)]
        mov     edx, TAPE_W
        sub     edx, ecx
        add     xdi, xdx
        mov     byte [xdi - 2], '='
        xor     ebx, ebx
.copy:
        cmp     ebx, ecx
        jae     .d
        mov     al, [xsi + xbx]
        mov     [xdi + xbx], al
        inc     ebx
        jmp     .copy
.d:
        ret

; ---------------------------------------------------------------------------
; Drawing
; ---------------------------------------------------------------------------

; Print the zero terminated string at xsi.
put_str:
        mov     al, [xsi]
        inc     xsi
        test    al, al
        jz      .d
        call    plat_putc
        jmp     put_str
.d:
        ret

; Print ecx characters from xsi.
put_n:
        test    ecx, ecx
        jz      .d
.l:
        mov     al, [xsi]
        inc     xsi
        call    plat_putc
        dec     ecx
        jnz     .l
.d:
        ret

; Print character al ecx times.
rep_ch:
        test    ecx, ecx
        jz      .d
.l:
        call    plat_putc
        dec     ecx
        jnz     .l
.d:
        ret

; Print ecx characters from xsi right-aligned in a field ebx wide (one
; blank is kept on the right), at the cursor, in the current colour.
field:
        push    xcx
        mov     eax, ebx
        dec     eax
        sub     eax, ecx
        mov     ecx, eax
        mov     al, ' '
        call    rep_ch
        pop     xcx
        call    put_n
        mov     al, ' '
        call    plat_putc
        ret

; Draw a box in the current colour and blank its inside.
; dl,dh = top left, bl = width, bh = height, xsi = 6 frame characters
; (top left, horizontal, top right, vertical, bottom left, bottom right).
; Clobbers al, ecx, dh, bh.
draw_frame:
        call    plat_gotoxy
        mov     al, [xsi]
        call    plat_putc
        mov     al, [xsi + 1]
        movzx   ecx, bl
        sub     ecx, 2
        call    rep_ch
        mov     al, [xsi + 2]
        call    plat_putc
        sub     bh, 2
.side:
        inc     dh
        call    plat_gotoxy
        mov     al, [xsi + 3]
        call    plat_putc
        mov     al, ' '
        movzx   ecx, bl
        sub     ecx, 2
        call    rep_ch
        mov     al, [xsi + 3]
        call    plat_putc
        dec     bh
        jnz     .side
        inc     dh
        call    plat_gotoxy
        mov     al, [xsi + 4]
        call    plat_putc
        mov     al, [xsi + 1]
        movzx   ecx, bl
        sub     ecx, 2
        call    rep_ch
        mov     al, [xsi + 5]
        call    plat_putc
        ret

; Draw key eax (0..BTN_COUNT-1): an outline with a coloured label, both on
; the panel background.  The selected key gets a white double outline and
; pointers either side of its label.  Preserves all registers.
draw_button:
        push    xax
        push    xbx
        push    xcx
        push    xdx
        push    xsi
        mov     ebx, eax
        mov     al, ATTR_PANEL
        lea     xsi, [M(box_single)]
        cmp     bl, [M(sel)]
        jne     .frame
        mov     al, ATTR_SEL
        lea     xsi, [M(box_double)]
.frame:
        call    plat_setattr
        mov     eax, ebx
        mov     cl, BTN_COLS
        div     cl                      ; al = row, ah = column
        mov     dl, ah
        shl     dl, 3
        add     dl, BTN_X
        mov     dh, al
        add     dh, dh
        add     dh, al
        add     dh, BTN_Y
        push    xbx
        push    xdx
        mov     bl, 7
        mov     bh, 3
        call    draw_frame
        pop     xdx
        pop     xbx
        inc     dl
        inc     dh
        call    plat_gotoxy
        cmp     bl, [M(sel)]
        je      .label                  ; selected: keep the white colour
        lea     xsi, [M(btn_attr)]
        mov     al, [xsi + xbx]
        call    plat_setattr
.label:
        imul    eax, ebx, 5
        lea     xsi, [M(btn_labels)]
        add     xsi, xax
        mov     ecx, 5
        call    put_n
        cmp     bl, [M(sel)]
        jne     .done
        call    plat_gotoxy             ; pointers over the label's blank ends
        mov     al, 0x10
        call    plat_putc
        add     dl, 4
        call    plat_gotoxy
        mov     al, 0x11
        call    plat_putc
.done:
        pop     xsi
        pop     xdx
        pop     xcx
        pop     xbx
        pop     xax
        ret

draw_display:
        mov     al, ATTR_EXPR           ; top line: memory flag + expression
        call    plat_setattr
        mov     dl, DISP_X
        mov     dh, DISP_Y
        call    plat_gotoxy
        mov     al, ' '                 ; one blank of padding on the left
        call    plat_putc
        fld     tword [M(mem)]
        ftst
        fnstsw  ax
        fstp    st0
        mov     al, 'M'
        test    ah, 0x40                ; C3 set: memory is zero
        jz      .mem
        mov     al, ' '
.mem:
        call    plat_putc
        lea     xsi, [M(xbuf)]
        movzx   ecx, byte [M(xlen)]
        cmp     byte [M(err)], 0
        je      .t1
        xor     ecx, ecx
.t1:
        cmp     ecx, DISP_W - 4         ; too long: show the tail
        jbe     .t2
        mov     eax, ecx
        sub     eax, DISP_W - 4
        add     xsi, xax
        mov     ecx, DISP_W - 4
.t2:
        mov     ebx, DISP_W - 2
        call    field

        mov     al, ATTR_VAL            ; bottom line: error, entry or value
        call    plat_setattr
        mov     dl, DISP_X
        mov     dh, DISP_Y + 1
        call    plat_gotoxy
        cmp     byte [M(err)], 0
        je      .b1
        lea     xsi, [M(s_error)]
        mov     ecx, s_error_len
        jmp     .bottom
.b1:
        cmp     byte [M(entering)], 0
        je      .b2
        lea     xsi, [M(ebuf)]
        movzx   ecx, byte [M(elen)]
        jmp     .bottom
.b2:
        lea     xsi, [M(cur)]
        call    fmt_num
.bottom:
        mov     ebx, DISP_W
        call    field
        ret

draw_tape:
        xor     ebx, ebx
.l:
        lea     xsi, [M(tape_kind)]
        mov     al, ATTR_TAPE
        cmp     byte [xsi + xbx], 0
        je      .attr
        mov     al, ATTR_RESULT
.attr:
        call    plat_setattr
        mov     dl, DISP_X
        mov     dh, bl
        add     dh, TAPE_Y
        call    plat_gotoxy
        imul    eax, ebx, TAPE_W
        lea     xsi, [M(tape)]
        add     xsi, xax
        mov     ecx, TAPE_W
        call    put_n
        inc     ebx
        cmp     ebx, TAPE_LINES
        jb      .l
        ret

draw_screen:
        mov     al, ATTR_TITLE          ; title bar
        call    plat_setattr
        xor     edx, edx
        call    plat_gotoxy
        mov     al, ' '
        mov     ecx, 80
        call    rep_ch
        mov     dl, (80 - s_title_len) / 2
        mov     dh, 0
        call    plat_gotoxy
        lea     xsi, [M(s_title)]
        call    put_str

        mov     al, ATTR_BG             ; background
        call    plat_setattr
        mov     dh, 1
.bg:
        mov     dl, 0
        call    plat_gotoxy
        mov     al, ' '
        mov     ecx, 80
        call    rep_ch
        inc     dh
        cmp     dh, 24
        jb      .bg

        mov     al, ATTR_FOOT           ; help bar
        call    plat_setattr
        mov     dl, 0
        mov     dh, 24
        call    plat_gotoxy
        mov     al, ' '
        mov     ecx, FOOT_W
        call    rep_ch
        mov     dl, 1
        call    plat_gotoxy
        lea     xsi, [M(s_help)]
        call    put_str
        lea     xsi, [M(plat_exit_text)]
        call    put_str

        mov     al, ATTR_PANEL          ; the frame
        call    plat_setattr
        mov     dl, FX
        mov     dh, PY
        mov     bl, FW
        mov     bh, PH
        lea     xsi, [M(box_double)]
        call    draw_frame

        mov     dl, DIV_X               ; dividing line, joined to the frame
        mov     dh, PY
        call    plat_gotoxy
        mov     al, 0xD1
        call    plat_putc
.div:
        inc     dh
        call    plat_gotoxy
        mov     al, 0xB3
        call    plat_putc
        cmp     dh, PY + PH - 2
        jb      .div
        inc     dh
        call    plat_gotoxy
        mov     al, 0xCF
        call    plat_putc

        mov     dl, FX + (DIV_X - FX + 1 - s_panel_len) / 2
        mov     dh, PY
        call    plat_gotoxy
        lea     xsi, [M(s_panel)]
        call    put_str
        mov     dl, DIV_X + (FX + FW - DIV_X - s_keys_len) / 2
        call    plat_gotoxy             ; title over the keys
        lea     xsi, [M(s_keys)]
        call    put_str

        mov     dl, FX                  ; line between display and history
        mov     dh, TAPE_Y - 1
        call    plat_gotoxy
        mov     al, 0xC7
        call    plat_putc
        mov     al, 0xC4
        mov     ecx, DIV_X - FX - 1
        call    rep_ch
        mov     al, 0xB4
        call    plat_putc
        mov     dl, FX + (DIV_X - FX + 1 - s_tape_len) / 2
        call    plat_gotoxy
        lea     xsi, [M(s_tape)]
        call    put_str

        mov     al, ATTR_VAL            ; blank top and bottom display rows
        call    plat_setattr
        mov     dl, DISP_X
        mov     dh, DISP_Y - 1
        call    plat_gotoxy
        mov     al, ' '
        mov     ecx, DISP_W
        call    rep_ch
        mov     dh, DISP_Y + 2
        call    plat_gotoxy
        mov     ecx, DISP_W
        call    rep_ch

        xor     eax, eax                ; keys
.btn:
        call    draw_button
        inc     eax
        cmp     eax, BTN_COUNT
        jb      .btn
        ret
