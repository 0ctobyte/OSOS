#include <kernel/kassert.h>
#include <kernel/slab.h>
#include <kernel/kmem.h>
#include <kernel/arch/arch_asm.h>
#include <kernel/vm/vm_page.h>
#include <kernel/vm/vm_map.h>

#define VM_MAPPING_INITIALIZER (vm_mapping_t){0}

// Kernel vmap
vm_map_t kernel_vmap;

typedef struct {
    spinlock_t lock; // Lock
    slab_t slab;     // Kernel vm_mapping_t slab
    slab_buf_t slab_buf;
} vm_mapping_slab_t;

// Slab for kernel vm_mapping_t struct
vm_mapping_slab_t vm_mapping_slab;
#define NUM_KERNEL_VM_MAPPING_STRUCTS (64)

rbtree_compare_result_t _vm_mapping_overlap(rbtree_node_t *n1, rbtree_node_t *n2) {
    vm_mapping_t *m1 = rbtree_entry(n1, vm_mapping_t, rb_snode), *m2 = rbtree_entry(n2, vm_mapping_t, rb_snode);
    return (m1->vstart >= m2->vend) ? RBTREE_COMPARE_GT : (m1->vend <= m2->vstart) ? RBTREE_COMPARE_LT : RBTREE_COMPARE_EQ;
}

rbtree_compare_result_t _vm_mapping_compare(rbtree_node_t *n1, rbtree_node_t *n2) {
    vm_mapping_t *m1 = rbtree_entry(n1, vm_mapping_t, rb_snode), *m2 = rbtree_entry(n2, vm_mapping_t, rb_snode);
    return (m1->vstart > m2->vstart) ? RBTREE_COMPARE_GT : (m1->vstart < m2->vstart) ? RBTREE_COMPARE_LT : RBTREE_COMPARE_EQ;
}

rbtree_compare_result_t _vm_mapping_compare_hole(rbtree_node_t *n1, rbtree_node_t *n2) {
    vm_mapping_t *m1 = rbtree_entry(n1, vm_mapping_t, rb_hnode), *m2 = rbtree_entry(n2, vm_mapping_t, rb_hnode);
    return (m1->hole_size > m2->hole_size) ? RBTREE_COMPARE_GT : RBTREE_COMPARE_LT;
}

rbtree_compare_result_t _vm_mapping_find_hole(rbtree_node_t *n1, rbtree_node_t *n2) {
    vm_mapping_t *m1 = rbtree_entry(n1, vm_mapping_t, rb_hnode), *m2 = rbtree_entry(n2, vm_mapping_t, rb_hnode);
    return (m1->hole_size > m2->vend) ? RBTREE_COMPARE_GT : (m1->hole_size < m2->vend) ? RBTREE_COMPARE_LT : RBTREE_COMPARE_EQ;
}

void _vm_mapping_hole_update(vm_map_t *vmap, vm_mapping_t *mapping, size_t new_hole_size) {
    rbtree_remove(&vmap->rb_holes, &mapping->rb_hnode);
    mapping->hole_size = new_hole_size;
    if (new_hole_size > 0 ) kassert(rbtree_insert(&vmap->rb_holes, _vm_mapping_compare_hole, &mapping->rb_hnode));
}

void _vm_mapping_hole_insert(vm_map_t *vmap, vm_mapping_t *predecessor, vm_mapping_t *new_mapping) {
    kassert(new_mapping != NULL);

    // Update the hole size for the predecessor
    if (predecessor != NULL) _vm_mapping_hole_update(vmap, predecessor, new_mapping->vstart - predecessor->vend);

    // Assumption is that this function is called after the new mapping has been inserted into the mapping tree and list
    // So we should be able to get the successor if it exists
    vm_mapping_t *successor = list_entry(list_next(&new_mapping->ll_node), vm_mapping_t, ll_node);
    size_t new_mapping_hole_size = ((successor != NULL) ? successor->vstart : vmap->end) - new_mapping->vend;

    // Insert the new mapping into the hole tree
    new_mapping->hole_size = new_mapping_hole_size;
    if (new_mapping_hole_size > 0) kassert(rbtree_insert(&vmap->rb_holes, _vm_mapping_compare_hole, &new_mapping->rb_hnode));
}

