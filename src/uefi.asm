; ---------------------------------------------------------------------------
; uefi.asm - UEFI target.  One source, two builds:
;
;   nasm -f bin -DX64 -o BOOTX64.EFI  uefi.asm     64-bit UEFI (most PCs)
;   nasm -f bin       -o BOOTIA32.EFI uefi.asm     32-bit UEFI (rare)
;
; The PE/COFF executable header is written out by hand, so no linker is
; needed.  All code is position independent (RIP-relative on x64, relative
; to the load address kept in ebp on IA32), so the image has no relocations.
; Text is drawn with the firmware's Simple Text Output protocol and keys are
; read with Simple Text Input - the same services firmware setup menus use.
; ---------------------------------------------------------------------------

%ifdef X64
        bits    64
        default rel
  %define xax rax
  %define xbx rbx
  %define xcx rcx
  %define xdx rdx
  %define xsi rsi
  %define xdi rdi
  %define M(s) rel s
  %define PTRSZ 8
  %define DPTR dq
  PE_MACHINE    equ 0x8664
  PE_MAGIC      equ 0x20B
  PE_FLAGS      equ 0x0022      ; executable, large address aware
  ST_CONIN      equ 48          ; EFI_SYSTEM_TABLE field offsets
  ST_CONOUT     equ 64
  ST_BOOTSVC    equ 96
%else
        bits    32
  %define xax eax
  %define xbx ebx
  %define xcx ecx
  %define xdx edx
  %define xsi esi
  %define xdi edi
  %define M(s) ebp + s
  %define PTRSZ 4
  %define DPTR dd
  PE_MACHINE    equ 0x014C
  PE_MAGIC      equ 0x10B
  PE_FLAGS      equ 0x0102      ; executable, 32-bit machine
  ST_CONIN      equ 36
  ST_CONOUT     equ 44
  ST_BOOTSVC    equ 60
%endif

; Writing the bottom right cell would scroll a UEFI console that is exactly
; 80x25, so the help bar stops one column short.
%define FOOT_W 79

%define IF(n) ((n) * PTRSZ)             ; n-th function of a protocol
%define BS(n) (24 + (n) * PTRSZ)        ; n-th EFI_BOOT_SERVICES function

; Simple Text Output protocol
TO_OUTPUT_STRING equ IF(1)
TO_QUERY_MODE    equ IF(3)
TO_SET_ATTRIBUTE equ IF(5)
TO_CLEAR_SCREEN  equ IF(6)
TO_SET_CURSOR    equ IF(7)
TO_ENABLE_CURSOR equ IF(8)
TO_MODE          equ IF(9)
; Simple Text Input protocol
TI_READ_KEY      equ IF(1)
TI_WAIT_FOR_KEY  equ IF(2)
; Boot services
BS_WAIT_FOR_EVENT equ BS(9)
BS_EXIT           equ BS(24)
BS_SET_WATCHDOG   equ BS(29)

; EFI object, function offset, arguments...
; Calls a firmware function with the platform calling convention.  The stack
; is re-aligned to 16 bytes first, so it may be used at any push depth.
; Arguments must not be xcx/xdx.  Clobbers xax, xcx, xdx (and r8-r11).
%ifdef X64
%macro EFI 2-6
        mov     [rel saved_sp], rsp
        and     rsp, -16
        sub     rsp, 32                 ; shadow space
  %if %0 > 2
        mov     rcx, %3
  %endif
  %if %0 > 3
        mov     rdx, %4
  %endif
  %if %0 > 4
        mov     r8, %5
  %endif
  %if %0 > 5
        mov     r9, %6
  %endif
        call    [%1 + %2]
        mov     rsp, [rel saved_sp]
%endmacro
%else
%macro EFI 2-7
  %define %%obj  %1
  %define %%func %2
  %assign %%n %0 - 2
        mov     [ebp + saved_sp], esp
        and     esp, -16
  %if (%%n % 4) != 0
        sub     esp, (4 - (%%n % 4)) * 4
  %endif
  %rep %%n
    %rotate -1
        push    %1
  %endrep
        call    [%%obj + %%func]
        mov     esp, [ebp + saved_sp]
