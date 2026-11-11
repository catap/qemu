bits 16
default abs
org 0xf0000

%define PML4 0x1000
%define PDPT 0x2000
%define PD 0x3000
%define PT 0x6000
%define EXPECTED_RIP 0x4000
%define RESUME_RIP 0x4008
%define FAULT_COUNT 0x4010
%define FAULT_ARMED 0x4018
%define EXPECTED_VECTOR 0x4020
%define IDT 0x5000
%define STACK_TOP 0x9000
%define BUFFER 0x10000
%define ALIAS (BUFFER + 0x200000)
%define CROSS_PAGE 0x11ffc
%define AP_START 0x7000
%define APIC_PD 0xa000
%define AP_STACK_TOP 0xd000
%define APIC_BASE 0xfee00000
%define AP_READY 0x4100
%define SMP_COMMAND 0x4108
%define SMP_ACK 0x4110
%define SMP_COUNTER 0x10100
%define SMP_ITERATIONS 4096
%define FAILED_MARK 0x12ffc
%define COPY_SOURCE 0x20000
%define COPY_TARGET 0x21000
%define COPY_ALIAS (COPY_SOURCE + 0x200000)
%define COPY_IMAGE 0x23fff
%define REGION_SOURCE 0x26ffb
%define REGION_TARGET 0x28ff7
%define REGION_IMAGE 0x2afff
%define REGION_LENGTH 19

%define CODE32 0x08
%define DATA 0x10
%define CODE64 0x18

%assign expected_faults 0

%macro dead_memory 2
    mov rdi, %1
    mov rcx, %2
    db 0x0f, 0x3f
%endmacro

%macro expect_exception 2+
    mov qword [EXPECTED_VECTOR], %1
    mov qword [EXPECTED_RIP], %%fault
    mov qword [RESUME_RIP], %%resumed
    mov qword [FAULT_ARMED], 1
%%fault:
    %2
    jmp failed
%%resumed:
    cmp qword [FAULT_ARMED], 0
    jne failed
%assign expected_faults expected_faults + 1
%endmacro

%macro expect_fault 1+
    expect_exception 13, %1
%endmacro

%macro expect_ud_fault 1+
    expect_exception 6, %1
%endmacro

%macro expect_pf_fault 1+
    expect_exception 14, %1
%endmacro

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_TOP
    in al, 0x92
    or al, 2
    and al, 0xfe
    out 0x92, al
    lgdt [cs:gdt_pointer - $$]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE32:protected_mode

bits 32
protected_mode:
    mov ax, DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, STACK_TOP

    xor eax, eax
    mov edi, PML4
    mov ecx, 6 * 4096 / 4
    rep stosd
    mov dword [PML4], PDPT | 3
    mov dword [PDPT], PD | 3
    mov dword [PD], PT | 3
    mov dword [PD + 8], PT | 3
    mov edi, PT
    mov eax, 3
    mov ecx, 512
.fill_pt:
    mov [edi], eax
    add edi, 8
    add eax, 4096
    loop .fill_pt

%ifdef DEAD_SMP
    xor eax, eax
    mov edi, APIC_PD
    mov ecx, 4096 / 4
    rep stosd
    mov dword [PDPT + 3 * 8], APIC_PD | 3
    mov dword [APIC_PD + 503 * 8], APIC_BASE | 0x83
%endif

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov eax, PML4
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    jmp CODE64:long_mode

bits 64
long_mode:
    mov rsp, STACK_TOP
    xor eax, eax
    mov fs, ax
    mov gs, ax

    mov edi, IDT
    mov ecx, 256
    mov rax, unexpected_fault
.fill_idt:
    mov word [rdi], ax
    mov word [rdi + 2], CODE64
    mov byte [rdi + 4], 0
    mov byte [rdi + 5], 0x8e
    mov rdx, rax
    shr rdx, 16
    mov word [rdi + 6], dx
    shr rdx, 16
    mov dword [rdi + 8], edx
    mov dword [rdi + 12], 0
    add rdi, 16
    loop .fill_idt

    mov eax, general_protection
    mov word [IDT + 13 * 16], ax
    shr eax, 16
    mov word [IDT + 13 * 16 + 6], ax
    mov eax, page_fault
    mov word [IDT + 14 * 16], ax
    shr eax, 16
    mov word [IDT + 14 * 16 + 6], ax
    mov eax, invalid_opcode
    mov word [IDT + 6 * 16], ax
    shr eax, 16
    mov word [IDT + 6 * 16 + 6], ax
    lidt [idt_pointer]

    mov esi, starting_message
    call puts

