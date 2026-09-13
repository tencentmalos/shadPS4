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
    call qword ptr [rip+got_init]
    cmp eax,0x80b80001
    jne failed
    call qword ptr [rip+got_common]
    check
    call qword ptr [rip+got_init]
    check
    mov r13d,2
    lea rax,[rip+items]
    mov qword ptr [rip+param+72],rax
    lea rax,[rip+message_param]
    mov qword ptr [rip+param+80],rax
    lea rax,[rip+message]
    mov qword ptr [rip+message_param+8],rax
    // Invalid nested message rejected, then same initialized dialog can retry.
    mov qword ptr [rip+message_param+8],1
    lea rdi,[rip+param]
    call qword ptr [rip+got_open]
    cmp eax,0x80b8000a
    jne failed
    lea rax,[rip+message]
    mov qword ptr [rip+message_param+8],rax
    lea rdi,[rip+param]
    call qword ptr [rip+got_open]
    check
    mov r13d,3
    lea rdi,[rip+result]
    call qword ptr [rip+got_result]
    test eax,eax
    je poll
    cmp eax,0x80b80005
    jne failed
poll:
    call qword ptr [rip+got_status]
    cmp eax,2
    je poll
    cmp eax,3
    jne failed
    mov r13d,4
    lea rdi,[rip+result]
    call qword ptr [rip+got_result]
    check
    cmp dword ptr [rip+result],2
    jne failed
    cmp qword ptr [rip+result+32],0x12345678
    jne failed
    // This fixture expects an actual user/app cancellation, never synthetic OK.
    cmp dword ptr [rip+result+4],1
    jne failed
    cmp dword ptr [rip+result+8],0
    jne failed
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
.org 0x4400
param:
.quad 48
.fill 40,1,0
.long 152,2,1,0
.quad 0,0,0,0,0,0,0x12345678,0
.fill 24,1,0
items:
.long 1000,0
.quad 0,0
.long 0,0
.quad 0
.long 0,0
.quad 0
.long 0
.fill 36,1,0
message_param:
.long 3,0
.quad 0
.fill 32,1,0
message: .asciz "Savedata dialog cancellation contract"
.balign 8
result: .fill 72,1,0
.org 0x8000
got_init: .quad 0
got_open: .quad 0
got_status: .quad 0
got_result: .quad 0
got_term: .quad 0
got_common: .quad 0
