; ---------------------------------------------------------------------------
; data.inc - constants and variables used by core.inc
; ---------------------------------------------------------------------------

s_title:        db "Written in x86 Assembly", 0
s_title_len     equ $ - s_title - 1
s_panel:        db " Calculator ", 0
s_panel_len     equ $ - s_panel - 1
s_tape:         db " History ", 0
s_tape_len      equ $ - s_tape - 1
s_error:        db "Error"
s_error_len     equ $ - s_error

; 0x18..0x1B are the CP437 arrow glyphs (up, down, right, left)
s_keys:         db " Use ", 0x18, 0x19, 0x1B, 0x1A, " To Move ", 0
s_keys_len      equ $ - s_keys - 1
s_help:         db "Enter: Press", "              "
                db "M: M+    R: MR    C: Clear"
                db "              ", "Esc: ", 0

; Frame characters (CP437): top left, horizontal, top right, vertical,
; bottom left, bottom right.
box_double:     db 0xC9, 0xCD, 0xBB, 0xBA, 0xC8, 0xBC
box_single:     db 0xDA, 0xC4, 0xBF, 0xB3, 0xC0, 0xD9

; Key grid, BTN_COLS x BTN_ROWS, each label exactly 5 characters.
btn_labels:     db " MR  ", " M+  ", " DEL ", "  C  "
                db "  (  ", "  )  ", "  ^  ", "  /  "
                db "  7  ", "  8  ", "  9  ", "  *  "
                db "  4  ", "  5  ", "  6  ", "  -  "
                db "  1  ", "  2  ", "  3  ", "  +  "
                db " +/- ", "  0  ", "  .  ", "  =  "
; The character each key stands for (fed to handle_key).
btn_keys:       db "RM", 8, "C"
                db "()^/"
                db "789*"
                db "456-"
                db "123+"
                db "N0.="
btn_attr:       db ATTR_MEM,   ATTR_MEM,   ATTR_CLR,   ATTR_CLR
                db ATTR_OPER,  ATTR_OPER,  ATTR_OPER,  ATTR_OPER
                db ATTR_DIGIT, ATTR_DIGIT, ATTR_DIGIT, ATTR_OPER
                db ATTR_DIGIT, ATTR_DIGIT, ATTR_DIGIT, ATTR_OPER
                db ATTR_DIGIT, ATTR_DIGIT, ATTR_DIGIT, ATTR_OPER
                db ATTR_DIGIT, ATTR_DIGIT, ATTR_DIGIT, ATTR_EQ

c_ten:          dd 10
c_64:           dd 64
c_million:      dd 1000000
c_limit:        dq 999999999999.0       ; largest magnitude the display holds
c_huge:         dq 1.0e300              ; stands for an invalid result

; --- variables ---
cur:            times 10 db 0           ; value shown / current operand
mem:            times 10 db 0           ; memory register
bcd:            times 10 db 0
tmpq:           dq 0
tmpd:           dd 0
pend:           db 0                    ; operator waiting for its operand
newop:          db 0
last:           db 0                    ; L_* state
depth:          db 0                    ; open brackets
entering:       db 0                    ; 1 while a number is being typed
err:            db 0
sel:            db 0                    ; highlighted key
elen:           db 0
ebuf:           times 24 db 0           ; number being typed
digits:         times 18 db 0
numbuf:         times 28 db 0
vcount:         db 0                    ; value stack (80-bit floats)
ocount:         db 0                    ; operator stack (characters)
vstk:           times VS_MAX * 10 db 0
ostk:           times OS_MAX db 0
xlen:           db 0                    ; expression text
xbuf:           times XMAX + 1 db 0
tape_dirty:     db 0
tape_kind:      times TAPE_LINES db 0   ; per line: 0 expression, 1 result
tape:           times TAPE_LINES * TAPE_W db 0