%ifdef DEAD_SMP
    jmp smp_bsp_test
%endif

%ifdef DEAD_FEATURE_DISABLED
    expect_ud_fault db 0x0f, 0x3f
    expect_ud_fault db 0x0f, 0x3e, 0xc0
    expect_ud_fault db 0x0f, 0x3d
    expect_ud_fault db 0x0f, 0x3c
    expect_ud_fault db 0x0f, 0x3b
%else
    ; Check exception recovery independently of the experimental opcodes.
    mov rax, 0x0000800000000000
    expect_fault mov rax, [rax]

    mov edi, COPY_SOURCE
    mov ecx, 4096 / 8
    mov rax, 0x0123456789abcdef
    rep stosq
    dead_memory COPY_SOURCE, 4096
    mov dword [COPY_SOURCE], 0x12345678
    mov byte [COPY_SOURCE + 9], 0xa5
    mov byte [COPY_SOURCE + 4095], 0x5a
    mov edi, COPY_TARGET
    mov esi, COPY_SOURCE
    mov ecx, 4096
    stc
    db 0x0f, 0x3d
    jnc failed
    cmp rdi, COPY_TARGET
    jne failed
    cmp rsi, COPY_SOURCE
    jne failed
    cmp rcx, 4096
    jne failed
    cmp dword [COPY_TARGET], 0x12345678
    jne failed
    cmp byte [COPY_TARGET + 9], 0xa5
    jne failed
    cmp byte [COPY_TARGET + 4095], 0x5a
    jne failed
    expect_fault mov al, [COPY_TARGET + 4]
    expect_fault mov ax, [COPY_TARGET + 3]
    expect_fault mov al, [COPY_SOURCE + 4]

    dead_memory COPY_IMAGE, 4608
    mov edi, COPY_IMAGE
    mov esi, COPY_TARGET
    mov ecx, 4096
    db 0x0f, 0x3c
    cmp dword [COPY_IMAGE], 0x12345678
    jne failed
    cmp dword [COPY_IMAGE + 4], 0x01234567
    jne failed
    cmp byte [COPY_IMAGE + 9], 0xa5
    jne failed
    cmp byte [COPY_IMAGE + 4095], 0x5a
    jne failed
    cmp byte [COPY_IMAGE + 4096], 0xf0
    jne failed
    cmp byte [COPY_IMAGE + 4097], 0xfd
    jne failed
    cmp byte [COPY_IMAGE + 4098], 0xff
    jne failed
    cmp byte [COPY_IMAGE + 4607], 0x7f
    jne failed
    expect_fault mov al, [COPY_TARGET + 4]

    mov edi, COPY_SOURCE
    mov ecx, 4096 / 8
    xor eax, eax
    rep stosq
    mov edi, COPY_SOURCE
    mov esi, COPY_IMAGE
    mov ecx, 4096
    db 0x0f, 0x3b
    cmp dword [COPY_SOURCE], 0x12345678
    jne failed
    cmp byte [COPY_SOURCE + 9], 0xa5
    jne failed
    cmp byte [COPY_SOURCE + 4095], 0x5a
    jne failed
    expect_fault mov al, [COPY_SOURCE + 4]
    expect_fault mov al, [COPY_SOURCE + 8]
    mov edi, COPY_IMAGE
    mov esi, COPY_SOURCE
    mov ecx, 4096
    db 0x0f, 0x3c
    cmp dword [COPY_IMAGE + 4], 0x01234567
    jne failed
    cmp byte [COPY_IMAGE + 4096], 0xf0
    jne failed

    mov edi, COPY_ALIAS
    mov esi, COPY_SOURCE
    mov ecx, 4096
    expect_fault db 0x0f, 0x3d
    mov ecx, 4096
    expect_fault db 0x0f, 0x3c
    mov edi, (COPY_IMAGE & ~4095) + 0x200000
    mov esi, COPY_IMAGE
    mov ecx, 4096
    expect_fault db 0x0f, 0x3b

    dead_memory COPY_IMAGE + 4096, 1
    mov dword [COPY_TARGET], 0x0badc0de
    mov edi, COPY_TARGET
    mov esi, COPY_IMAGE
    mov ecx, 4096
    expect_fault db 0x0f, 0x3b
    cmp dword [COPY_TARGET], 0x0badc0de
    jne failed
    expect_fault mov al, [COPY_TARGET + 4]
    mov byte [COPY_IMAGE + 4096], 0xf0

    mov edi, REGION_SOURCE - 1
    mov ecx, REGION_LENGTH + 2
    mov al, 0x6d
    rep stosb
    dead_memory REGION_SOURCE, REGION_LENGTH
    mov byte [REGION_SOURCE], 0x11
    mov byte [REGION_SOURCE + 7], 0x22
    mov byte [REGION_SOURCE + 8], 0x33
    mov byte [REGION_SOURCE + 18], 0x44
    mov edi, REGION_TARGET - 1
    mov ecx, REGION_LENGTH + 2
    mov al, 0xee
    rep stosb
    dead_memory REGION_TARGET - 1, REGION_LENGTH + 2
    mov edi, REGION_TARGET
    mov esi, REGION_SOURCE
    mov ecx, REGION_LENGTH
    db 0x0f, 0x3d
    cmp rcx, REGION_LENGTH
    jne failed
    cmp byte [REGION_TARGET], 0x11
    jne failed
    cmp byte [REGION_TARGET + 7], 0x22
    jne failed
    cmp byte [REGION_TARGET + 8], 0x33
    jne failed
    cmp byte [REGION_TARGET + 18], 0x44
    jne failed
    expect_fault mov al, [REGION_TARGET + 1]
    expect_fault mov al, [REGION_TARGET + 17]
    expect_fault mov al, [REGION_TARGET - 1]
    expect_fault mov al, [REGION_TARGET + REGION_LENGTH]

    mov edi, REGION_IMAGE
    mov esi, REGION_TARGET
    mov ecx, REGION_LENGTH
    db 0x0f, 0x3c
    cmp byte [REGION_IMAGE], 0x11
    jne failed
    cmp byte [REGION_IMAGE + 1], 0x6d
    jne failed
    cmp byte [REGION_IMAGE + 7], 0x22
    jne failed
    cmp byte [REGION_IMAGE + 8], 0x33
    jne failed
    cmp byte [REGION_IMAGE + 18], 0x44
    jne failed
    cmp byte [REGION_IMAGE + REGION_LENGTH], 0x7e
    jne failed
    cmp byte [REGION_IMAGE + REGION_LENGTH + 1], 0xfe
    jne failed
    cmp byte [REGION_IMAGE + REGION_LENGTH + 2], 0x03
    jne failed
    mov edi, COPY_IMAGE
    mov esi, REGION_TARGET - 1
    mov ecx, REGION_LENGTH + 2
    db 0x0f, 0x3c
    cmp byte [COPY_IMAGE], 0xee
    jne failed
    cmp byte [COPY_IMAGE + REGION_LENGTH + 1], 0xee
    jne failed

    mov edi, REGION_TARGET
    mov ecx, REGION_LENGTH
    mov al, 0x55
    rep stosb
    mov edi, REGION_TARGET
    mov esi, REGION_IMAGE
    mov ecx, REGION_LENGTH
    db 0x0f, 0x3b
    cmp byte [REGION_TARGET], 0x11
    jne failed
    cmp byte [REGION_TARGET + 7], 0x22
    jne failed
    cmp byte [REGION_TARGET + 8], 0x33
    jne failed
    cmp byte [REGION_TARGET + 18], 0x44
    jne failed
    expect_fault mov al, [REGION_TARGET + 1]
    expect_fault mov al, [REGION_TARGET + 17]
    expect_fault mov al, [REGION_TARGET - 1]
    expect_fault mov al, [REGION_TARGET + REGION_LENGTH]
    mov edi, COPY_IMAGE
    mov esi, REGION_TARGET - 1
    mov ecx, REGION_LENGTH + 2
    db 0x0f, 0x3c
    cmp byte [COPY_IMAGE], 0xee
    jne failed
    cmp byte [COPY_IMAGE + REGION_LENGTH + 1], 0xee
    jne failed

    mov rdi, -1
    mov rsi, -2
    xor ecx, ecx
    stc
    db 0x0f, 0x3d
    jnc failed
    db 0x0f, 0x3c
    jnc failed
    db 0x0f, 0x3b
    jnc failed
    cmp rdi, -1
    jne failed
    cmp rsi, -2
    jne failed
    test rcx, rcx
    jne failed

    mov r14, rsp
    expect_fault db 0x0f, 0x3e, 0xc4
    cmp rsp, r14
    jne failed

    mov rax, 0x1122334455667788
    mov [BUFFER], rax
    dead_memory BUFFER, 8
    cmp rdi, BUFFER
    jne failed
    cmp rcx, 8
    jne failed
    expect_fault mov rax, [BUFFER]
    expect_fault add qword [BUFFER], 1
    expect_fault lock add qword [BUFFER], 1

    mov byte [BUFFER], 0xa5
    mov al, [BUFFER]
    cmp al, 0xa5
    jne failed
    expect_fault mov rax, [BUFFER]

    dead_memory BUFFER, 8
    expect_fault mov al, [BUFFER]
    dead_memory BUFFER, 0
    expect_fault mov al, [BUFFER]

    mov rax, 0x8877665544332211
    mov [BUFFER], rax
    mov rdx, [BUFFER]
    cmp rax, rdx
    jne failed
    dead_memory BUFFER, 0
    mov rdx, [BUFFER]
    cmp rax, rdx
    jne failed

    mov rax, 0x0123456789abcdef
    mov [CROSS_PAGE], rax
    dead_memory CROSS_PAGE, 8
    expect_fault mov rax, [CROSS_PAGE]
    mov dword [CROSS_PAGE], 0x12345678
    mov eax, [CROSS_PAGE]
    cmp eax, 0x12345678
    jne failed
    expect_fault mov rax, [CROSS_PAGE]
    expect_fault mov eax, [CROSS_PAGE + 4]
    mov dword [CROSS_PAGE + 4], 0x76543210
    mov rax, [CROSS_PAGE]
    mov rdx, 0x7654321012345678
    cmp rax, rdx
    jne failed

    dead_memory BUFFER, 8
    expect_fault mov rax, [ALIAS]
    mov byte [ALIAS], 0x33
    mov al, [BUFFER]
    cmp al, 0x33
    jne failed
    expect_fault mov rax, [BUFFER]
    mov rax, 0x123456789abcdef0
    mov [ALIAS], rax
    mov rdx, [BUFFER]
    cmp rax, rdx
    jne failed

    dead_memory BUFFER, 8
    and qword [PT + (BUFFER >> 12) * 8], ~2
    mov rax, cr0
    or rax, 1 << 16
    mov cr0, rax
    invlpg [BUFFER]
    expect_pf_fault mov byte [BUFFER], 0x7a
    or qword [PT + (BUFFER >> 12) * 8], 2
    invlpg [BUFFER]
    expect_fault mov al, [BUFFER]
    mov qword [BUFFER], 0

    mov rdi, -4
    mov rcx, 8
    expect_fault db 0x0f, 0x3f

    mov rax, 0x1122334455667788
    db 0x0f, 0x3e, 0xc0
    db 0x0f, 0x3e, 0xc0
    expect_fault mov rdx, rax
    expect_fault add rax, 1
    expect_fault xor eax, eax
    mov al, 0x12
    mov dl, al
    cmp dl, 0x12
    jne failed
    expect_fault mov dl, ah
    expect_fault mov dx, ax
    expect_fault mov rdx, rax
    mov ah, 0x34
    mov dx, ax
    cmp dx, 0x3412
    jne failed
    expect_fault mov edx, eax
    expect_fault mov rdx, rax
    mov ax, 0x5678
    mov dx, ax
    cmp dx, 0x5678
    jne failed
    expect_fault mov rdx, rax
    mov eax, 0x89abcdef
    mov rdx, rax
    mov ecx, 0x89abcdef
    cmp rdx, rcx
    jne failed

    mov r8, 0x1122334455667788
    db 0x41, 0x0f, 0x3e, 0xc0
    expect_fault mov r9, r8
    mov r8b, 0x5a
    mov r9b, r8b
    cmp r9b, 0x5a
    jne failed
    expect_fault mov r9d, r8d
    mov r8d, 0x12345678
    mov r9, r8
    cmp r9, 0x12345678
    jne failed

    mov rax, BUFFER
    db 0x0f, 0x3e, 0xc0
    expect_fault mov rdx, [rax]
    expect_fault lea rdx, [rax + 8]
    mov rax, BUFFER
    mov rdx, [rax]

    mov qword [BUFFER + 8], 0
    db 0x0f, 0x3e, 0xc0
    expect_fault mov [BUFFER + 8], rax
    mov rax, 0
    cmp qword [BUFFER + 8], 0
    jne failed

    db 0x0f, 0x3e, 0xc0
    expect_ud_fault cpuid
    mov eax, 0
    cpuid
