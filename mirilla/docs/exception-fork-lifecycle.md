# Exception slab fork lifecycle

The exception file owns one refcounted family. The family has a mutex-protected
weak list of per-mm contexts, an immutable slab geometry, and one strong anchor
for its creating context until file release. A context holds one family reference
and one `mm_count` reference. Each slab VMA holds one context reference; the
global mm registry and the family list hold no context references. A published
slab holds one independently refcounted immutable table. Active readers retain
table references through revocation. The VMA owns its mutable `vmalloc` backing,
publication handle, and slab-set admission slot.

The fd and every mapping hold the same file object, so its family survives while
any inherited mapping exists. An ordinary fork duplicates the fd and VMA, then
calls the slab `.open` callback with the child mm. The callback resolves or
constructs that mm's context in the same family, reserves a child slab slot,
copies the complete parent backing into new storage, and publishes an equivalent
child table if the parent is read-only. The child VMA receives only the child
slab pointer. Before `copy_page_range()`, `.open` zaps the parent VMA's PTEs
while `dup_mmap()` holds the parent mmap write lock. It also sets
`VM_WIPEONFORK` on the new child VMA, making `dup_mmap()` skip the PTE copy.
Setting this flag in `.open` means a prior parent `MADV_KEEPONFORK` cannot
remove the protection for this fork. Parent and child subsequently fault pages
from their own backing. The same procedure applies to nested forks. `CLONE_VM`
shares the original VMA and never calls this fork clone path.

If `.open` cannot finish, it clears the inherited `vm_private_data` pointer.
The VMA stays mapped at its address, but every fault returns `SIGBUS`, every
protection transition fails, and close releases no parent-owned resource. The
child cannot use an incomplete slab or parent backing. The void callback cannot
make the `fork()` syscall report the allocation error. Other successfully cloned
slabs in that mm can still be used; this is a per-VMA fail-closed state.

For a valid slab, the state machine is:

| VMA permissions | Backing | Publication |
| --- | --- | --- |
| read/write | VMA-owned and editable | none |
| read only | VMA-owned and frozen | one immutable table in its mm context |

Whole-VMA `mprotect(R)` snapshots, validates, and publishes before the core
permission change. Whole-VMA `mprotect(RW)` revokes before the core change.
The callback first rejects sealed mappings and all unsupported operations.
On the supported shared VMA, the core's later data/commit accounting cannot
fail: `VM_SHARED` excludes it from data and commit charging, while the address
space limit sees the same total size in old and new flags. The `.close` hook
prevents VMA merging, and a whole-VMA operation needs no split. Thus the core
fixup has no remaining error path after a successful callback on the supported
kernel. A different kernel MM implementation must be audited before this
pre-commit publication protocol is ported.

Linux does not make a protection request spanning multiple VMAs atomic. It can
commit the slab VMA and then return an error from a later VMA. Mirilla keeps its
slab VMA and publication consistent in that case, but the return value alone
does not prove that a multi-VMA request changed no VMA. The callback receives
only the clipped range for its own VMA and has no whole-syscall rollback hook.
Callers of the slab API protect exactly one complete slab VMA at a time.
