#if defined(__i386__) || defined(__x86_64__)
        .text
        .global _breakpoint_table_entry_start
        .hidden _breakpoint_table_entry_start
_breakpoint_table_entry_start:
        ret
        .global _breakpoint_table_entry_end
        .hidden _breakpoint_table_entry_end
_breakpoint_table_entry_end:
        .rept 131071 /* SYSCALLBUF_BUFFER_SIZE/8 - 1 */
        ret
        .endr


#else
#error unknown CPU architecture
#endif /* __i386__/__x86_64__ */
        .section .note.GNU-stack,"",@progbits
        .previous
