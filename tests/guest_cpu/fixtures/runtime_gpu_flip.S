// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    mov r13d,1
    mov edi,1000
    xor esi,esi
    xor edx,edx
    xor ecx,ecx
    call qword ptr [rip+got]
    cmp eax,1
    jne failed
    lea rdi,[rip+attr]
    mov esi,0x80000000
    mov edx,1
    xor ecx,ecx
    mov r8d,64
    mov r9d,64
    sub rsp,16
    mov qword ptr [rsp],64
    call qword ptr [rip+got+8]
    add rsp,16
    lea rax,[rip+fb0]
    mov [rip+addresses],rax
    lea rax,[rip+fb1]
    mov [rip+addresses+8],rax
    mov edi,1
    xor esi,esi
    lea rdx,[rip+addresses]
    mov ecx,2
    lea r8,[rip+attr]
    call qword ptr [rip+got+24]
    test eax,eax
    js failed
    inc r13d
    lea rdi,[rip+queue]
    lea rsi,[rip+queue_name]
    call qword ptr [rip+got+64]
    test eax,eax
    jne failed
    mov rdi,[rip+queue]
    mov esi,1
    mov edx,0x314159
    call qword ptr [rip+got+32]
    test eax,eax
    jne failed
    mov r14d,0
round:
    inc r13d
    mov eax,r14d
    and eax,1
    lea rdx,[rip+addresses]
    mov rdi,[rdx+rax*8]
    mov eax,0xff0033cc
    test r14d,1
    jz color
    mov eax,0xff66cc00
color:
    mov ecx,4096
    rep stosd // Subsequent rounds must retire real GPU write watches.
    lea rdi,[rip+command]
    mov eax,0
    mov ecx,64
    rep stosd
    mov dword ptr [rip+command],0xc03e1000
    mov dword ptr [rip+command+4],0x68750777 // PrepareFlip, verified against native enum.
    lea rax,[rip+command]
    mov [rip+dcb],rax
    mov edi,1
    lea rsi,[rip+dcb]
    lea rdx,[rip+dcb_size]
    xor ecx,ecx
    xor r8d,r8d
    mov r9d,1
    sub rsp,32
    mov eax,r14d
    and eax,1
    mov [rsp],rax
    mov qword ptr [rsp+8],1
    mov [rsp+16],r14
    call qword ptr [rip+got+48]
    add rsp,32
    test eax,eax
    jne failed
    call qword ptr [rip+got+56]
    test eax,eax
    jne failed
    mov rdi,[rip+queue]
    lea rsi,[rip+event]
    mov edx,1
    lea rcx,[rip+event_count]
    lea r8,[rip+timeout]
    call qword ptr [rip+got+72]
    test eax,eax
    jne failed
    cmp dword ptr [rip+event_count],1
    jne failed
    cmp qword ptr [rip+event+24],0x314159
    jne failed
    inc r14d
    cmp r14d,4
    jb round
    mov rdi,[rip+queue]
    call qword ptr [rip+got+80]
    test eax,eax
    jne failed
    mov edi,1
    call qword ptr [rip+got+40]
    test eax,eax
    jne failed
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
attr: .fill 40,1,0
addresses: .quad 0,0
queue: .quad 0
queue_name: .asciz "gpu-flip-contract"
.balign 8
event: .fill 32,1,0
event_count: .long 0
timeout: .long 2000000
dcb_size: .long 256
.balign 8
dcb: .quad 0
.balign 256
command: .fill 256,1,0
.org 0x8000
got: .fill 11,8,0
.set fb0,_start+0xc000
.set fb1,_start+0x10000
