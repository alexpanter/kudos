/*
 * Process startup.
 */

#include <arch.h>
#include "proc/process.h"
#include "proc/elf.h"
#include "kernel/thread.h"
#include "kernel/assert.h"
#include "kernel/interrupt.h"
#include "kernel/config.h"
#include "fs/vfs.h"
#include "kernel/sleepq.h"
#include "vm/memory.h"


/** @name Process startup
 *
 * This module contains functions for starting and managing userland
 * processes.
 */
extern void process_set_pagetable(pagetable_t*);

/* Return non-zero on error. */
int setup_new_process(TID_t thread,
                      const char *executable, const char **argv_src,
                      virtaddr_t *entry_point, virtaddr_t *stack_top)
{
  pagetable_t *pagetable;
  elf_info_t elf;
  openfile_t file;
  uintptr_t phys_page;
  virtaddr_t virt_page;
  int i, res;
  interrupt_status_t intr_status;
  TID_t current_thread = thread_get_current_thread();
  pml4_t *current_pml4;
  thread_table_t *thread_entry = thread_get_thread_entry(thread);

  //Save the currently running threads page table
  current_pml4 = thread_get_thread_entry(current_thread)
      ->context->virt_memory;

  file = vfs_open((char *)executable);

  /* Make sure the file existed and was a valid ELF file */
  if (file < 0) {
    return -1;
  }

  res = elf_parse_header(&elf, file);
  if (res < 0) {
    return -1;
  }

  /* Trivial and naive sanity check for entry point: */
  if (elf.entry_point <= VMM_KERNEL_SPACE) {
    return -1;
  }

  *entry_point = elf.entry_point;

  //Create new page table
  pagetable = vm_create_pagetable(thread);

  //We don't want to be interrupted when we potentially
  //is running with the wrong page tables mapped in ;)
  intr_status = _interrupt_disable();

  //Change the page table of the currently running thread
  process_set_pagetable(pagetable);

  thread_entry->pagetable = pagetable;

  /* Allocate and map stack */
  for(i = 0; i < CONFIG_USERLAND_STACK_SIZE; i++) {
    phys_page = physmem_allocblock();
    KERNEL_ASSERT(phys_page != 0);
    virt_page = (USERLAND_STACK_TOP & PAGE_SIZE_MASK) - i*PAGE_SIZE;
    vm_map(pagetable, phys_page,
           virt_page, PAGE_USER | PAGE_WRITE);
    /* Zero the page */
    memoryset((void*)virt_page, 0, PAGE_SIZE);
  }

  /* Allocate and map pages for the ELF segments. We assume that
     the segments begin at a page boundary. (The linker script
     in the userland directory helps users get this right.) */
  for(i = 0; i < (int)elf.ro_pages; i++) {
    int left_to_read = elf.ro_size - i*PAGE_SIZE;
    phys_page = physmem_allocblock();
    KERNEL_ASSERT(phys_page != 0);
    virt_page = elf.ro_vaddr + i*PAGE_SIZE;
    vm_map(pagetable, phys_page,
           virt_page, PAGE_USER | PAGE_WRITE);
    /* Zero the page */
    memoryset((void*)virt_page, 0, PAGE_SIZE);
    /* Fill the page from ro segment */
    if (left_to_read > 0) {
      KERNEL_ASSERT(vfs_seek(file, elf.ro_location + i*PAGE_SIZE) == VFS_OK);
      KERNEL_ASSERT(vfs_read(file, virt_page,
                             MIN(PAGE_SIZE, left_to_read))
                    == (int) MIN(PAGE_SIZE, left_to_read));
    }
    //Make the page read only
    vm_map(pagetable, phys_page,
            virt_page, PAGE_USER);
  }

  for(i = 0; i < (int)elf.rw_pages; i++) {
    int left_to_read = elf.rw_size - i*PAGE_SIZE;
    phys_page = physmem_allocblock();
    KERNEL_ASSERT(phys_page != 0);
    virt_page = elf.rw_vaddr + i*PAGE_SIZE;
    vm_map(pagetable, phys_page,
           virt_page, PAGE_USER | PAGE_WRITE);
    /* Zero the page */
    memoryset((void*)virt_page, 0, PAGE_SIZE);
    /* Fill the page from rw segment */
    if (left_to_read > 0) {
      KERNEL_ASSERT(vfs_seek(file, elf.rw_location + i*PAGE_SIZE) == VFS_OK);
      KERNEL_ASSERT(vfs_read(file, (void*)virt_page,
                             MIN(PAGE_SIZE, left_to_read))
                    == (int) MIN(PAGE_SIZE, left_to_read));
    }
  }

  /* Done with the file. */
  vfs_close(file);

  *stack_top = USERLAND_STACK_TOP;

  /* We restore the currently running threads page table
   * here. And saves the new page table to the new threads
   * context
   */
  //restore
  process_set_pagetable(current_pml4);

  //save new page table to new threads context
  thread_entry->context->pml4 = (uintptr_t)pagetable;
  thread_entry->context->virt_memory = pagetable;

  //enable interrupts
  _interrupt_set_state(intr_status);


  return 0;
}

void process_start(const char *executable, const char **argv)
{
  TID_t my_thread;
  virtaddr_t entry_point;
  int ret;
  context_t user_context;
  virtaddr_t stack_top;

  my_thread = thread_get_current_thread();
  ret = setup_new_process(my_thread, executable, argv,
                          &entry_point, &stack_top);

  if (ret != 0) {
    return; /* Something went wrong. */
  }

  /* Initialize the user context. (Status register is handled by
     thread_goto_userland) */
  memoryset(&user_context, 0, sizeof(user_context));

  _context_set_ip(&user_context, entry_point);
  _context_set_sp(&user_context, stack_top);

  /* Switch pagetables */
  vmm_setcr3(thread_get_thread_entry(my_thread)->context->pml4);

  thread_goto_userland(&user_context);
}
