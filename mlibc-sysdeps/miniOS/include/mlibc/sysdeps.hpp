#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {

struct MiniOSSysdepTags :
	// Core I/O
	LibcPanic, LibcLog, Isatty,
	Write, Read, Open, Openat, Close, Seek, Flock,
	Readv, Writev,
	// Memory
	AnonAllocate, AnonFree, VmMap, VmUnmap, VmProtect,
	// Threads/TLS/Futex
	TcbSet, GetTid,
	FutexWake, FutexWait,
	// Time
	ClockGet, Sleep,
	// Process lifecycle
	Exit, ThreadExit,
	Fork, Execve, Waitpid,
	GetPid, GetPpid, GetPgid, SetPgid, SetSid,
	GetUid, GetEuid, GetGid, GetEgid,
	Kill, Sigaction, Sigprocmask, ThreadSigmask,
	// Filesystem
	Stat, Statvfs, Fstatvfs, OpenDir, ReadEntries,
	Mkdir, Mkdirat, Rmdir, Unlinkat, Rename,
	Readlink, Symlink, Access, Chmod, Fchmod, Fchmodat,
	GetCwd, Chdir, Fcntl, Ioctl, Mknodat,
	// I/O multiplexing
	Pipe, Dup, Dup2, Poll,
	// Networking
	Socket, Socketpair, Bind, Listen, Accept, Connect,
	Sendto, Recvfrom, GetSockopt, SetSockopt, Shutdown,
	// Terminal
	Tcgetattr, Tcsetattr, Tcgetwinsize, Tcsetwinsize,
	// Timers (miniOS-specific syscall numbers)
	SetItimer, GetItimer
{};

template<typename Tag>
using Sysdeps = SysdepOf<MiniOSSysdepTags, Tag>;

} // namespace mlibc
