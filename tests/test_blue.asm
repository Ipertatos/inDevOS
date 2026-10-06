; prepend custom header -------------------------------------------------
org 0
    db 'EXE!'
    dd 8              ; entry offset (absolute from file start)

; test_blue.asm  – blue screen + serial spam for inDevOS
bits 64

; -----------------------------------------------------------------
; Hardware-only version (no kernel function calls)
; -----------------------------------------------------------------
%define SERIAL_PORT  0x3F8     ; COM1 base port

; -----------------------------------------------------------------
_start:
    ; --- Just spam serial port (skip framebuffer for now) -------
    mov     r10, 10            ; repeat count (reduced for testing)

.spam_outer:
    lea     rsi, [rel msg]     ; RIP-relative address of message
.spam_char:
    lodsb                      ; load next character into AL
    test    al, al             ; check for null terminator
    jz      .spam_next         ; if null, move to next iteration
    
    ; wait for serial port ready
.wait_ready:
    mov     dx, SERIAL_PORT + 5  ; line status register
    in      al, dx
    test    al, 0x20           ; transmit holding register empty?
    jz      .wait_ready        ; if not, keep waiting
    
    ; send character (reload from memory)
    mov     al, [rsi-1]        ; get the character we just loaded
    mov     dx, SERIAL_PORT    ; data register
    out     dx, al             ; send it
    
    jmp     .spam_char

.spam_next:
    dec     r10
    jnz     .spam_outer

    ; --- Simple infinite loop to see if we get this far ----------
.done:
    mov     rax, 0x12345678    ; distinctive pattern
    jmp     .done              ; infinite loop instead of ret

msg db "HW-TEST!",13,10,0      ; message with CRLF
