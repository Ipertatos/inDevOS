#include "pmm.h"

#define BLOCK_SIZE 4096 //4KB block size

static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};


static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
}; 

uint64_t memmap_entry_cnt = 0;// number of entries in the memory map
uint64_t pmm_usable_addr = 0;// first usable address
uint64_t pmm_usable_top = 0;//top of the first usable address
uint64_t pmm_free_blocks =0;//number of free blocks
uint64_t pmm_block_count = 0;//number of blocks
uint64_t pmm_mem_size =0;//size of the memory in bytes
uint8_t *pmm_bitmap = 0;//bitmap of the memory

struct limine_memmap_response *memmap;
uint64_t hhdmoffset;

void mmap_set(int bit){
    pmm_bitmap[bit/8] |= (1 << (bit % 8));
}

bool mmap_test(int bit){
    return pmm_bitmap[bit/8] & (1 << (bit % 8));
} 

void mmap_unset(int bit){
    pmm_bitmap[bit/8] &= ~(1 << (bit % 8));
}

uint64_t addr2block(uint64_t addr){
    return addr/BLOCK_SIZE;
}

uint64_t block2addr(uint64_t block){
    return block*BLOCK_SIZE;
}

uint64_t find_free_block(void){
    for(uint64_t i = 0; i < pmm_block_count/8; i++){
        if(pmm_bitmap[i] != 0xFF){
            for(uint64_t j = 0; j < 8; j++){
                int bit = 1 << j;
                if(!(pmm_bitmap[i] & bit))
                    return i*8+j;
            }
        }
    }
    return -1;
}

uint64_t *pmm_alloc_block(){
    uint64_t block = find_free_block();
    if(block == -1){
        return NULL;
    }

    mmap_set(block);
    return (uint64_t *)(block * BLOCK_SIZE);
}

void pmm_free_block(uint64_t addr){
    mmap_unset(addr2block(addr));
}
void pmm_init(){
    
    hhdmoffset = hhdm_request.response->offset;
    if(!hhdmoffset){
        while(1) asm("hlt");
    }
    if(memmap_request.response ==NULL)
    {
        //failed to get memory map
        fillrect(0xff0000,0,0,800,600);
    }
    memmap = memmap_request.response;
    struct limine_memmap_entry **entries = memmap->entries;
    memmap_entry_cnt = memmap->entry_count;

    for(uint64_t i = 0; i < memmap_entry_cnt; i++){
        switch (entries[i]->type)
        {
        case LIMINE_MEMMAP_USABLE:
                pmm_mem_size += entries[i]->length;
            break;
        }
    }
    int usable = 0;

    for(uint64_t i = 0; i < memmap_entry_cnt; i++){
        switch (entries[i]->type)
        {
        case LIMINE_MEMMAP_USABLE:
            if(usable == 0){
                pmm_usable_addr = entries[i]->base;
                pmm_usable_top = pmm_usable_addr + entries[i]->length;
                usable = 1;
            }
            break;
        default:
            break;
        };
    }
    pmm_free_blocks = (pmm_usable_top/BLOCK_SIZE)*8;
    pmm_block_count = pmm_mem_size/BLOCK_SIZE;

    pmm_bitmap = (uint8_t *)(pmm_usable_addr + hhdmoffset);
    memset(pmm_bitmap,0xff,pmm_block_count/8);

    for(uint64_t i = 0; i < memmap_entry_cnt; i++){
        switch (entries[i]->type)
        {
        case LIMINE_MEMMAP_USABLE:
            for(uint64_t j = 0; j < entries[i]->length ; j+=BLOCK_SIZE){
                mmap_unset(addr2block((entries[i]->base+j)));
            }
        }
    }
    for(uint64_t i = 0; i < pmm_block_count/8; i++){
        mmap_set(addr2block(pmm_usable_addr)+i);
    }
}

uint64_t pmm_fetch_mem(){
    return pmm_mem_size;
}