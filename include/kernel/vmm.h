#ifndef __VMM_H__
#define __VMM_H__

#include <sys/types.h>

#include <kernel/pmap.h>
#include <kernel/spinlock.h>

// Opaque types
typedef struct vm_anon vm_anon_t;
typedef struct vm_amap vm_amap_t;

struct vm_anon {
    // The page associated with this anon
    vmm_page_t *page;

    // How many amaps pointing to this anon?
    long refcount;
};

struct vm_amap {
    // An array of possible anon references associated with this anonymous map
    vm_anon_t **aslots;

    // How many arefs pointing to this amap
    long refcount;

    // The max number of 'slots' (or elements) in the anon array
    unsigned long maxslots;

    // The number of slots currently occupied
    unsigned long nslots;
};

typedef struct {
    // A pointer to an amap and the slot offset within the anon array of that amap where this memory region begins
    vm_amap_t *amap;
    unsigned long slotoff;
} vm_aref_t;

typedef struct vregion {
    struct vregion *next;

    // The start and end addresses of this virtual memory region
    vaddr_t vstart, vend;

    // The region's attributes
    vmm_prot_t vm_prot;

    // The anonymous memory associated with this region
    vm_aref_t aref;

    bool needs_copy, copy_on_write;
    char padding[6];
} vregion_t;

typedef struct {
    // Multiple readers, single writer lock
    spinlock_t lock;

    // The pmap associated with this vmap
    pmap_t *pmap;

    // A list of contiguous virtual memory regions associated with this virtual memory space
    vregion_t *regions;

    // Main regions start and end virtual addresses
    vaddr_t text_start, text_end;
    vaddr_t data_start, data_end;
    vaddr_t heap_start, heap_end;
    vaddr_t stack_start;
} vmap_t;

// Declare the kernel's vmap
extern vmap_t kernel_vmap;

// Initialize the VMM system and initializes the kernel's vmap
void vmm_init(void);

// Initial kernel memory allocator used by vmm before the heap is initialized
vaddr_t vmm_km_zalloc(size_t size);

// Initializes the kernel heap region
void vmm_km_heap_init(void);

// Extends by size bytes (rounded to a page) the kernel heap region specified
// Returns the address to the start of the block of the specified size
vaddr_t vmm_km_heap_extend(size_t size);

// Returns a reference to the kernel's vmap
#define vmap_kernel() (&(kernel_vmap))

#endif // __VMM_H__

