/* El Torito real-mode guest for end-to-end Android input. EBP marks readiness.
   EAX receives the success marker only after both clicks and relative motion. */
.code16
.intel_syntax noprefix
.text
.global _start
_start:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x7000
    mov al, 0xa8
    out 0x64, al
    mov al, 0xd4
    out 0x64, al
    mov al, 0xf4
    out 0x60, al
    call aux_byte
    cmp al, 0xfa
    jne failed
    mov ebp, 0x52504701
    xor si, si
packet:
    call aux_byte
    mov bh, al
    call aux_byte
    mov bl, al
    call aux_byte
    or al, bl
    jz buttons
    or si, 1
buttons:
    test bh, 1
    jz left_up
    or si, 2
    jmp right
left_up:
    test si, 2
    jz right
    or si, 8
right:
    test bh, 2
    jz right_up
    or si, 4
    jmp check
right_up:
    test si, 4
    jz check
    or si, 16
check:
    cmp si, 31
    jne packet
    mov eax, 0x52474250
    hlt
failed:
    mov eax, 0xbad
    hlt
aux_byte:
    in al, 0x64
    and al, 0x21
    cmp al, 0x21
    jne aux_byte
    in al, 0x60
    ret
