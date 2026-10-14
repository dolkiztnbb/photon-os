#ifndef __PHT_SETUP_DONE__
#define __PHT_SETUP_DONE__

// PhotonOS compilation flags

// Version (1.0.0)
#define __PHT_KV_HIGH__ 1 // Major changes, inclusion of incompatablility is possible
#define __PHT_KV_MID__  0 // Few kernel additions, functions
#define __PHT_KV_LOW__  0 // Hotfixes

// Debug/Release version control
#define __PHT_DEBUG__ // __PHT_DEBUG__, __PHT_RELEASE__

// Graphics inclusion
#define __PHT_TEXTMODE__ // __PHT_TEXTMODE__, __PHT_AMPHIBIOUS__

// Colorful textmode support
#define __PHT_COLOR__ // __PHT_COLOR__, __PHT_NOCOLOR__

// Language
#define __PHT_LOC_EN__

// ============================================================
// MEMORY ADDRESS CONFIGURATION
// ============================================================
// Total addressable: 0x800000 - 0x1000000 (8 MB usable)

// AHCI buffers
#define __PHT_MEM_AHCI_BASE     0x400000
#define __PHT_MEM_AHCI_CMD_LIST 0x400000  // Command list table
#define __PHT_MEM_AHCI_FIS      0x401000  // FIS buffer
#define __PHT_MEM_AHCI_CMDTBL   0x402000  // Command tables (32 slots × 4KB)
#define __PHT_MEM_AHCI_BUFFER   0x408000  // Read/write buffer
#define __PHT_MEM_AHCI_END      0x4f0000

// Malloc region
#define __PHT_MEM_MALLOC_TABLE  __PHT_MEM_AHCI_END      // 0x4F0000
#define __PHT_MEM_MALLOC_POOL   0x600000                 // 6 MB mark
#define __PHT_MEM_MALLOC_END    0xE00000                 // 13 MB mark

// File handles
#define __PHT_MEM_HANDLES        __PHT_MEM_MALLOC_END
#define __PHT_MEM_HANDLES_END   0x1000000

// User space
#define __PHT_MEM_USERSPACE     __PHT_MEM_HANDLES_END
#define __PHT_MEM_END           0x10000000

// ============================================================

// Preprocessor and defaults
#if defined(__PHT_RELEASE__) && defined(__PHT_DEBUG__)
    #error "Cannot define both __PHT_RELEASE__ and __PHT_DEBUG__"
#endif

#if !defined(__PHT_RELEASE__) && !defined(__PHT_DEBUG__)
    #define __PHT_DEBUG__
#endif

#if !defined(__PHT_COLOR__) && !defined(__PHT_NOCOLOR__)
    #define __PHT_NOCOLOR__
#endif

#if !defined(__PHT_TEXTMODE__)
    #define __PHT_AMPHIBIOUS__
#else
    extern char* font;
#endif


#endif // __PHT_SETUP_DONE__
