
; Intercept INT 21h and trick FLOP.EXE into thinking the date/time of
; it's own executable are the exact (invalid) values it expects.
;
; It complains "do not mdofiy this executable" otherwise.
;
; It expects DOS to report time=0000h date=2800h
; Note that 2800 is 2000-0-0 which is an invalid date.
;
; The ZIP archive unpacks instead to 1999-12-31 00:00:00 which it complains about.
;
; This is a quick and dirty hack and not idiot proof. Be careful.

org         0x100
bits        16

; Entry point
; -----------
            xor     ax,ax
            mov     es,ax
            push    cs
            pop     ds
            cli

; save old INT 21h vector
            mov     ax,word [es:(0x21*4)]
            mov     bx,word [es:(0x21*4+2)]
            mov     word [old_int21],ax
            mov     word [old_int21+2],bx

; write new vector
            mov     word [es:(0x21*4)],new_int21
            mov     word [es:(0x21*4+2)],cs

; terminate and stay resident
            sti
            mov     ax,3100h
            mov     dx,the_end + 0xF
            mov     cl,4
            shr     dx,cl
            int     21h
            jmp     short $

; old INT 21h vector
old_int21   dd      0

; new INT 21h
new_int21:
            cmp     ax,0x5700               ; we're looking for "get file last written date/time"
            jne     new_int21_pass
;---------------------------------------- LIE, Say the date is 2800h and time is 0000h
            pushf
            call far word [cs:old_int21]    ; call through to DOS, so when DOS returns we can change the result
            jc      new_int21_pass          ; not if error
            mov     cx,0x0000               ; change result, time
            mov     dx,0x2800               ; change result, date
            iret
;----------------------------------------
new_int21_pass:
            jmp far word [cs:old_int21]

; end symbol
the_end:    db      'FLOP.EXE last/modified time/date hack (C) 2018 Hackipedia/DOSLIB project'