%endmacro
%endif

; ------------------------------ PE header ----------------------------------
ALIGNMENT       equ 0x1000              ; file and section alignment

        org     0
header:
        db      'MZ'
        times 0x3C - ($ - $$) db 0
        dd      pe_header
pe_header:
        db      'PE', 0, 0
        dw      PE_MACHINE
        dw      3                       ; sections
        dd      0, 0, 0
        dw      opt_end - opt_header
        dw      PE_FLAGS
opt_header:
        dw      PE_MAGIC
        db      1, 0
        dd      data_start - text_start ; size of code
        dd      image_end - data_start  ; size of initialised data
        dd      0
        dd      efi_main                ; entry point
        dd      text_start              ; base of code
%ifdef X64
        dq      0x400000                ; preferred image base
%else
        dd      data_start              ; base of data
        dd      0x400000
%endif
        dd      ALIGNMENT, ALIGNMENT
        dw      0, 0, 0, 0, 0, 0        ; OS / image / subsystem versions
        dd      0
        dd      image_end               ; size of image
        dd      text_start              ; size of headers
        dd      0                       ; checksum
        dw      10                      ; subsystem: EFI application
        dw      0
        DPTR    0, 0, 0, 0              ; stack / heap sizes
        dd      0
        dd      16                      ; data directories
        times 5 dq 0
        dd      reloc_start, reloc_end - reloc_start
        times 10 dq 0
opt_end:
        db      '.text', 0, 0, 0
        dd      data_start - text_start, text_start
        dd      data_start - text_start, text_start
        dd      0, 0
        dw      0, 0
        dd      0x60000020              ; code, execute, read
        db      '.data', 0, 0, 0
        dd      reloc_start - data_start, data_start
        dd      reloc_start - data_start, data_start
        dd      0, 0
        dw      0, 0
        dd      0xC0000040              ; data, read, write
        db      '.reloc', 0, 0
        dd      image_end - reloc_start, reloc_start
        dd      image_end - reloc_start, reloc_start
        dd      0, 0
        dw      0, 0
        dd      0x42000040              ; data, read, discardable

        times ALIGNMENT - ($ - $$) db 0

; -------------------------------- code -------------------------------------
text_start:

efi_main:
%ifndef X64
        call    .here                   ; ebp = address the image was loaded at
.here:
        pop     ebp
        sub     ebp, .here
        mov     ecx, [esp + 4]          ; image handle
        mov     edx, [esp + 8]          ; system table
%endif
        mov     [M(image_handle)], xcx
        mov     xax, [xdx + ST_CONIN]
        mov     [M(conin)], xax
        mov     xax, [xdx + ST_CONOUT]
        mov     [M(conout)], xax
        mov     xax, [xdx + ST_BOOTSVC]
        mov     [M(bootsvc)], xax

        fnstcw  word [M(saved_cw)]      ; use full 80-bit FPU precision
        fninit

        mov     xbx, [M(bootsvc)]       ; firmware resets the machine after
%ifdef X64                              ; 5 minutes unless the watchdog is off
        EFI     xbx, BS_SET_WATCHDOG, 0, 0, 0, 0
%else
        EFI     xbx, BS_SET_WATCHDOG, 0, 0, 0, 0, 0   ; 64-bit code = 2 slots
%endif

        mov     xbx, [M(conout)]
        EFI     xbx, TO_ENABLE_CURSOR, xbx, 0

        mov     xax, [xbx + TO_MODE]    ; centre the 80x25 screen if the
        mov     eax, [xax + 4]          ; console is larger
        lea     xsi, [M(cols)]
        lea     xdi, [M(rows)]
        EFI     xbx, TO_QUERY_MODE, xbx, xax, xsi, xdi
        test    xax, xax
        jnz     .nomode
        mov     eax, [M(cols)]
        sub     eax, 80
        jbe     .rows
        shr     eax, 1
        mov     [M(off_x)], al
