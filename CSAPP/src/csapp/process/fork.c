#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../headers/cpu.h"
#include "../headers/interrupt.h"
#include "../headers/memory.h"
#include "../headers/syscall.h"
#include "../headers/process.h"

// from page fault
int copy_physicalframe(pte4_t *child_pte, uint64_t parent_ppn);
int enough_frame(int request_num);

static pcb_t* fork_naive_copy(pcb_t *parent_pcb);
static pcb_t* fork_cow(pcb_t *parent_pcb);

#define DUSE_FORK_NATIVE_COPY
uint64_t syscall_fork() {
  pcb_t *parent = get_current_pcb();
  pcb_t *child = NULL;

#if defined(DUSE_FORK_NATIVE_COPY)
  child = fork_naive_copy(parent);
#elif defined(DUSE_FORK_COW)
  child = fork_cow(parent);
#endif
  if (child == NULL) {
    return 0;
  }
  return 1;
}

// Implementaion

static uint64_t get_newpid() {
  pcb_t *p = get_current_pcb();
  pcb_t *x = p;
  uint64_t max_pid = p->pid;
  // 1<->2<->3<->[4]<->5<->6<->[7]<->1 4 is parent, 7 is child
  while (x->next != p) {
    max_pid = max_pid < x->pid ? x->pid : max_pid;
    x = x->next;
  }
  return max_pid + 1;
}

// Update rax register in user frame
void update_userframe_returnvalue(pcb_t *p, uint64_t retval) {
  p->context.regs.rax = retval;
  uint64_t uf_vaddr = (uint64_t)p->kstack + KERNEL_STACK_SIZE -
                      sizeof(trapframe_t) - sizeof(userframe_t);
  userframe_t *uf = (userframe_t *)uf_vaddr;
  uf->regs.rax = retval;
}

static pte123_t *copy_pagetable(pte123_t *src, int level) {
  // allocate one page for destination
  pte123_t *dst = (pte123_t *)&heap[KERNEL_malloc(sizeof(pte123_t) * PAGE_TABLE_ENTRY_NUM)];

  // copy the current page table to destination
  memcpy(dst, src, sizeof(pte123_t) * PAGE_TABLE_ENTRY_NUM);

  if (level == 4) {
    return dst;
  }

  // check source
  for (int i = 0; i < PAGE_TABLE_ENTRY_NUM; ++i) {
    if (src[i].present == 1) {
      pte123_t *src_next = (pte123_t *)(uint64_t)(src[i].paddr);
      dst[i].paddr = (uint64_t)copy_pagetable(src_next, level + 1);
    }
  }

  return dst;
}

static void copy_userframes(pte123_t *src, pte123_t *dst, int level) {
  if (level == 4) {
    pte4_t *parent_pt = (pte4_t *)src;
    pte4_t *child_pt = (pte4_t *)dst;

    // copy user frames here
    for (int j = 0; j < PAGE_TABLE_ENTRY_NUM; ++j) {
      if (parent_pt[j].present == 1) {
        // copy the physical frame to child
        int copy_ = copy_physicalframe(&child_pt[j], parent_pt[j].ppn);
        assert(copy_ == 1);
      }
    }

    return;
  }

  // DFS to go down
  for (int i = 0;i < PAGE_TABLE_ENTRY_NUM;++i) {
    if (src[i].present == 1) {
      pte123_t *src_next = (pte123_t *)(uint64_t)(src[i].paddr);
      pte123_t *dst_next = (pte123_t *)(uint64_t)(dst[i].paddr);
      copy_userframes(src_next, dst_next, level+1);
    }
  }
}

static int get_childframe_num(pte123_t *p, int level) {
  int count = 0;

  if (level == 4) {
    for (int i = 0;i < PAGE_TABLE_ENTRY_NUM;++i) {
      if (p[i].present == 1) {
        count += 1;
      }
    }
  } else {
    for (int i = 0;i < PAGE_TABLE_ENTRY_NUM;++i) {
      if (p[i].present == 1) {
        // get #pte of next levle
        count += get_childframe_num((pte123_t *)(uint64_t)p[i].paddr, level + 1);
      }
    }
  }

  return count;
}

#define PAGE_TABLE_ENTRY_PADDR_MASK (~((0xffffffffffffffff >> 12) << 12))

static int compare_pagetables(pte123_t *p, pte123_t *q, int level) {
  for (int i = 0; i < PAGE_TABLE_ENTRY_NUM; ++i) {
    if ((p[i].pte_value & PAGE_TABLE_ENTRY_PADDR_MASK) !=
        (q[i].pte_value & PAGE_TABLE_ENTRY_PADDR_MASK)) {
      // this entry not match
      assert(0);
    }

    if (level < 4 && p[i].present == 1 &&
        compare_pagetables((pte123_t *)(uint64_t)p[i].paddr,
                           (pte123_t *)(uint64_t)q[i].paddr, level + 1) == 0) {
      // sub-pages not match
      assert(0);
    } else if (level == 4 && p[i].present == 1) {
      // compare the physical frame content
      int p_ppn = p[i].paddr;
      int q_ppn = q[i].paddr;
      assert(memcmp(&pm[p_ppn << 12], &pm[q_ppn << 12], PAGE_SIZE) == 0);
    }
  }

  return 1;
}

static pcb_t *copy_pcb(pcb_t *parent_pcb) {
  //ATTENTION HERE!!!
  pcb_t *child_pcb = (pcb_t *)&heap[KERNEL_malloc(sizeof(pcb_t))];
  if (child_pcb == NULL) {
    return NULL;
  }
  //pcb_t *child_pcb = KERNEL_malloc(sizeof(pcb_t));
  memcpy(child_pcb, parent_pcb, sizeof(pcb_t));

  // update child PID
  child_pcb->pid = get_newpid();

  // prepare child kernel stack
  kstack_t *child_kstack = aligned_alloc(KERNEL_STACK_SIZE, KERNEL_STACK_SIZE);
  //child_kstack = (kstack_t*)&heap[KERNEL_algined_alloc(KERNEL_STACK_SIZE, KERNEL_STACK_SIZE)];
  memcpy(child_kstack, (kstack_t *)parent_pcb->kstack, sizeof(kstack_t));
  child_pcb->kstack = child_kstack;
  child_kstack->threadinfo.pcb = child_pcb;

  // prepare child process context (especially RSP) for context switching
  // for child process to be switched to
  child_pcb->context.regs.rsp = (uint64_t)child_kstack + KERNEL_STACK_SIZE -
                                sizeof(trapframe_t) - sizeof(userframe_t);

  // add child process PCB to linked list for scheduling
  // it's better the child process is right after parent
  if (parent_pcb->prev == parent_pcb) {
    // parent is the only PCB in the linked list
    assert(parent_pcb->next == parent_pcb);
    parent_pcb->prev = child_pcb;
  }
  parent_pcb->next = child_pcb;
  child_pcb->prev = parent_pcb;

  // call once, return twice
  update_userframe_returnvalue(parent_pcb, child_pcb->pid);
  update_userframe_returnvalue(child_pcb,0);

  // copy the entire page table of parent
  child_pcb->mm.pgd = copy_pagetable(parent_pcb->mm.pgd, 1);

  // All copy works are done here
  return child_pcb;
}

static pcb_t* fork_naive_copy(pcb_t *parent_pcb) {
  //check memory size because we directly copy the physical frames
  int needed_userfrane_num =
    get_childframe_num(parent_pcb->mm.pgd, 1);

  if (enough_frame(needed_userfrane_num) == 0) {
    // there are no enough frames for child process to use
    update_userframe_returnvalue(parent_pcb, -1);
    return NULL;
  }

  pcb_t *child_pcb = copy_pcb(parent_pcb);
  assert(child_pcb != NULL);

  // directly copy user frames in mian memory now
  copy_userframes(child_pcb->mm.pgd, parent_pcb->mm.pgd, 1);
  assert(compare_pagetables(child_pcb->mm.pgd, parent_pcb->mm.pgd, 1) == 1);

  return child_pcb;
}

static pcb_t* fork_cow(pcb_t *parent_pcb) { return 0; }
