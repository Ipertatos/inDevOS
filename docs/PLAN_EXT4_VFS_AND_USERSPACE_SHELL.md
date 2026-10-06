# Execution Plan: Ext4 in VFS + Userspace Shell with Root Capability

This document outlines a step-by-step plan to:
1. **Integrate ext4 into the VFS** so both FAT32 and ext4 can be mounted and used through a single file API.
2. **Move the shell to userspace** so it runs as a normal process (e.g. via `exec`), with the ability to **run privileged operations as root** when needed (e.g. via `su` / setuid).

---

## Part A: Ext4 in VFS

### A.1 Current State

- **VFS** exists (`vfs.c`, `vfs.h`) with a full API: `vfs_mount`, `vfs_open`, `vfs_read`, `vfs_write`, `vfs_seek`, `vfs_close`, `vfs_readdir`, `vfs_create`, `vfs_mkdir`, `vfs_unlink`, `vfs_exists`.
- **FAT32** has a VFS adapter (`vfs_fat32.c`) implementing `vfs_fs_ops_t`, but **`vfs_fat32_register()` is never called**; all file I/O currently goes **directly to FAT32** (syscalls use `fat32_open`/`fat32_read`/etc., and Zeus/cmd use `fat32_*` directly).
- **ext4** has a full read/write driver (`ext4.c`/`ext4.h`) with path-based APIs: `ext4_lookup_path`, `ext4_read_file`, `ext4_write_file`, `ext4_create_file`, `ext4_delete_file`, `ext4_list_directory`, `ext4_create_directory`, `ext4_remove_directory`, `ext4_file_exists`. It has **no open-handle API**; it uses inode + offset implicitly.

### A.2 Ext4 VFS Adapter (new file)

**Goal:** Implement `vfs_fs_ops_t` for ext4 so VFS can use it like FAT32.

1. **Add `vfs_ext4.c` and `vfs_ext4.h`** (mirror `vfs_fat32.c` / `vfs_fat32.h`).

2. **Open handle for ext4:**  
   Ext4 has no persistent “open file” struct. Define a small **cookie** used as the VFS handle:
   - Either a **heap-allocated struct** (e.g. `ext4_vfs_handle_t`) with:
     - `uint32_t inode`
     - `uint32_t position` (current read/write offset)
     - `uint32_t size` (file size, from inode when opened)
   - Or a single global pool of such structs (e.g. `ext4_open_files[VFS_MAX_OPEN_FILES]`) and return an index/cookie.
   - **open:** `ext4_lookup_path(rel_path)` → inode; read inode to get size; allocate handle (inode, position=0, size); return handle.
   - **read:** use `ext4_read_file(handle->inode, buffer, handle->position, size)`; advance `handle->position` by bytes read; return bytes read.
   - **write:** use `ext4_write_file(handle->inode, buffer, handle->position, size)`; advance `handle->position`; update `handle->size` if needed.
   - **seek:** update `handle->position` (clamp to 0..size for SEEK_SET/END); return new position.
   - **close:** free the handle (or mark slot free).

3. **Other ops:**  
   - **readdir:** build path with leading `/` + `rel_path`, call `ext4_list_directory`; convert `ext4_dir_list_t` to `vfs_dirent_t` (name, inode, type, size).
   - **create:** `ext4_create_file(rel_path)`.
   - **mkdir:** `ext4_create_directory(rel_path)`.
   - **unlink:** for files use `ext4_delete_file`; for empty dirs use `ext4_remove_directory` (or add a “is dir” check and call the right one).
   - **exists:** `ext4_file_exists(rel_path)` → return 1/0.

4. **Path convention:**  
   Like FAT32 adapter: if VFS gives `rel_path` (e.g. `"foo/bar"`), build full path as `"/" + rel_path` for ext4 (ext4 expects paths starting with `/`).

5. **Register with VFS:**  
   Provide `vfs_ext4_register(const char *mount_path)`. It should:
   - Call `vfs_mount(mount_path, &ext4_vfs_ops, NULL)` (ext4 uses global `ext4_fs` state, so `fs_data` can be NULL, similar to `vfs_fat32_register` using `&fat32_filesystem`).

### A.3 Mount flow: FAT32 and ext4 via VFS

**Goal:** One code path that can mount either FAT32 or ext4 and register it with VFS.

1. **Unify mount in the kernel:**
   - **Option A – device-first:**  
     `mount <device_id> [partition_lba] [fstype] [path]`  
     - Examples: `mount 0 0 fat32 /`, `mount 1 1 ext4 /mnt`.
   - **Option B – path optional:**  
     `mount <device_id> [partition_lba] [fstype]`  
     - Default path `/` for first mount; later extend for multiple mounts.

2. **Implementation (in `cmd.c` and/or Zeus `mount` handler):**
   - Parse args: `device_id`, `partition_lba` (default 0), `fstype` (default `fat32`), optional `path` (default `"/"`).
   - If `fstype` is `fat32`:
     - Call `fat32_mount(device_id)` (use partition_lba if FAT32 API supports it; otherwise current behavior).
     - Call `vfs_fat32_register(path)` (e.g. `"/"`).
   - If `fstype` is `ext4`:
     - Call `ext4_mount(device_id, partition_lba)`.
     - Call `vfs_ext4_register(path)`.
   - **Constraint:** ext4 driver has a single global mount; only one ext4 mount at a time. Document this.

3. **Unmount:**
   - Extend `unmount` to accept an optional path: `unmount [path]`.
   - Call `vfs_unmount(path)`.
   - If the mount at `path` was FAT32, then call `fat32_unmount()` after VFS unmount; if it was ext4, call `ext4_unmount()`.  
   - Tracking: either store in `vfs_filesystem_t.fs_data` a small tag (e.g. "fat32" vs "ext4"), or maintain a side table (mount_path → fstype). Simplest: when unmounting by path, VFS already has the `fs`; the ops pointer can be compared to `&fat32_vfs_ops` vs `&ext4_vfs_ops` to decide which low-level unmount to call (and optionally clear global FAT32/ext4 state).

### A.4 Switch syscalls to VFS

**Goal:** All file operations from user space go through VFS so that both FAT32 and ext4 (and any future FS) work transparently.

1. **Current behavior:**  
   `sys_open` / `sys_read` / `sys_write` / `sys_seek` / `sys_close` use `fat32_open` / `fat32_read` / … and store `fat32_file_t*` in `proc->fds[fd].file_handle` with `fd_type == FD_TYPE_FILE`.

2. **New behavior:**
   - **Path resolution:**  
     Resolve path (e.g. with `proc->cwd`) to an absolute path. Use **VFS** for any path that matches a mount (e.g. `vfs_find_mount(path, &rel)`).
   - **sys_open:**  
     - If VFS has a mount for the path: call `vfs_open(absolute_path, flags)` → get a **VFS fd** (integer index into VFS’s `open_files[]`).
     - Store in process: set `fd_type = FD_TYPE_VFS` (new constant) and store the VFS fd (e.g. `file_handle = (void*)(intptr_t)vfs_fd` or add a field `vfs_fd`).
   - **sys_read / sys_write / sys_seek / sys_close:**  
     - If `fd_type == FD_TYPE_VFS`: call `vfs_read` / `vfs_write` / `vfs_seek` / `vfs_close` with the stored VFS fd. For read/write, use `proc->fds[fd].position` only if VFS doesn’t manage position (VFS does manage position inside the fs handle for FAT32; for your ext4 handle you also track position there—so either rely on VFS handle position and don’t use `proc->fds[fd].position` for VFS fds, or make VFS seek explicit; the former is simpler).
   - **Backward compatibility:**  
     If no VFS mount exists (e.g. only FAT32 mounted the old way without registering with VFS), you have two options:  
     - **(Recommended)** Always require at least one VFS mount: during boot, after `fat32_mount(0)`, call `vfs_fat32_register("/")`. Then all syscalls use VFS.  
     - Or keep a fallback: if `vfs_find_mount` returns NULL, fall back to current FAT32-only path (fat32_open etc.) and keep storing `fat32_file_t*` with `FD_TYPE_FILE`.

3. **FD type and descriptor:**
   - Add `FD_TYPE_VFS` (e.g. `4`) in `syscall.h`.
   - In `file_descriptor_t`, either:
     - Use `file_handle` as a single field: for `FD_TYPE_FILE` it’s `fat32_file_t*`; for `FD_TYPE_VFS` it’s `(void*)(intptr_t)vfs_fd`. Then in sys_close you must call either `fat32_close` or `vfs_close` depending on type.  
     Or:
     - Add a dedicated `int vfs_fd` (or `int32_t vfs_fd`) and keep `file_handle` for FAT32/sockets/pipes. Then `FD_TYPE_VFS` implies use of `vfs_fd`.
   - **Fork/dup:** When duplicating an fd, if it’s VFS you don’t duplicate the underlying VFS fd (VFS has no dup); you just copy the same vfs_fd and ref-count or document that closing one closes the underlying VFS fd. Simplest: same vfs_fd in both; on close, call vfs_close once (and avoid double-close by ref-counting or a “closed” flag in the process fd entry).

4. **VFS fd lifetime:**  
   VFS uses a global `open_files[]` table. When a process closes an fd that is VFS-backed, call `vfs_close(vfs_fd)`. No per-process VFS fd table is needed; the integer fd from `vfs_open` is what the process stores.

### A.5 Boot and Zeus use of VFS

1. **Boot:**  
   In `kernel.c`, after `storage_init()` and `mount_filesystem_device(0)` (or equivalent), ensure the root filesystem is **registered with VFS**:
   - If you keep `mount_filesystem_device(0)` as “mount device 0 as FAT32 at /”, then inside `mount_filesystem_device` do:
     - `fat32_mount(0);`
     - `vfs_fat32_register("/");`
   - So the first mount is both FAT32 and VFS at `/`.

2. **Zeus and cmd.c:**  
   For commands that currently use `fat32_*` directly (e.g. `ls`, `cat`, `touch`, `mkdir`, `rm`, `cp`, `mv`), switch them to **VFS**:
   - Resolve path (relative to current “cwd” if Zeus has one; kernel doesn’t have a global cwd for the shell yet—Zeus runs in kernel).
   - Call `vfs_readdir`, `vfs_open` + `vfs_read`/`vfs_write` + `vfs_close`, `vfs_create`, `vfs_mkdir`, `vfs_unlink` as appropriate.
   - This way Zeus and user processes see the same files whether the root is FAT32 or ext4 (once you add ext4 mount).

3. **Places that check `fat32_is_mounted()`:**  
   Replace or complement with a “any VFS mount” check, e.g. a helper `vfs_has_mount()` or “is there a mount at `/`?” so that pkg, users, settings, exec, etc. work when root is ext4. Prefer one function (e.g. `vfs_has_root_mount()`) so you can later support root on ext4.

### A.6 Order of implementation (Ext4 + VFS)

1. Implement **ext4 VFS adapter** (`vfs_ext4.c` / `vfs_ext4.h`) and `vfs_ext4_register`.
2. **Wire boot to VFS:** call `vfs_fat32_register("/")` after `fat32_mount(0)` in `mount_filesystem_device(0)`.
3. **Switch syscalls** to VFS for file ops (add `FD_TYPE_VFS`, use VFS when a mount exists; optionally keep FAT32 fallback during transition).
4. **Extend mount/unmount** in Zeus/cmd to support `mount <dev> [lba] [fat32|ext4] [path]` and `unmount [path]`, and call the right register/unmount for FAT32 vs ext4.
5. **Migrate Zeus/cmd file operations** from direct `fat32_*` to VFS.
6. **Replace `fat32_is_mounted()` checks** with a VFS-based “root (or relevant path) mounted” check where appropriate.

---

## Part B: Shell in Userspace with Root When Needed

### B.1 Current State

- **Zeus** runs **inside the kernel** main loop (`kernel.c`): keyboard input triggers `zeus_process_command(buffer)`. There is no userspace shell binary; no `exec("/bin/sh")` or similar.
- **Process model:** You have `process_scheduler`, `exec` (ELF loader), `fork`, and syscalls (open/read/write, fork/exec, etc.). Process has `uid`/`gid` in `process_t`, but they are **not** set from the current login (e.g. `user_current_uid()`) when creating or forking processes.
- **Users:** `users.c` provides `user_login`, `user_current_uid()`, `user_is_root()`, etc. So “current user” exists for the session, but it is not yet tied to the process’s `uid`/`gid`.

### B.2 Goals

1. Run the **shell as a userspace process** (e.g. one that reads from stdin, writes to stdout, and runs commands via fork+exec).
2. Support **“run as root when needed”**: either a **setuid(0)** style path (e.g. after authentication) or a **privileged helper** that the kernel runs on behalf of the user (e.g. for mount/kill etc.).

### B.3 Tie process identity to login

1. **Set process uid/gid at creation:**
   - In **init_process_system** (or wherever the first process is set up): set `current_process->uid` and `current_process->gid` from `user_current_uid()` and `user_current_gid()`. If no user is logged in, use a default (e.g. UID_ROOT for “console” or a fixed “nobody” UID).
   - In **sys_fork:** copy `parent->uid` and `parent->gid` into `child->uid` and `child->gid` (already natural if you copy the whole process struct; ensure it’s done).
   - In **exec** (when loading a new program): keep the existing process’s `uid`/`gid` (do not reset them in exec). So the new image runs as the same user.

2. **Syscalls that should check uid/gid:**
   - **sys_mount / sys_unmount:** allow only if `proc->uid == UID_ROOT` (or add a capability later). Return -EPERM otherwise.
   - **sys_kill:** allow sending signals to another process only if current is root or the target’s uid matches current uid (standard Unix semantics).
   - Optionally: **chmod/chown** (if exposed via syscall): restrict to root or file owner.

### B.4 Add setuid/setgid (for “run as root when needed”)

1. **New syscalls:**  
   - `SYS_SETUID` (e.g. number 71): `int setuid(uint32_t uid);`  
   - `SYS_SETGID` (e.g. 72): `int setgid(uint32_t gid);`

2. **Semantics (simplified Unix-like):**
   - **setuid(uid):**
     - If `current_process->uid == UID_ROOT`: allow setting to any uid (so root can “drop” to a user or switch to root).
     - Else: allow only `uid == current_process->uid` (no change in practice).
     - On success: set `current_process->uid = uid`.
   - **setgid(gid):** same idea for gid (root can set any; non-root only to current gid).

3. **“Run as root” from the shell:**
   - **Option A – su-style:**  
     A userspace program (e.g. `su` or `login`) that:
     - Prompts for password (or reads from a secure channel).
     - Calls a **kernel API** that validates the password and, if correct for root (or the target user), calls the equivalent of setuid (e.g. a syscall or internal kernel function).  
     To avoid putting password logic in the kernel, prefer: **kernel only provides setuid;** the userspace `su` reads password, then calls a **privileged syscall** that accepts a token (e.g. “I just verified root password”) — e.g. `sys_setuid_verified(0)` that the kernel only allows if the process is in a “verified” state. Simpler: **kernel setuid(0) allowed only for root.** So to “become root”, the user must log in as root (e.g. `login root`), which sets the **session** as root; then when you start the userspace shell, that shell process must get uid=0. So the flow is: **login** (kernel or a small stub) sets “current user” to root; then the **first process** (or the shell) is started with uid=0. So you need “login” to set the process uid for the process that will run the shell (e.g. init starts a getty/login, and after login the shell is exec’d with the correct uid).  
   - **Option B – setuid binary:**  
     A small executable (e.g. `/bin/su`) that is **owned by root** and has a “setuid” bit. On exec, the kernel sets the process’s effective uid to the file owner (root). That requires: (1) storing and checking a “setuid” bit in the filesystem/VFS; (2) on exec, if the executable has setuid, set `proc->uid = file_owner_uid`. So “run as root when needed” = run a setuid root binary that then exec’s a shell or the requested command.

   **Recommended for “run as root when needed”:**  
   - Implement **setuid/setgid** as above.  
   - Implement **login** (or reuse existing `user_login`): when a user logs in on the console, the **kernel** (or a tiny stub that runs before the userspace shell) sets the “current session” uid/gid. When the **userspace shell** is started, it must be started **with that uid/gid**. So either:  
     - The kernel starts **one** userspace process (init/shell) and passes uid/gid via auxv or environment (e.g. `UID=0`), and the kernel’s process creation sets `proc->uid`/`proc->gid` from that; or  
     - The kernel’s “start userspace shell” path creates the process with uid/gid = current_session_uid/gid.  
   - Then “run as root when needed” = **log in as root** (e.g. `login root`), then run the shell; the shell will have uid=0 and can call mount, kill, etc.  
   - Optionally add a **setuid bit** on executables later so that binaries like `/bin/su` can become root without storing passwords in the kernel.

### B.5 Userspace shell binary

1. **Build a minimal shell** that:
   - Uses only syscalls: read(0, …), write(1, …), open/close/read/write/seek for files, fork, exec, wait, getpid, chdir, getcwd, and optionally mount/unmount (which will be root-only).
   - Reads a line from stdin, parses simple commands (e.g. first word = program, rest = args), and for each command:
     - fork();
     - in child: exec(program, args);
     - in parent: wait() for child.
   - For built-ins (cd, exit, maybe export), implement in the shell without exec.
   - No need to port all of Zeus; a minimal clone that supports `ls` (exec a program that lists dirs via getcwd + readdir) and `exec` is enough for a first step. Alternatively, **port a subset of Zeus to userspace** (same command names, but implemented with syscalls and exec of helpers).

2. **Where the shell lives:**  
   - Put the shell binary on the root filesystem (e.g. `/sh` or `/bin/sh`). It can be a custom ELF that you build with your toolchain and place on the FAT32/ext4 image.
   - Kernel must be able to **open and exec** that path. So root must be mounted (VFS) and the path resolved (e.g. `/sh`) before starting the shell.

3. **How the kernel starts the userspace shell:**
   - **Option 1 – kernel main loop starts one process:**  
     After boot, instead of (or in addition to) running Zeus in the kernel loop, the kernel does:
     - Ensure root is mounted (VFS).
     - Load and exec the userspace shell binary (e.g. `exec_run_elf_from_path("/sh")` or similar). That runs in the **current** context (the “kernel shell” context). So you need a **dedicated process** for the userspace shell: create a process (e.g. `process_create("sh", shell_entry, NULL)`) whose entry point is a small stub that loads and runs the ELF (or the kernel has a “run userspace program X” that creates a process and runs exec).  
   - **Option 2 – init process:**  
     Kernel starts a single **init** process (e.g. PID 1) that is a small program which:
     - Sets up session (e.g. open console fds 0,1,2).
     - Optionally runs `login` (or reads “who is logged in” from kernel), then exec’s the shell with the right uid (if you pass uid via env or kernel sets it when creating the process for init).  
     Then the “main loop” in kernel no longer runs Zeus; it only runs the scheduler and the init process (and its children). Console input must be **fed to the process that has stdin** (e.g. the shell). So you need **keyboard → stdin of the foreground process**. That implies the kernel’s keyboard driver pushes input to a “foreground process” or a TTY layer that the shell’s stdin reads from. This is a bigger change (TTY/PTY or at least “stdin attached to console”).

   **Pragmatic order:**  
   - First get **one** userspace process running: kernel creates a process that exec’s `/sh` (or similar). That process’s stdin/stdout/stderr are 0,1,2. You need to **wire console output to fd 1** and **console input to fd 0** for that process. Today, printf in kernel goes to the framebuffer/serial; read from keyboard goes to the kernel buffer that Zeus uses. So you need:
     - **stdout (fd 1):** when the shell (or any user process) does `write(1, buf, n)`, the kernel’s `sys_write` should send that to the **console** (framebuffer and/or serial). You may already do this for fd 1.
     - **stdin (fd 0):** when the shell does `read(0, buf, n)`, the kernel must **block** until keyboard (or TTY) input is available and then return it. So keyboard driver must feed a queue that the process’s read(0) reads from. This is the **TTY/input layer** that you may already have partially (e.g. keyboard buffer); it must be associated with the **foreground process** so that read(0) in that process gets the keys.
   - Once that works, the “main loop” can stop calling Zeus and instead run the scheduler; the only “interactive” process is the userspace shell, which reads from fd 0 and writes to fd 1.

