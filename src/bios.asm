; ---------------------------------------------------------------------------
; bios.asm - legacy BIOS target: boot sector + 16-bit real mode calculator.
;
; Runs on any PC with a BIOS (or UEFI with CSM/legacy boot) and a 386 or
; later CPU with an FPU - 32-bit and 64-bit machines alike.
;
;   sector 0      boot sector: loads the following sectors, jumps to them
;   sector 1..n   the calculator itself
;
; Build: nasm -f bin -DPART_SECTORS=<n> -o bios.bin bios.asm
; ---------------------------------------------------------------------------

        bits    16
        org     0x7C00

%define xax eax
%define xbx ebx
%define xcx ecx
%define xdx edx
%define xsi esi
%define xdi edi
%define M(s) s

%ifndef PART_START
  %define PART_START 2048
%endif
%ifndef PART_SECTORS
  %define PART_SECTORS 0
%endif

STAGE2_SECTORS  equ (image_end - stage2 + 511) / 512

; ------------------------------- boot sector -------------------------------
boot:
        jmp     short .start
        nop
        times 0x5A - ($ - $$) db 0      ; room for a BPB some BIOSes patch in
.start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti
        cld
        mov     [boot_drive], dl
        mov     di, 3                   ; three attempts
.read:
        mov     ax, 0x0200 | STAGE2_SECTORS
        mov     cx, 0x0002              ; cylinder 0, sector 2
        xor     dh, dh                  ; head 0
        mov     dl, [boot_drive]
        mov     bx, stage2
        int     0x13
        jnc     stage2
        xor     ah, ah                  ; reset the drive and retry
        mov     dl, [boot_drive]
        int     0x13
        dec     di
        jnz     .read
        mov     si, msg_disk
        ; fall through

; Print the zero terminated string at si and stop.
fatal:
        lodsb
        test    al, al
        jz      .halt
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     fatal
.halt:
        hlt
        jmp     .halt

msg_disk:       db "Disk read error", 0
boot_drive:     db 0

        times 0x1BE - ($ - $$) db 0
        ; partition table: one bootable EFI system partition (FAT) that
        ; holds the UEFI builds of the calculator
        db      0x80                    ; active
        db      0x20, 0x21, 0x00        ; CHS of first sector
        db      0xEF                    ; EFI system partition
        db      0xFE, 0xFF, 0xFF        ; CHS of last sector
        dd      PART_START
        dd      PART_SECTORS
        times 3 * 16 db 0
        dw      0xAA55

; ------------------------------ calculator ---------------------------------
stage2:
        movzx   esp, sp
        fninit                          ; make sure there is an FPU
        mov     word [tmpd], 0x5A5A
        fnstsw  word [tmpd]
        cmp     byte [tmpd], 0
        je      .fpu_ok
        mov     si, msg_fpu
        jmp     fatal
.fpu_ok:
        mov     ax, 0x0003              ; 80x25 colour text, clears the screen
        int     0x10
        mov     ah, 0x01                ; hide the cursor
        mov     cx, 0x2000
        int     0x10
        jmp     calc_main

msg_fpu:        db "This calculator needs a 386 or later CPU with an FPU", 0

; --- platform layer: text is written straight into VGA memory ---
plat_setattr:
        mov     [cur_attr], al
        ret

plat_gotoxy:
        mov     [cur_x], dl
        mov     [cur_y], dh
        ret

plat_putc:
        push    eax
        push    ebx
        push    es
        push    ax
        mov     ax, 0xB800
        mov     es, ax
        movzx   bx, byte [cur_y]
        imul    bx, bx, 80
        movzx   ax, byte [cur_x]
        add     bx, ax
        shl     bx, 1
        pop     ax
        mov     ah, [cur_attr]
        mov     [es:bx], ax
        inc     byte [cur_x]
        pop     es
        pop     ebx
        pop     eax
        ret

plat_getkey:
        pushad                          ; shield 32-bit registers from the BIOS
        xor     ah, ah
        int     0x16                    ; al = ASCII, ah = scan code
        mov     [key_tmp], ax
        popad
        mov     ax, [key_tmp]
        test    al, al
        jz      .extended
        movzx   eax, al
        ret
.extended:
        mov     eax, KEY_UP
        cmp     byte [key_tmp + 1], 0x48
        je      .done
        mov     eax, KEY_DOWN
        cmp     byte [key_tmp + 1], 0x50
        je      .done
        mov     eax, KEY_LEFT
        cmp     byte [key_tmp + 1], 0x4B
        je      .done
        mov     eax, KEY_RIGHT
        cmp     byte [key_tmp + 1], 0x4D
        je      .done
        mov     eax, KEY_DEL
        cmp     byte [key_tmp + 1], 0x53
        je      .done
        mov     eax, 0xFFFF             ; anything else: ignored by the core
.done:
        ret

plat_exit:                              ; warm reboot
        mov     ax, 0x0003
        int     0x10
        mov     word [0x0472], 0x1234
        jmp     0xFFFF:0x0000

plat_exit_text: db "Reboot", 0
cur_attr:       db 0x07
cur_x:          db 0
cur_y:          db 0
key_tmp:        dw 0

%include "core.inc"
%include "data.inc"

image_end:
        ; build.sh checks that this stays within 17 sectors (one floppy track)
        times (STAGE2_SECTORS + 1) * 512 - ($ - $$) db 0