void _vm_mapping_hole_delete(vm_map_t *vmap, vm_mapping_t *predecessor, vm_mapping_t *mapping) {
    // Calculate the hole size for the predecessor and update it
    if (predecessor != NULL) {
        size_t new_hole_size = predecessor->hole_size + (mapping->vend - mapping->vstart) + mapping->hole_size;
        _vm_mapping_hole_update(vmap, predecessor, new_hole_size);
    }

    rbtree_remove(&vmap->rb_holes, &mapping->rb_hnode);
}

vm_mapping_t* _vm_mapping_alloc(vm_map_t *vmap) {
    vm_mapping_t *mapping;

    if (vm_map_kernel() == vmap) {
        // Use the slab instead of kmem for kernel vm_mapping_t structs
        // If we're out of buffers from the slab then the kernel is in an unrecoverable state
        spinlock_acquire(&vm_mapping_slab.lock);
        mapping = (vm_mapping_t*)slab_alloc(&vm_mapping_slab.slab);
        spinlock_release(&vm_mapping_slab.lock);
        kassert(mapping != NULL);
    } else {
        mapping = (vm_mapping_t*)kmem_alloc(sizeof(vm_mapping_t));
    }

    return mapping;
}

void _vm_mapping_free(vm_map_t *vmap, vm_mapping_t *mapping) {
    if (vm_map_kernel() == vmap) {
        spinlock_acquire(&vm_mapping_slab.lock);
        slab_free(&vm_mapping_slab.slab, mapping);
        spinlock_release(&vm_mapping_slab.lock);
    } else {
        kmem_free(mapping, sizeof(vm_mapping_t));
    }
}

void _vm_mapping_insert(vm_map_t *vmap, rbtree_slot_t slot, vm_mapping_t *predecessor, vm_mapping_t *new_mapping) {
    if (slot == 0) {
        kassert(rbtree_insert(&vmap->rb_mappings, _vm_mapping_compare, &new_mapping->rb_snode));
    } else {
        kassert(rbtree_insert_slot(&vmap->rb_mappings, slot, &new_mapping->rb_snode));
    }

    kassert(list_insert_after(&vmap->ll_mappings, &predecessor->ll_node, &new_mapping->ll_node));
}

void _vm_mapping_unwire(vm_mapping_t *mapping) {
    mapping->wired = false;

    // Go through all pages in this mapping and unwire the pages
    size_t vsize = mapping->vend - mapping->vstart;
    for (vm_offset_t moffset = 0; moffset < vsize; moffset += PAGESIZE) {
        vm_offset_t offset = moffset + mapping->offset;
        vm_page_t *page = vm_page_lookup(mapping->object, offset);
        kassert(page != NULL);
        vm_page_unwire(page);
    }
}

void _vm_mapping_delete(vm_map_t *vmap, vm_mapping_t *mapping) {
    // Remove mappings from the pmap
    pmap_remove(vmap->pmap, mapping->vstart, mapping->vend);

    // Unwire the mapping if it had been wired
    if (mapping->wired) _vm_mapping_unwire(mapping);

    kassert(rbtree_remove(&vmap->rb_mappings, &mapping->rb_snode));
    kassert(list_remove(&vmap->ll_mappings, &mapping->ll_node));

    vm_object_destroy(mapping->object);

    _vm_mapping_free(vmap, mapping);
}

