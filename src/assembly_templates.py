import StringIO
import sys

class RawBytes(object):
    """A sequence of literal bytes to appear in an assembly language template."""
    def __init__(self, *bytes):
        self.bytes = bytes

    def __len__(self):
        return len(self.bytes)

class Field(object):
    """A variable field of bytes."""
    def __init__(self, name, byte_length):
        self.name = name
        self.byte_length = byte_length

    def __len__(self):
        return self.byte_length

    def c_type(self):
        types = { 8: 'uint64_t', 4: 'uint32_t', 2: 'uint16_t', 1: 'uint8_t' }
        return types[self.byte_length]

class AssemblyTemplate(object):
    """A sequence of RawBytes and Field objects, which can be used to verify
    that a given sequence of assembly instructions matches the RawBytes while
    pulling out the Field values for inspection.  Or for creating custom
    assembly stubs, filling out Fields with runtime-determined values."""
    def __init__(self, *chunks):
        # Merge consecutive RawBytes elements together for efficiency of
        # matching and for simplicity of template expansion.
        merged_chunks = []
        current_raw_bytes = []
        for c in chunks:
            if isinstance(c, Field):
                # Push any raw bytes before this.
                if current_raw_bytes:
                    merged_chunks.append(RawBytes(*current_raw_bytes))
                    current_raw_bytes = []
                merged_chunks.append(c)
            else:
                current_raw_bytes.extend(c.bytes)
        # Merge in trailing raw bytes.
        if current_raw_bytes:
            merged_chunks.append(RawBytes(*current_raw_bytes))
        self.chunks = merged_chunks

    def fields(self):
        return [c for c in self.chunks if isinstance(c, Field)]

    def bytes(self):
        bytes = []
        for c in self.chunks:
            if isinstance(c, Field):
                bytes.extend([0] * len(c))
            else:
                bytes.extend(c.bytes)
        return bytes

templates = {
    'X86SysenterVsyscallImplementation': AssemblyTemplate(
        RawBytes(0x51),         # push %ecx
        RawBytes(0x52),         # push %edx
        RawBytes(0x55),         # push %ebp
        RawBytes(0x89, 0xe5),   # mov %esp,%ebp
        RawBytes(0x0f, 0x34),   # sysenter
    ),
    'X86SysenterVsyscallUseInt80': AssemblyTemplate(
        RawBytes(0xcd, 0x80),   # int $0x80
        RawBytes(0xc3),         # ret
    ),
    'X86SysenterVsyscallSyscallHook': AssemblyTemplate(
        RawBytes(0xe9),         # jmp $syscall_hook_trampoline
        Field('syscall_hook_trampoline', 4),
    ),
    'X86VsyscallMonkeypatch': AssemblyTemplate(
        RawBytes(0x53),         # push %ebx
        RawBytes(0xb8),         # mov $syscall_number,%eax
        Field('syscall_number', 4),
        # __vdso functions use the C calling convention, so
        # we have to set up the syscall parameters here.
        # No x86-32 __vdso functions take more than two parameters.
        RawBytes(0x8b, 0x5c, 0x24, 0x08), # mov 0x8(%esp),%ebx
        RawBytes(0x8b, 0x4c, 0x24, 0x0c), # mov 0xc(%esp),%ecx
        RawBytes(0xcd, 0x80),   # int $0x80
        # pad with NOPs to make room to dynamically patch the syscall
        # with a call to the preload library, once syscall buffering
        # has been initialized.
        RawBytes(0x90),         # nop
        RawBytes(0x90),         # nop
        RawBytes(0x90),         # nop
        RawBytes(0x5b),         # pop %ebx
        RawBytes(0xc3),         # ret
    ),
    'X86SyscallStubExtendedJump': AssemblyTemplate(
        RawBytes(0xe9), # jmp
        Field('relative_jump_target', 4),
    ),
    'X86SyscallStubMonkeypatch': AssemblyTemplate(
        # This code must match the stubs in syscall_hook.S.
        # We must adjust the stack pointer without modifying flags,
        # at least on the return path.
        RawBytes(0xc7, 0x84, 0x24, 0x00, 0xff, 0xff, 0xff), # movq $fake_return_addr,-256(%esp)
        Field('fake_return_addr', 4),
        RawBytes(0x89, 0xa4, 0x24, 0x04, 0xff, 0xff, 0xff), # mov %esp,-252(%esp)
        RawBytes(0x8d, 0xa4, 0x24, 0x00, 0xff, 0xff, 0xff), # lea -256(%esp),%esp
        RawBytes(0xe8),         # call $trampoline_relative_addr
        Field('trampoline_relative_addr', 4),
        RawBytes(0x8d, 0xa4, 0x24, 0x00, 0x01, 0x00, 0x00), # lea 256(%esp),%esp
        RawBytes(0xff, 0xa4, 0x24, 0x00, 0xff, 0xff, 0xff), # jmp -256(%esp)
    ),

    'X64JumpMonkeypatch': AssemblyTemplate(
        RawBytes(0xe9),         # jmp $relative_addr
        Field('relative_addr', 4),
    ),
    'X64VsyscallMonkeypatch': AssemblyTemplate(
        RawBytes(0xb8),         # mov $syscall_number,%eax
        Field('syscall_number', 4),
        RawBytes(0x0f, 0x05),   # syscall
        # pad with NOPs to make room to dynamically patch the syscall
        # with a call to the preload library, once syscall buffering
        # has been initialized.
        RawBytes(0x90),         # nop
        RawBytes(0x90),         # nop
        RawBytes(0x90),         # nop
        RawBytes(0xc3),         # ret
    ),
    'X64SyscallStubExtendedJump': AssemblyTemplate(
        RawBytes(0xff, 0x25, 0x00, 0x00, 0x00, 0x00), # jmp *0(%rip)
        Field('jump_target', 8),
    ),
    'X64SyscallStubMonkeypatch': AssemblyTemplate(
        # This code must match the stubs in syscall_hook.S.
        # We must adjust the stack pointer without modifying flags,
        # at least on the return path.
        RawBytes(0xc7, 0x84, 0x24, 0x00, 0xff, 0xff, 0xff), # movl $return_addr_lo,-256(%rsp)
        Field('return_addr_lo', 4),
        RawBytes(0xc7, 0x84, 0x24, 0x04, 0xff, 0xff, 0xff), # movl $return_addr_hi,-252(%rsp)
        Field('return_addr_hi', 4),
        RawBytes(0x48, 0x89, 0xa4, 0x24, 0x08, 0xff, 0xff, 0xff), # mov %rsp,-248(%rsp)
        RawBytes(0x48, 0x8d, 0xa4, 0x24, 0x00, 0xff, 0xff, 0xff), # lea -256(%rsp),%rsp
        RawBytes(0xe8),         # call $trampoline_relative_addr
        Field('trampoline_relative_addr', 4),
        RawBytes(0x48, 0x8d, 0xa4, 0x24, 0x00, 0x01, 0x00, 0x00), # lea 256(%rsp),%rsp
        RawBytes(0xff, 0xa4, 0x24, 0x00, 0xff, 0xff, 0xff), # jmp -256(%rsp)
    ),
}