%endif

    cmp qword [FAULT_COUNT], expected_faults
    jne failed
    mov esi, passed_message
    call puts
    xor eax, eax
    out 0xf4, eax
    jmp halted

%ifdef DEAD_SMP
%assign expected_faults 0

%macro wait_value 2
    mov ebx, 0x1000000
%%retry:
    cmp dword [%1], %2
    je %%done
    pause
    dec ebx
    jne %%retry
    jmp failed
%%done:
%endmacro

%macro wait_icr 0
    mov ecx, 0x100000
%%retry:
    mov eax, [rbx + 0x300]
    test eax, 0x1000
    jz %%done
    dec ecx
    jne %%retry
    jmp failed
%%done:
%endmacro

smp_bsp_test:
    mov qword [BUFFER], 0
    dead_memory BUFFER, 8
    mov rsi, ap_trampoline
    mov edi, AP_START
    mov ecx, ap_trampoline_end - ap_trampoline
    rep movsb
    mov ebx, APIC_BASE
    mov dword [rbx + 0xf0], 0x1ff
    mov dword [rbx + 0x310], 1 << 24
    mov dword [rbx + 0x300], 0xc500
    wait_icr
    mov dword [rbx + 0x300], 0x8500
    wait_icr
    mov ecx, 128
.init_delay:
    pause
    dec ecx
    jne .init_delay
    mov dword [rbx + 0x300], 0x4600 | (AP_START >> 12)
    wait_icr
    mov ecx, 128