void _vm_mapping_enter(vm_map_t *vmap, vm_mapping_t *predecessor, vm_mapping_t *mapping, rbtree_slot_t slot) {
    size_t size = mapping->vend - mapping->vstart;

    // Check if we can merge the predecessor mapping with this new mapping
    if (predecessor != NULL && predecessor->vend == mapping->vstart && predecessor->object == mapping->object && predecessor->prot == mapping->prot && predecessor->wired == mapping->wired) {
        // Increase the size of the object
        size_t new_size = predecessor->offset + (predecessor->vend - predecessor->vstart) + size;
        vm_object_set_size(predecessor->object, new_size);

        // Increase the size of the map and the end vaddr of the mapping
        vmap->size += size;
        predecessor->vend = mapping->vend;

        // Update the predecessor's hole size and hole tree
        _vm_mapping_hole_update(vmap, predecessor, predecessor->hole_size - size);
    } else {
        vm_mapping_t *new_mapping = _vm_mapping_alloc(vmap);
        arch_fast_move((uintptr_t)new_mapping, (uintptr_t)mapping, sizeof(vm_mapping_t));
        vm_object_reference(new_mapping->object);

        // Insert the new mapping
        _vm_mapping_insert(vmap, slot, predecessor, new_mapping);

        // Update the hole tree
        _vm_mapping_hole_insert(vmap, predecessor, new_mapping);

        vmap->size += size;
    }
}

vm_mapping_t* _vm_mapping_split(vm_map_t *vmap, vm_mapping_t *mapping, vaddr_t start) {
    // Check if a split should be performed
    if (start <= mapping->vstart || start >= mapping->vend) return mapping;

    // Splits a mapping based on the given starting virtual address
    vm_mapping_t *split = _vm_mapping_alloc(vmap);
    arch_fast_move((vaddr_t)split, (vaddr_t)mapping, sizeof(vm_mapping_t));

    split->ll_node = LIST_NODE_INITIALIZER;
    split->rb_snode = RBTREE_NODE_INITIALIZER;
    split->rb_hnode = RBTREE_NODE_INITIALIZER;

    // Adjust the ending virtual address and hole size for the original mapping
    // Adjust the starting virtual address and object offset for the new mapping
    mapping->vend = start;
    split->vstart = start;
    split->offset = mapping->offset + (start - mapping->vstart);
    vm_object_reference(split->object);

    // Insert it into the various trees and lists
    _vm_mapping_insert(vmap, 0, mapping, split);
    _vm_mapping_hole_insert(vmap, mapping, split);

    return split;
}

void vm_map_init(void) {
    // Initialize the slab to allocate kernel vm_mapping_t structs from. We can't use the general
    // purpose allocators because of circular dependencies if we are out of kernel virtual memory
    size_t size = NUM_KERNEL_VM_MAPPING_STRUCTS * sizeof(vm_mapping_t);
    void *buf = (void*)pmap_steal_memory(size, NULL, NULL);
    slab_init(&vm_mapping_slab.slab, &vm_mapping_slab.slab_buf, buf, size, sizeof(vm_mapping_t));
}

vm_map_t* vm_map_create(pmap_t *pmap, vaddr_t vmin, vaddr_t vmax) {
    vm_map_t *map = (vm_map_t*)kmem_alloc(sizeof(vm_map_t));

    *map = (vm_map_t){ .lock = SPINLOCK_INIT, .rb_mappings = RBTREE_INITIALIZER, .rb_holes = RBTREE_INITIALIZER,
        .pmap = pmap, .start = vmin, .end = vmax, .size = 0, .refcnt = 0 };

    vm_map_reference(map);
    return map;
}

void vm_map_destroy(vm_map_t *vmap) {
    kassert(vmap != NULL);

    spinlock_writeacquire(&vmap->lock);
    vmap->refcnt--;

    if (vmap->refcnt == 0) {
        vm_mapping_t *mapping = NULL;
        list_for_each_entry(&vmap->ll_mappings, mapping, ll_node) {
            _vm_mapping_delete(vmap, mapping);
        }

        pmap_destroy(vmap->pmap);

        kmem_free(vmap, sizeof(vm_map_t));

        return;
    }

    spinlock_writerelease(&vmap->lock);
}

void vm_map_reference(vm_map_t *vmap) {
    spinlock_writeacquire(&vmap->lock);
    vmap->refcnt++;
    spinlock_writerelease(&vmap->lock);
}

