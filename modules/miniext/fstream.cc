/*
 * The miniext side of the std::fstream shim (see include/osv/fstream_shim.hpp).
 *
 * Kept behind plain C entry points so miniext's headers stay out of the
 * translation units that force-include the shim header -- 1757 of them in the
 * DuckDB build alone.
 */

#include <cerrno>
#include <string>

#include "modules/miniext/miniext.hh"

namespace {

//! Same rule as LocalFileSystem: there is no working directory, so a relative
//! path is resolved against the mount point.
std::string absolute(const char *path)
{
	if (path && path[0] == '/') {
		return path;
	}
	std::string root = miniext::mount_point();
	if (root.empty()) {
		return path ? path : "";
	}
	return root + "/" + (path ? path : "");
}

} // namespace

extern "C" {

void *miniosv_fs_open(const char *path, int write, int append)
{
	const std::string full = absolute(path);
	int mode = write ? (miniext::O_RDWR | miniext::O_CREATE) : miniext::O_RD;
	if (write && !append) {
		mode |= miniext::O_TRUNC;
	}
	int err = 0;
	miniext::file *f = miniext::open(full.c_str(), mode, &err);
	if (!f) {
		return nullptr;
	}
	// The stream keeps its own position; the handle carries it here.
	auto *pos = new uint64_t(append ? miniext::size(f) : 0);
	return new std::pair<miniext::file *, uint64_t *>(f, pos);
}

namespace {
using entry = std::pair<miniext::file *, uint64_t *>;
entry *as_entry(void *h) { return static_cast<entry *>(h); }
} // namespace

void miniosv_fs_close(void *handle)
{
	if (!handle) {
		return;
	}
	auto *e = as_entry(handle);
	miniext::close(e->first);
	delete e->second;
	delete e;
}

long miniosv_fs_read(void *handle, char *buf, unsigned long len)
{
	if (!handle) {
		return -1;
	}
	auto *e = as_entry(handle);
	int64_t got = miniext::pread(e->first, buf, len, *e->second);
	if (got < 0) {
		return -1;
	}
	*e->second += static_cast<uint64_t>(got);
	return static_cast<long>(got);
}

long miniosv_fs_write(void *handle, const char *buf, unsigned long len)
{
	if (!handle) {
		return -1;
	}
	auto *e = as_entry(handle);
	int64_t put = miniext::pwrite(e->first, buf, len, *e->second);
	if (put < 0) {
		return -1;
	}
	*e->second += static_cast<uint64_t>(put);
	return static_cast<long>(put);
}

long long miniosv_fs_seek(void *handle, long long off, int whence)
{
	if (!handle) {
		return -1;
	}
	auto *e = as_entry(handle);
	long long base = 0;
	switch (whence) {
	case 0:
		base = 0;
		break;
	case 1:
		base = static_cast<long long>(*e->second);
		break;
	default:
		base = static_cast<long long>(miniext::size(e->first));
		break;
	}
	long long pos = base + off;
	if (pos < 0) {
		return -1;
	}
	*e->second = static_cast<uint64_t>(pos);
	return pos;
}

long long miniosv_fs_tell(void *handle)
{
	return handle ? static_cast<long long>(*as_entry(handle)->second) : -1;
}

} // extern "C"