.sipi_delay:
    pause
    dec ecx
    jne .sipi_delay
    mov dword [rbx + 0x300], 0x4600 | (AP_START >> 12)
    wait_icr
    wait_value AP_READY, 1

    ; Each fault assertion owns the shared recovery state until its phase ends.
    and qword [PT + ((FAILED_MARK + 4) >> 12) * 8], ~2
    mov rax, cr0
    or rax, 1 << 16
    mov cr0, rax
    invlpg [FAILED_MARK + 4]
    mov rdi, FAILED_MARK
    mov rcx, 8
    expect_pf_fault db 0x0f, 0x3f
    mov eax, [FAILED_MARK]
    test eax, eax
    jne failed
    or qword [PT + ((FAILED_MARK + 4) >> 12) * 8], 2
    invlpg [FAILED_MARK + 4]
    mov dword [SMP_COMMAND], 1
    wait_value SMP_ACK, 1
    mov al, [BUFFER]
    cmp al, 0x33
    jne failed
    expect_fault mov rax, [BUFFER]
    mov dword [SMP_COMMAND], 2
    wait_value SMP_ACK, 2
    mov rax, 0x123456789abcdef0
    cmp [BUFFER], rax
    jne failed

    mov dword [SMP_COMMAND], 3
    wait_value SMP_ACK, 3
    expect_fault mov rax, [BUFFER]
    mov rax, 0x0fedcba987654321
    mov [BUFFER], rax
    mov dword [SMP_COMMAND], 4
    wait_value SMP_ACK, 4
    cmp qword [BUFFER], 0
    jne failed

    db 0x0f, 0x3e, 0xc0
    mov dword [SMP_COMMAND], 5
    wait_value SMP_ACK, 5
    expect_fault mov rdx, rax
    mov eax, 0

    mov qword [SMP_COUNTER], 0
    dead_memory BUFFER, 8
    mov dword [SMP_COMMAND], 6
    mov ecx, SMP_ITERATIONS