kresult_t vm_map_enter_at(vm_map_t *vmap, vaddr_t vaddr, size_t size, vm_object_t *object, vm_offset_t offset, vm_prot_t prot) {
    kassert(vmap != NULL);

    rbtree_slot_t slot = 0;
    rbtree_node_t *predecessor_node = NULL;
    vm_mapping_t tmp = VM_MAPPING_INITIALIZER;
    tmp.vstart = vaddr;
    tmp.vend = vaddr + size;
    tmp.prot = prot;
    tmp.object = object;
    tmp.offset = offset;

    // Make sure it is within the total virtual address space
    if (tmp.vstart < vmap->start && tmp.vend > vmap->end) {
        return KRESULT_NO_SPACE;
    }

    spinlock_writeacquire(&vmap->lock);

    // Make sure the address region specified isn't already mapped or partially mapped
    if (rbtree_search(&vmap->rb_mappings, _vm_mapping_overlap, &tmp.rb_snode)) {
        spinlock_writerelease(&vmap->lock);
        return KRESULT_INVALID_ARGUMENT;
    }

    rbtree_search_predecessor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &predecessor_node, &slot);
    vm_mapping_t *predecessor = rbtree_entry(predecessor_node, vm_mapping_t, rb_snode);

    _vm_mapping_enter(vmap, predecessor, &tmp, slot);

    spinlock_writerelease(&vmap->lock);
    return KRESULT_OK;
}

kresult_t vm_map_enter(vm_map_t *vmap, vaddr_t *vaddr, size_t size, vm_object_t *object, vm_offset_t offset, vm_prot_t prot) {
    kassert(vmap != NULL);

    rbtree_slot_t slot = 0;
    rbtree_node_t *predecessor_node = NULL;
    vm_mapping_t tmp = VM_MAPPING_INITIALIZER;
    tmp.vend = size;
    tmp.prot = prot;
    tmp.object = object;
    tmp.offset = offset;

    spinlock_writeacquire(&vmap->lock);

    // Search for a hole in the virtual address space that can fit this request. vm_mapping_t hold the hole_size after the mapping which means
    // we are looking for the successor node, i.e. the node with the next biggest hole size, which is actually the predecessor node where this new mapping will be inserted
    rbtree_search_successor(&vmap->rb_holes, _vm_mapping_find_hole, &tmp.rb_hnode, &predecessor_node, &slot);
    vm_mapping_t *predecessor = rbtree_entry(predecessor_node, vm_mapping_t, rb_hnode);

    // No hole can fit this request
    if (predecessor == NULL) {
        spinlock_writerelease(&vmap->lock);
        return KRESULT_NO_SPACE;
    }

    tmp.vstart = predecessor->vend;
    tmp.vend = tmp.vstart + size;

    kassert(tmp.vstart >= vmap->start);

    // Make sure it is within the total virtual address space
    if (tmp.vend > vmap->end) return KRESULT_NO_SPACE;

    // Find the slot where this mapping will go in the mapping tree (also, it shouldn't exist in the tree)
    kassert(!rbtree_search_slot(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &slot));

    _vm_mapping_enter(vmap, predecessor, &tmp, slot);

    spinlock_writerelease(&vmap->lock);

    *vaddr = tmp.vstart;
    return KRESULT_OK;
}

