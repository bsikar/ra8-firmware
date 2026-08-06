    .syntax unified
    .text
    .align 4

    .section .app_header, "ax"
    .global __ra8_app_header

__ra8_app_header:
    .dc.l 0x52413841          // magic: k_ra8_app_magic ("RA8A")
    .dc.l 1                   // version
    .dc.l 0x7F                // capabilities: k_ra8_app_cap_lifecycle | others
    .dc.l 0                   // min_fw_version
    .dc.l __code_size__       // code_size
    .dc.l __data_size__       // data_size
    .dc.l __bss_size__        // bss_size
    .dc.l 1024                // stack_size
    .dc.l __data_end__        // sig_offset (total binary size before signature)
    .dc.l 0                   // reserved
    .dc.l 0                   // reserved
    .dc.l 0                   // reserved

    .section .preamble, "ax"
    .global __txm_module_preamble
    .global demo_module_start
    .global _txm_module_thread_shell_entry
    .global _txm_module_callback_request_thread_entry

__txm_module_preamble:
    .dc.l      0x4D4F4455                                       // Module ID
    .dc.l      0x6                                              // Module Major Version
    .dc.l      0x1                                              // Module Minor Version
    .dc.l      32                                               // Module Preamble Size in 32-bit words
    .dc.l      0x12345678                                       // Module ID (application defined)
    .dc.l      0x02000003                                       // Module Properties: Bit 1 (MPU), Bit 0 (User mode)
    .dc.l      _txm_module_thread_shell_entry - . - 0           // Module Shell Entry Point
    .dc.l      demo_module_start - . - 0                        // Module Start Thread Entry Point
    .dc.l      0                                                // Module Stop Thread Entry Point
    .dc.l      1                                                // Priority
    .dc.l      1024                                             // Stack size
    .dc.l      _txm_module_callback_request_thread_entry - . - 0 // Callback Entry
    .dc.l      1                                                // Callback Priority
    .dc.l      1024                                             // Callback Stack Size
    .dc.l      __code_size__                                    // Code Size
    .dc.l      __data_size__                                    // Data Size
    .dc.l      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0                  // Reserved (16 words)