.contend:
    lock add qword [SMP_COUNTER], 1
    dec ecx
    jne .contend
    wait_value SMP_ACK, 6
    cmp qword [SMP_COUNTER], 2 * SMP_ITERATIONS
    jne failed
    expect_fault mov rax, [BUFFER]
    mov qword [BUFFER], 0
    mov dword [SMP_COMMAND], 7
    wait_value SMP_ACK, 7
    cmp qword [FAULT_COUNT], SMP_FAULTS
    jne failed
    mov esi, passed_message
    call puts
    xor eax, eax
    out 0xf4, eax
    jmp halted

bits 16
ap_trampoline:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, AP_STACK_TOP
    lgdt [AP_START + ap_gdt_pointer - ap_trampoline]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE32:ap_protected_mode
ap_gdt_pointer:
    dw gdt_end - gdt - 1
    dd gdt
ap_trampoline_end:

bits 32
ap_protected_mode:
    mov ax, DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, AP_STACK_TOP
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov eax, PML4
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    jmp CODE64:ap_long_mode

bits 64
ap_long_mode:
    mov rsp, AP_STACK_TOP
    lidt [idt_pointer]
    mov dword [AP_READY], 1
    wait_value SMP_COMMAND, 1
    expect_fault mov rax, [ALIAS]
    mov byte [ALIAS], 0x33
    mov dword [SMP_ACK], 1
    wait_value SMP_COMMAND, 2
    mov rax, 0x123456789abcdef0
    mov [ALIAS], rax
    mov dword [SMP_ACK], 2
    wait_value SMP_COMMAND, 3
    dead_memory ALIAS, 8
    mov dword [SMP_ACK], 3
    wait_value SMP_COMMAND, 4
    mov rax, 0x0fedcba987654321
    cmp [ALIAS], rax
    jne failed
    dead_memory ALIAS, 8
    expect_fault lock add qword [ALIAS], 1
    mov qword [ALIAS], 0
    mov dword [SMP_ACK], 4
    wait_value SMP_COMMAND, 5
    mov rdx, rax
    mov eax, 0
    mov dword [SMP_ACK], 5
    wait_value SMP_COMMAND, 6
    mov ecx, SMP_ITERATIONS