kresult_t vm_map_remove(vm_map_t *vmap, vaddr_t start, vaddr_t end) {
    kassert(vmap != NULL);

    rbtree_node_t *nearest_node = NULL;
    vm_mapping_t tmp = VM_MAPPING_INITIALIZER;
    tmp.vstart = start;
    tmp.vend = end;
    tmp.prot = VM_PROT_DEFAULT;

    // Make sure it is within the total virtual address space
    if (tmp.vstart < vmap->start && tmp.vend > vmap->end) {
        return KRESULT_INVALID_ARGUMENT;
    }

    spinlock_writeacquire(&vmap->lock);

    // Search for the first mapping entry to contain the starting virtual address to be removed
    rbtree_search_predecessor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
    vm_mapping_t *nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);

    // If we can't find the previous mapping to the starting virtual address to be removed, then try finding the next mapping
    if (nearest == NULL) {
        rbtree_search_successor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
        nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);
    }

    // There's no mappings to remove
    if (nearest == NULL) {
        spinlock_writerelease(&vmap->lock);
        return KRESULT_INVALID_ARGUMENT;
    }

    // Iterate through mappings. One of two cases may occur; either the mapping is contained completely within the range to be removed
    // Or, the mapping partly lies in the range. The first case is easy, remove it from the various trees and lists, decrement the associated
    // vm_object refcnt and free the mapping entry structure. For the second case, we must first split the mapping entry in two at the intersection
    // which can either happen at the start or end of the region or both.
    for (vm_mapping_t *mapping = _vm_mapping_split(vmap, nearest, start); !list_end(mapping) && mapping->vstart < end; ) {
        _vm_mapping_split(vmap, mapping, end);

        vm_mapping_t *next = list_entry(list_next(&mapping->ll_node), vm_mapping_t, ll_node);

        if (mapping->vend > start) {
            // Remove from the various trees and lists; update the hole tree
            vm_mapping_t *prev = list_entry(list_prev(&mapping->ll_node), vm_mapping_t, ll_node);
            _vm_mapping_hole_delete(vmap, prev, mapping);
            _vm_mapping_delete(vmap, mapping);
            // FIXME what happens when object refcnt drops to zero?
        }

        mapping = next;
    }

    spinlock_writerelease(&vmap->lock);
    return KRESULT_OK;
}

kresult_t vm_map_protect(vm_map_t *vmap, vaddr_t start, vaddr_t end, vm_prot_t new_prot) {
    kassert(vmap != NULL);

    rbtree_node_t *nearest_node = NULL;
    vm_mapping_t tmp = VM_MAPPING_INITIALIZER;
    tmp.vstart = start;
    tmp.vend = end;
    tmp.prot = VM_PROT_DEFAULT;

    // Make sure it is within the total virtual address space
    if (tmp.vstart < vmap->start && tmp.vend > vmap->end) {
        return KRESULT_INVALID_ARGUMENT;
    }

    spinlock_writeacquire(&vmap->lock);

    // Search for the first mapping entry to contain the starting virtual address of the region specified
    rbtree_search_predecessor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
    vm_mapping_t *nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);

    // If we can't find the previous mapping to the starting virtual address to be removed, then try finding the next mapping
    if (nearest == NULL) {
        rbtree_search_successor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
        nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);
    }

    // There's no mappings to protect
    if (nearest == NULL) {
        spinlock_writerelease(&vmap->lock);
        return KRESULT_INVALID_ARGUMENT;
    }

    // Iterate through mappings and update protections. Make sure to split if start or end intersects a mapping
    nearest = (nearest->prot != new_prot) ? _vm_mapping_split(vmap, nearest, start) : nearest;
    for (vm_mapping_t *mapping = nearest; !list_end(mapping) && mapping->vstart < end; ) {
        if (mapping->prot != new_prot) _vm_mapping_split(vmap, mapping, end);

        vm_mapping_t *next = list_entry(list_next(&mapping->ll_node), vm_mapping_t, ll_node);

        if (mapping->prot != new_prot && mapping->vend > start) {
            mapping->prot = new_prot;
            pmap_protect(vmap->pmap, mapping->vstart, mapping->vend, new_prot);
        }

        mapping = next;
    }

    spinlock_writerelease(&vmap->lock);
    return KRESULT_OK;
}

