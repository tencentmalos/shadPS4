// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
.macro check
    test eax,eax
    jne failed
.endm
_start:
    mov r12,rsi
    sub rsp,8
    mov r13d,1
    xor edi,edi
    call qword ptr [rip+got_init]
    check
    // Refuse nested bad pointer before creating a directory/mount.
    mov r13d,2
    mov qword ptr [rip+mount+8],1
    lea rdi,[rip+mount]
    lea rsi,[rip+result]
    call qword ptr [rip+got_mount]
    cmp eax,0x809f0000
    jne failed
    lea rax,[rip+directory]
    mov qword ptr [rip+mount+8],rax
    mov qword ptr [rip+backup_input+16],rax
    // Bad output must also be refused before side effects.
    lea rdi,[rip+mount]
    mov esi,1
    call qword ptr [rip+got_mount]
    cmp eax,0x809f0000
    jne failed
    mov r13d,3
    lea rdi,[rip+mount]
    lea rsi,[rip+result]
    call qword ptr [rip+got_mount]
    check
    // /savedata0, and reserved outputs initialized.
    cmp byte ptr [rip+result],47
    jne failed
    cmp qword ptr [rip+result+32],0
    jne failed
    mov r13d,4
    lea rdi,[rip+path]
#ifdef READ_SAVE
    xor esi,esi
#else
    mov esi,0x602
#endif
    mov edx,0x180
    call qword ptr [rip+got_open]
    test eax,eax
    js failed
    mov r14d,eax
#ifndef READ_SAVE
    mov r13d,5
    mov edi,r14d
    lea rsi,[rip+payload]
    mov edx,16
    call qword ptr [rip+got_write]
    cmp rax,16
    jne failed
    mov edi,r14d
    call qword ptr [rip+got_sync]
    check
    mov edi,r14d
    xor esi,esi
    xor edx,edx
    call qword ptr [rip+got_seek]
    check
#endif
    mov r13d,6
    mov edi,r14d
    lea rsi,[rip+readback]
    mov edx,16
    call qword ptr [rip+got_read]
    cmp rax,16
    jne failed
    mov rax,qword ptr [rip+payload]
    cmp qword ptr [rip+readback],rax
    jne failed
    mov rax,qword ptr [rip+payload+8]
    cmp qword ptr [rip+readback+8],rax
    jne failed
    mov r13d,7
    lea rdi,[rip+result]
    call qword ptr [rip+got_umount]
    cmp eax,0x809f0003 // outstanding descriptor => Busy
    jne failed
    mov edi,r14d
    call qword ptr [rip+got_close]
    check
    lea rdi,[rip+result]
#ifdef READ_SAVE
    call qword ptr [rip+got_umount]
    check
#else
    call qword ptr [rip+got_backup]
    check
    mov r13d,8
    xor edi,edi
    lea rsi,[rip+event]
    call qword ptr [rip+got_event]
    check
    cmp qword ptr [rip+event],1 // type=UMOUNT_BACKUP, error=0
    jne failed
    lea rdi,[rip+backup_input]
    call qword ptr [rip+got_check]
    check
    // Replace live bytes after the backup, then restore. The subsequent read-only
    // fixture in another Session/process must observe the original 16 bytes.
    mov dword ptr [rip+mount+24],2
    lea rdi,[rip+mount]
    lea rsi,[rip+result]
    call qword ptr [rip+got_mount]
    check
    lea rdi,[rip+path]
    mov esi,0x402
    mov edx,0x180
    call qword ptr [rip+got_open]
    test eax,eax
    js failed
    mov r14d,eax
    mov edi,r14d
    lea rsi,[rip+event] // differs from the payload
    mov edx,8
    call qword ptr [rip+got_write]
    cmp eax,8
    jne failed
    mov edi,r14d
    call qword ptr [rip+got_close]
    check
    lea rdi,[rip+result]
    call qword ptr [rip+got_umount]
    check
    mov r13d,9
    lea rdi,[rip+backup_input]
    call qword ptr [rip+got_restore]
    check
#endif
    call qword ptr [rip+got_term]
    check
    mov eax,0xcafe
    add rsp,8
    jmp r12
failed:
    mov eax,r13d
    or eax,0xbad000
    add rsp,8
    jmp r12
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4200
mount:
.long 1000,0
.quad 0,96
#ifdef READ_SAVE
.long 1
#else
.long 34
#endif
.fill 36,1,0
result: .fill 64,1,0xa5
backup_input: .long 1000,0
.quad 0,0,0,0
.fill 32,1,0
event: .fill 104,1,0
directory: .asciz "production-contract"
path: .asciz "/savedata0/roundtrip.bin"
.balign 8
payload: .quad 0x0123456789abcdef,0x8877665544332211
readback: .fill 16,1,0
.org 0x8000
got_init: .quad 0
got_mount: .quad 0
got_umount: .quad 0
got_term: .quad 0
got_open: .quad 0
got_write: .quad 0
got_sync: .quad 0
got_seek: .quad 0
got_read: .quad 0
got_close: .quad 0
got_backup: .quad 0
got_event: .quad 0
got_check: .quad 0
got_restore: .quad 0