.rows:
        mov     eax, [M(rows)]
        sub     eax, 25
        jbe     .nomode
        shr     eax, 1
        mov     [M(off_y)], al
.nomode:
        EFI     xbx, TO_SET_ATTRIBUTE, xbx, ATTR_BG
        mov     byte [M(cur_attr)], ATTR_BG
        EFI     xbx, TO_CLEAR_SCREEN, xbx
        jmp     calc_main

%macro PUSHV 0
        push    xax
        push    xbx
        push    xcx
        push    xdx
        push    xsi
        push    xdi
%endmacro
%macro POPV 0
        pop     xdi
        pop     xsi
        pop     xdx
        pop     xcx
        pop     xbx
        pop     xax
%endmacro

plat_setattr:
        PUSHV
        cmp     al, [M(cur_attr)]
        je      .done
        mov     [M(cur_attr)], al
        call    efi_flush
        movzx   eax, byte [M(cur_attr)]
        mov     xbx, [M(conout)]
        EFI     xbx, TO_SET_ATTRIBUTE, xbx, xax
.done:
        POPV
        ret

plat_gotoxy:
        PUSHV
        push    xdx
        call    efi_flush
        pop     xdx
        movzx   eax, dl
        movzx   ecx, byte [M(off_x)]
        add     eax, ecx
        movzx   esi, dh
        movzx   ecx, byte [M(off_y)]
        add     esi, ecx
        mov     xbx, [M(conout)]
        EFI     xbx, TO_SET_CURSOR, xbx, xax, xsi
        POPV
        ret

; Characters are collected in obuf and sent to the firmware in runs.
plat_putc:
        PUSHV
        call    cp437_to_ucs2
        lea     xsi, [M(obuf)]
        movzx   ecx, byte [M(olen)]
        mov     [xsi + xcx * 2], ax
        inc     ecx
        mov     [M(olen)], cl
        cmp     ecx, 80
        jb      .done
        call    efi_flush
.done:
        POPV
        ret

; Clobbers xax, xbx, xcx, xdx, xsi.
efi_flush:
        movzx   ecx, byte [M(olen)]
        test    ecx, ecx
        jz      .done
        lea     xsi, [M(obuf)]
        mov     word [xsi + xcx * 2], 0
        mov     byte [M(olen)], 0
        mov     xbx, [M(conout)]
        EFI     xbx, TO_OUTPUT_STRING, xbx, xsi
.done:
        ret

; al = CP437 character -> eax = UCS-2 character.  Clobbers xcx, xsi.
cp437_to_ucs2:
        movzx   eax, al
        cmp     al, 0x20
        jb      .lookup
        cmp     al, 0x7F
        jb      .done
.lookup:
        lea     xsi, [M(cp_table)]
.next:
        mov     cl, [xsi]
        test    cl, cl
        jz      .unknown
        cmp     cl, al
        je      .hit
        add     xsi, 3
        jmp     .next
.hit:
        movzx   eax, word [xsi + 1]
        ret
.unknown:
        mov     eax, '?'
.done:
        ret

plat_getkey:
        push    xbx
        push    xcx
        push    xdx
        push    xsi
        push    xdi
        call    efi_flush
.wait:
        mov     xbx, [M(conin)]         ; sleep until a key is pressed
        lea     xsi, [xbx + TI_WAIT_FOR_KEY]
        lea     xdi, [M(event_index)]
        mov     xbx, [M(bootsvc)]
        EFI     xbx, BS_WAIT_FOR_EVENT, 1, xsi, xdi
        mov     xbx, [M(conin)]
        lea     xsi, [M(key)]
        EFI     xbx, TI_READ_KEY, xbx, xsi
        test    xax, xax
        jnz     .wait
        movzx   eax, word [M(key) + 2]  ; Unicode character, 0 for special keys
        test    eax, eax
        jz      .special
        cmp     eax, 0x7F
        jb      .done
        mov     eax, 0xFFFF             ; not ASCII: ignored by the core
        jmp     .done