kresult_t vm_map_wire(vm_map_t *vmap, vaddr_t start, vaddr_t end) {
    kassert(vmap != NULL);

    rbtree_node_t *nearest_node = NULL;
    vm_mapping_t tmp = VM_MAPPING_INITIALIZER;
    tmp.vstart = start;
    tmp.vend = end;
    tmp.prot = VM_PROT_DEFAULT;

    // Make sure it is within the total virtual address space
    if (tmp.vstart < vmap->start && tmp.vend > vmap->end) {
        return KRESULT_INVALID_ARGUMENT;
    }

    spinlock_writeacquire(&vmap->lock);

    // Search for the first mapping entry to contain the starting virtual address of the region specified
    rbtree_search_predecessor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
    vm_mapping_t *nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);

    // If we can't find the previous mapping to the starting virtual address to be removed, then try finding the next mapping
    if (nearest == NULL) {
        rbtree_search_successor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
        nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);
    }

    // There's no mappings to wire
    if (nearest == NULL) {
        spinlock_writerelease(&vmap->lock);
        return KRESULT_INVALID_ARGUMENT;
    }

    // Iterate through mappings and wire the pages. Make sure to split if start or end intersects a mapping
    nearest = !nearest->wired ? _vm_mapping_split(vmap, nearest, start) : nearest;
    for (vm_mapping_t *mapping = nearest; !list_end(mapping) && mapping->vstart < end; ) {
        if (!mapping->wired) _vm_mapping_split(vmap, mapping, end);

        vm_mapping_t *next = list_entry(list_next(&mapping->ll_node), vm_mapping_t, ll_node);

        if (!mapping->wired) {
            mapping->wired = true;

            // Go through all pages in this mapping and wire down pages in the pmap
            size_t vsize = mapping->vend - mapping->vstart;
            for (vm_offset_t moffset = 0; moffset < vsize; moffset += PAGESIZE) {
                vm_offset_t offset = moffset + mapping->offset;
                vm_page_t *page = vm_page_lookup(mapping->object, offset);

                if (page == NULL) {
                    page = vm_page_alloc(mapping->object, offset);
                    kassert(page != NULL);
                    pmap_enter(vmap->pmap, moffset + mapping->vstart, vm_page_to_pa(page), mapping->prot, PMAP_FLAGS_WIRED);
                }

                vm_page_wire(page);
            }
        }

        mapping = next;
    }

    spinlock_writerelease(&vmap->lock);
    return KRESULT_OK;
}

kresult_t vm_map_unwire(vm_map_t *vmap, vaddr_t start, vaddr_t end) {
    kassert(vmap != NULL);

    rbtree_node_t *nearest_node = NULL;
    vm_mapping_t tmp = VM_MAPPING_INITIALIZER;
    tmp.vstart = start;
    tmp.vend = end;
    tmp.prot = VM_PROT_DEFAULT;

    // Make sure it is within the total virtual address space
    if (tmp.vstart < vmap->start && tmp.vend > vmap->end) {
        return KRESULT_INVALID_ARGUMENT;
    }

    spinlock_writeacquire(&vmap->lock);

    // Search for the first mapping entry to contain the starting virtual address of the region specified
    rbtree_search_predecessor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
    vm_mapping_t *nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);

    // If we can't find the previous mapping to the starting virtual address to be removed, then try finding the next mapping
    if (nearest == NULL) {
        rbtree_search_successor(&vmap->rb_mappings, _vm_mapping_compare, &tmp.rb_snode, &nearest_node, NULL);
        nearest = rbtree_entry(nearest_node, vm_mapping_t, rb_snode);
    }

    // There's no mappings to unwire
    if (nearest == NULL) {
        spinlock_writerelease(&vmap->lock);
        return KRESULT_INVALID_ARGUMENT;
    }

    // Iterate through mappings and unwire the pages
    for (vm_mapping_t *mapping = nearest; !list_end(mapping) && mapping->vstart < end; ) {
        vm_mapping_t *next = list_entry(list_next(&mapping->ll_node), vm_mapping_t, ll_node);

        // FIXME Try to merge this unwired mapping with surrounding mappings
        if (mapping->wired) _vm_mapping_unwire(mapping);

        mapping = next;
    }

    spinlock_writerelease(&vmap->lock);
    return KRESULT_OK;
}

kresult_t vm_map_lookup(vm_map_t *vmap, vaddr_t vaddr, vm_prot_t fault_type, vm_object_t *object, vm_offset_t *offset, vm_prot_t *prot) {
}