.contend:
    lock add qword [SMP_COUNTER], 1
    dec ecx
    jne .contend
    mov dword [SMP_ACK], 6
    wait_value SMP_COMMAND, 7
    mov rax, [ALIAS]
    test rax, rax
    jne failed
    mov dword [SMP_ACK], 7
    jmp halted

SMP_FAULTS equ expected_faults
%endif

general_protection:
    cmp qword [EXPECTED_VECTOR], 13
    jne failed
    cmp qword [rsp], 0
    jne failed
    jmp fault_with_error

page_fault:
    cmp qword [EXPECTED_VECTOR], 14
    jne failed
    cmp qword [rsp], 3
    jne failed

fault_with_error:
    cmp qword [FAULT_ARMED], 1
    jne failed
    mov r15, [EXPECTED_RIP]
    cmp [rsp + 8], r15
    jne failed
    mov r15, [RESUME_RIP]
    mov [rsp + 8], r15
    mov qword [FAULT_ARMED], 0
    inc qword [FAULT_COUNT]
    add rsp, 8
    iretq

invalid_opcode:
    cmp qword [EXPECTED_VECTOR], 6
    jne failed
    cmp qword [FAULT_ARMED], 1
    jne failed
    mov r15, [EXPECTED_RIP]
    cmp [rsp], r15
    jne failed
    mov r15, [RESUME_RIP]
    mov [rsp], r15
    mov qword [FAULT_ARMED], 0
    inc qword [FAULT_COUNT]
    iretq

unexpected_fault:
    mov rax, 0
    mov r8, 0
    mov esi, unexpected_message
    call puts
    jmp failed

failed:
    mov rax, 0
    mov r8, 0
    mov esi, failed_message
    call puts
    mov eax, 1
    out 0xf4, eax

halted:
    cli
    hlt
    jmp halted

puts:
    lodsb
    test al, al
    jz .done
    out 0xe9, al
    jmp puts
.done:
    ret

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
    dq 0x00af9a000000ffff
gdt_end:

gdt_pointer:
    dw gdt_end - gdt - 1
    dd gdt

idt_pointer:
    dw 256 * 16 - 1
    dq IDT

starting_message: db 'dead-state BIOS: long mode', 10, 0
%ifdef DEAD_SMP
passed_message: db 'dead-state BIOS: SMP PASS', 10, 0
%else
%ifdef DEAD_FEATURE_DISABLED
passed_message: db 'dead-state BIOS: disabled-feature PASS', 10, 0
%else
passed_message: db 'dead-state BIOS: PASS', 10, 0
%endif
%endif
unexpected_message: db 'dead-state BIOS: unexpected exception', 10, 0
failed_message: db 'dead-state BIOS: FAIL', 10, 0

times 0xfff0 - ($ - $$) db 0
bits 16
    jmp 0xf000:start - $$
times 0x10000 - ($ - $$) db 0