def byte_array_name(name):
    return '%s_bytes' % name

def generate_match_method(byte_array, template):
    s = StringIO.StringIO()
    fields = template.fields()
    field_types = [f.c_type() for f in fields]
    field_names = [f.name for f in fields]
    args = ', ' + ', '.join("%s* %s" % (t, n) for t, n in zip(field_types, field_names)) \
           if fields else ''
    
    s.write('static bool match(const uint8_t* buffer %s) {\n' % (args,))
    offset = 0
    for chunk in template.chunks:
        if isinstance(chunk, Field):
            field_name = chunk.name
            s.write('    memcpy(%s, &buffer[%d], sizeof(*%s));\n'
                    % (field_name, offset, field_name))
        else:
            s.write('    if (memcmp(&buffer[%d], &%s[%d], %d) != 0) { return false; }\n'
                    % (offset, byte_array, offset, len(chunk)))
        offset += len(chunk)
    s.write('    return true;\n')
    s.write('  }')
    return s.getvalue()

def generate_substitute_method(byte_array, template):
    s = StringIO.StringIO()
    fields = template.fields()
    field_types = [f.c_type() for f in fields]
    field_names = [f.name for f in fields]
    args = ', ' + ', '.join("%s %s" % (t, n) for t, n in zip(field_types, field_names)) \
           if fields else ''
    
    s.write('static void substitute(uint8_t* buffer %s) {\n' % (args,))
    offset = 0
    for chunk in template.chunks:
        if isinstance(chunk, Field):
            field_name = chunk.name
            s.write('    memcpy(&buffer[%d], &%s, sizeof(%s));\n'
                    % (offset, field_name, field_name))
        else:
            s.write('    memcpy(&buffer[%d], &%s[%d], %d);\n'
                    % (offset, byte_array, offset, len(chunk)))
        offset += len(chunk)
    s.write('  }')
    return s.getvalue()

def generate_size_member(byte_array):
    s = StringIO.StringIO()
    s.write('static const size_t size = sizeof(%s);' % byte_array)
    return s.getvalue()

def generate(f):
    # Raw bytes.
    for name, template in templates.iteritems():
        bytes = template.bytes()
        f.write('static const uint8_t %s[] = { %s };\n'
                % (byte_array_name(name), ', '.join(['0x%x' % b for b in bytes])))
    f.write('\n')

    # Objects representing assembly templates.
    for name, template in templates.iteritems():
        byte_array = byte_array_name(name)
        f.write("""class %(class_name)s {
public:
  %(match_method)s

  %(substitute_method)s

  %(size_member)s
};
""" % { 'class_name': name,
        'match_method': generate_match_method(byte_array, template),
        'substitute_method': generate_substitute_method(byte_array, template),
        'size_member': generate_size_member(byte_array), })
        f.write('\n\n')