.special:
        movzx   ecx, word [M(key)]      ; EFI scan code
        mov     eax, KEY_UP
        cmp     ecx, 0x01
        je      .done
        mov     eax, KEY_DOWN
        cmp     ecx, 0x02
        je      .done
        mov     eax, KEY_RIGHT
        cmp     ecx, 0x03
        je      .done
        mov     eax, KEY_LEFT
        cmp     ecx, 0x04
        je      .done
        mov     eax, KEY_DEL
        cmp     ecx, 0x08
        je      .done
        mov     eax, KEY_ESC
        cmp     ecx, 0x17
        je      .done
        mov     eax, 0xFFFF
.done:
        pop     xdi
        pop     xsi
        pop     xdx
        pop     xcx
        pop     xbx
        ret

plat_exit:                              ; hand control back to the firmware
        call    efi_flush
        mov     xbx, [M(conout)]
        EFI     xbx, TO_SET_ATTRIBUTE, xbx, 0x07
        EFI     xbx, TO_CLEAR_SCREEN, xbx
        EFI     xbx, TO_ENABLE_CURSOR, xbx, 1
        fninit
        fldcw   word [M(saved_cw)]
        mov     xbx, [M(bootsvc)]
        mov     xax, [M(image_handle)]
        EFI     xbx, BS_EXIT, xax, 0, 0, 0
.hang:
        jmp     .hang

%include "core.inc"

        times (ALIGNMENT - (($ - $$) % ALIGNMENT)) % ALIGNMENT db 0

; -------------------------------- data -------------------------------------
data_start:

plat_exit_text: db "Exit", 0

; CP437 characters used by the screen -> Unicode
cp_table:
        db 0x18
        dw 0x2191       ; arrows
        db 0x19
        dw 0x2193
        db 0x1A
        dw 0x2192
        db 0x1B
        dw 0x2190
        db 0xC9
        dw 0x2554       ; double line box corners and edges
        db 0xBB
        dw 0x2557
        db 0xC8
        dw 0x255A
        db 0xBC
        dw 0x255D
        db 0xCD
        dw 0x2550
        db 0xBA
        dw 0x2551
        db 0xDA
        dw 0x250C       ; single line box corners and edges
        db 0xBF
        dw 0x2510
        db 0xC0
        dw 0x2514
        db 0xD9
        dw 0x2518
        db 0xC4
        dw 0x2500
        db 0xB3
        dw 0x2502
        db 0xC7
        dw 0x255F       ; joins between double and single lines
        db 0xB4
        dw 0x2524
        db 0xD1
        dw 0x2564
        db 0xCF
        dw 0x2567
        db 0x10
        dw 0x25BA       ; pointers either side of the selected key
        db 0x11
        dw 0x25C4
        db 0

cur_attr:       db 0xFF
off_x:          db 0
off_y:          db 0
olen:           db 0
        align 8, db 0
image_handle:   DPTR 0
conin:          DPTR 0
conout:         DPTR 0
bootsvc:        DPTR 0
saved_sp:       DPTR 0
cols:           DPTR 0
rows:           DPTR 0
event_index:    DPTR 0
key:            dw 0, 0                 ; scan code, Unicode character
saved_cw:       dw 0
obuf:           times 82 dw 0

%include "data.inc"

        times (ALIGNMENT - (($ - $$) % ALIGNMENT)) % ALIGNMENT db 0

; A relocation table with one empty block: tells the loader the image may be
; placed at any address (nothing actually needs patching).
reloc_start:
        dd      0, 12
        dw      0, 0
reloc_end:
        times (ALIGNMENT - (($ - $$) % ALIGNMENT)) % ALIGNMENT db 0
image_end:
