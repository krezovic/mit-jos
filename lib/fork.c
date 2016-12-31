// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).
	envid_t envid = sys_getenvid();

	pte_t pte = uvpt[PGNUM(addr)];

	if (!(err & FEC_WR) || !(pte & PTE_COW))
		panic("pgfault: Not a write error or address is not on a COW page");

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	if (sys_page_alloc(envid, PFTEMP, PTE_P | PTE_W | PTE_U) < 0)
		panic("pgfault: Cannot allocate a new page at PFTEMP");

	addr = ROUNDDOWN(addr, PGSIZE);
	memmove(PFTEMP, addr, PGSIZE);

	if (sys_page_unmap(envid, addr) < 0)
		panic("pgfault: Cannot unmap old page from addr 0x%08x", addr);

	if (sys_page_map(envid, PFTEMP, envid, addr, PTE_P | PTE_W | PTE_U) < 0)
		panic("pgfault: Cannot map page to PFTEMP");
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;
	pte_t pte = uvpt[pn];
	uint32_t cow = (pte & PTE_COW || pte & PTE_W) ? PTE_COW : 0;
	void *addr = (void *)(pn * PGSIZE);
	envid_t thisenvid = sys_getenvid();

	if (sys_page_map(thisenvid, addr, envid, addr, PTE_P | PTE_U | cow) < 0)
		panic("duppage: Cannot map page into target environment");

	if (cow)
		if (sys_page_map(thisenvid, addr, thisenvid, addr, PTE_P | PTE_U | PTE_COW) < 0)
			panic("duppage: Cannot remap page as COW");

	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	envid_t thisenvid = sys_getenvid();
	envid_t envid;
	size_t i, j;

	set_pgfault_handler(pgfault);

	if ((envid = sys_exofork()) < 0)
		panic("fork: Cannot create a child environment");

	if (envid == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}

	for (i = 0; i < PDX(UTOP); i++) {
		if (!(uvpd[i] & PTE_P))
			continue;

		for (j = 0; j < NPTENTRIES; j++) {
			uint32_t pgno = i << 10 | j;

			if (pgno == PGNUM(UXSTACKTOP - PGSIZE))
				continue;

			if (uvpt[pgno] & PTE_P)
				duppage(envid, pgno);
		}
	}

	if (sys_page_alloc(envid, (void *) (UXSTACKTOP - PGSIZE), PTE_P | PTE_W | PTE_U) < 0)
		panic("fork: Cannot allocate child exception stack");

	if (sys_env_set_pgfault_upcall(envid, thisenv->env_pgfault_upcall) < 0)
		panic("fork: Cannot set pgfault_upcall for child environment");

	if (sys_env_set_status(envid, ENV_RUNNABLE) < 0)
		panic("fork: Cannot change child running status");

	return envid;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