### B.6 Implementation order (userspace shell + root)

1. **Process identity:** Set `uid`/`gid` in init_process_system and in fork/exec from current user (or default). Add sys_mount/sys_unmount/sys_kill checks for root or same-uid.
2. **setuid/setgid syscalls:** Implement and expose in user API; document that only root can set arbitrary uid/gid.
3. **Console ↔ process stdin/stdout:** Ensure fd 0 and fd 1 for the “main” process are connected to keyboard and console. If not already, add a small TTY layer: one “foreground” process; keyboard input is queued for it; sys_read(0) dequeues.
4. **Build and deploy a minimal userspace shell** (ELF) that uses only syscalls; put it on the root filesystem as `/sh` (or `/bin/sh`).
5. **Kernel start shell:** After boot and VFS mount, create one process that exec’s `/sh` and make it the foreground process. Disable (or keep optional) the in-kernel Zeus so that the main loop no longer calls Zeus for the primary console; the userspace shell is the primary interface.
6. **“Run as root”:** Implement login (or use existing user_login) so that when the user logs in as root, the **next** process that runs (e.g. the shell started by init) has uid=0. So init could: read “login:” / “password:”, call user_login(), then exec the shell; the shell process’s uid/gid are set at creation from the current session (you must set “session uid” in the kernel when login succeeds, and when creating the shell process use that session uid). Alternatively, keep running Zeus in kernel for “login” and “su”, and only the **subprocesses** (exec’d by Zeus) run in userspace with the correct uid—then “run as root when needed” = run `su` from Zeus which exec’s a setuid binary or a helper that calls setuid(0) after verifying password via a dedicated syscall.

---

## Part C: Summary Checklist

### Ext4 in VFS
- [ ] A.2 Implement `vfs_ext4.c` / `vfs_ext4.h` (ext4 open handle, read, write, seek, close, readdir, create, mkdir, unlink, exists).
- [ ] A.3 Add `vfs_ext4_register(mount_path)` and wire unmount to call `ext4_unmount()` when unmounting an ext4 mount.
- [ ] A.4 Extend mount command to `mount <dev> [lba] [fat32|ext4] [path]`; call fat32_mount + vfs_fat32_register or ext4_mount + vfs_ext4_register.
- [ ] A.5 Boot: call `vfs_fat32_register("/")` after fat32_mount in mount_filesystem_device(0).
- [ ] A.6 Add FD_TYPE_VFS; switch sys_open/sys_read/sys_write/sys_seek/sys_close to use VFS when a mount exists; store VFS fd in process fd table.
- [ ] A.7 Migrate Zeus/cmd file ops to VFS; replace fat32_is_mounted() with a VFS “has root mount” check where appropriate.

### Userspace shell and root
- [ ] B.3 Set process uid/gid at process creation and fork/exec; restrict sys_mount, sys_unmount, sys_kill to root or same-uid as target.
- [ ] B.4 Add SYS_SETUID / SYS_SETGID; implement setuid/setgid semantics (root can set any; non-root only self).
- [ ] B.5 Wire fd 0/1 to console (keyboard → stdin, stdout → display) for the foreground process.
- [ ] B.6 Build minimal userspace shell ELF; place on root fs; add kernel path to create one process that exec’s it and make it the main interactive process.
- [ ] B.7 Implement “run as root”: login sets session uid; shell (or init) is started with that uid; optionally add setuid-bit on executables later.

---

## Dependencies

- **Ext4 in VFS** does not depend on the userspace shell; it can be done first.
- **Userspace shell** benefits from VFS (so the shell can live on either FAT32 or ext4 root).
- **Root capability** (setuid, mount/kill checks) can be implemented in parallel with the shell; the shell then uses them once it runs in userspace.

Recommended order: **A (Ext4 + VFS) → B.3/B.4 (identity + setuid) → B.5/B.6 (stdin/stdout + shell binary + kernel start) → B.7 (login/su and “run as root”).**
