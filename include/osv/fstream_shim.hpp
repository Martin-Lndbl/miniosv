/*
 * miniOSv: std::ifstream / ofstream / fstream, backed by miniext.
 *
 * libc++ here is built with LIBCXX_ENABLE_FILESYSTEM=OFF (scripts/build-libcxx.sh),
 * because when it was configured the kernel had no filesystem at all. <iosfwd>
 * still declares basic_filebuf/basic_ifstream/... and the ifstream typedefs
 * unconditionally, but <fstream> only *defines* them under
 * _LIBCPP_HAS_FILESYSTEM, so std::ifstream is a typedef to an undefined
 * template. Anything that names it fails to compile.
 *
 * Both applications name it for real. DuckDB's benchmark runner reads every
 * .benchmark file with std::ifstream and writes its output and log with
 * ofstream; llama.cpp's common/ reads prompt, grammar and chat-template files
 * the same way, and the vAccel backend reads its kernel-shape JSON.
 *
 * Rather than turn libc++'s filesystem back on -- which would route these
 * through libc's fopen/fread, none of which work here -- this supplies explicit
 * char specializations that talk to miniext. It is force-included with
 * -include, so no application source has to name it.
 *
 * The implementation lives in modules/miniext/fstream.cc behind a handful of C
 * entry points, so miniext's headers do not have to be dragged into every
 * translation unit that force-includes this.
 *
 * Specializing a std template is formally UB. So is DuckDB's wchar shim; the
 * alternative is patching each application, which is worse to maintain.
 */

#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <streambuf>
#include <string>
#include <utility>

extern "C" {
//! Returns a handle, or nullptr. `write` opens for writing (creating and
//! truncating), otherwise for reading.
void *miniosv_fs_open(const char *path, int write, int append);
void miniosv_fs_close(void *handle);
long miniosv_fs_read(void *handle, char *buf, unsigned long len);
long miniosv_fs_write(void *handle, const char *buf, unsigned long len);
//! whence: 0 set, 1 cur, 2 end. Returns the new absolute position, or -1.
long long miniosv_fs_seek(void *handle, long long off, int whence);
long long miniosv_fs_tell(void *handle);
}

namespace std {

//! A streambuf over a miniext file. Buffered a block at a time in each
//! direction, which is what keeps getline() from turning into one read per
//! character.
template <>
class basic_filebuf<char, char_traits<char>> : public basic_streambuf<char> {
public:
	basic_filebuf() = default;
	~basic_filebuf() override {
		close();
	}

	basic_filebuf(const basic_filebuf &) = delete;
	basic_filebuf &operator=(const basic_filebuf &) = delete;

	//! Moving transfers the open file and leaves both buffers empty. The
	//! get/put areas point into this object's own _in/_out arrays, so they
	//! cannot be carried across; instead pending output is flushed and
	//! read-ahead is given back to the file position, which leaves the source's
	//! logical offset and the file's real offset equal at the moment of the
	//! handover. underflow()/sync() re-establish the areas on first use.
	basic_filebuf &operator=(basic_filebuf &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		close();
		if (other._handle) {
			other.sync();
			if (other.gptr() && other.gptr() < other.egptr()) {
				miniosv_fs_seek(other._handle,
				                -static_cast<long long>(other.egptr() - other.gptr()), 1);
			}
		}
		_handle = other._handle;
		_writing = other._writing;
		other._handle = nullptr;
		other.setg(nullptr, nullptr, nullptr);
		other.setp(nullptr, nullptr);
		setg(nullptr, nullptr, nullptr);
		setp(nullptr, nullptr);
		return *this;
	}

	basic_filebuf(basic_filebuf &&other) noexcept {
		*this = std::move(other);
	}

	bool is_open() const {
		return _handle != nullptr;
	}

	basic_filebuf *open(const char *path, ios_base::openmode mode) {
		if (_handle) {
			return nullptr;
		}
		const bool writing = (mode & ios_base::out) != 0;
		const bool append = (mode & ios_base::app) != 0;
		_handle = miniosv_fs_open(path, writing ? 1 : 0, append ? 1 : 0);
		if (!_handle) {
			return nullptr;
		}
		_writing = writing;
		if (mode & ios_base::ate) {
			miniosv_fs_seek(_handle, 0, 2);
		}
		return this;
	}

	basic_filebuf *close() {
		if (!_handle) {
			return nullptr;
		}
		sync();
		miniosv_fs_close(_handle);
		_handle = nullptr;
		return this;
	}

protected:
	int_type underflow() override {
		if (!_handle || _writing) {
			return traits_type::eof();
		}
		if (gptr() && gptr() < egptr()) {
			return traits_type::to_int_type(*gptr());
		}
		long got = miniosv_fs_read(_handle, _in, sizeof(_in));
		if (got <= 0) {
			return traits_type::eof();
		}
		setg(_in, _in, _in + got);
		return traits_type::to_int_type(*gptr());
	}

	int_type overflow(int_type c = traits_type::eof()) override {
		if (!_handle || !_writing) {
			return traits_type::eof();
		}
		if (sync() != 0) {
			return traits_type::eof();
		}
		if (!traits_type::eq_int_type(c, traits_type::eof())) {
			char ch = traits_type::to_char_type(c);
			if (miniosv_fs_write(_handle, &ch, 1) != 1) {
				return traits_type::eof();
			}
		}
		return traits_type::not_eof(c);
	}

	int sync() override {
		if (!_handle || !_writing) {
			return 0;
		}
		const long pending = static_cast<long>(pptr() - pbase());
		if (pending > 0) {
			if (miniosv_fs_write(_handle, pbase(), pending) != pending) {
				return -1;
			}
		}
		setp(_out, _out + sizeof(_out));
		return 0;
	}

	streamsize xsputn(const char *s, streamsize n) override {
		if (!_handle || !_writing) {
			return 0;
		}
		if (sync() != 0) {
			return 0;
		}
		return miniosv_fs_write(_handle, s, static_cast<unsigned long>(n));
	}

	pos_type seekoff(off_type off, ios_base::seekdir dir,
	                 ios_base::openmode = ios_base::in | ios_base::out) override {
		if (!_handle) {
			return pos_type(-1);
		}
		sync();
		int whence = dir == ios_base::beg ? 0 : (dir == ios_base::cur ? 1 : 2);
		// A pending read buffer means the file position is ahead of the logical
		// one; fold that in before seeking relative to "current".
		if (whence == 1 && gptr()) {
			off -= static_cast<off_type>(egptr() - gptr());
		}
		long long pos = miniosv_fs_seek(_handle, off, whence);
		setg(nullptr, nullptr, nullptr);
		return pos < 0 ? pos_type(-1) : pos_type(pos);
	}

	pos_type seekpos(pos_type pos, ios_base::openmode m = ios_base::in | ios_base::out) override {
		return seekoff(static_cast<off_type>(pos), ios_base::beg, m);
	}

private:
	void *_handle = nullptr;
	bool _writing = false;
	char _in[4096];
	char _out[4096];
};

template <>
class basic_ifstream<char, char_traits<char>> : public basic_istream<char> {
public:
	basic_ifstream() : basic_istream<char>(&_buf) {
	}
	explicit basic_ifstream(const char *path, ios_base::openmode mode = ios_base::in)
	    : basic_istream<char>(&_buf) {
		open(path, mode);
	}
	explicit basic_ifstream(const string &path, ios_base::openmode mode = ios_base::in)
	    : basic_ifstream(path.c_str(), mode) {
	}

	basic_ifstream(basic_ifstream &&other) : basic_istream<char>(&_buf) {
		_buf = std::move(other._buf);
		clear(other.rdstate());
		other.clear();
	}
	basic_ifstream &operator=(basic_ifstream &&other) {
		if (this != &other) {
			_buf = std::move(other._buf);
			clear(other.rdstate());
			other.clear();
		}
		return *this;
	}

	void open(const char *path, ios_base::openmode mode = ios_base::in) {
		if (!_buf.open(path, mode | ios_base::in)) {
			setstate(ios_base::failbit);
		} else {
			clear();
		}
	}
	void open(const string &path, ios_base::openmode mode = ios_base::in) {
		open(path.c_str(), mode);
	}

	bool is_open() const {
		return _buf.is_open();
	}
	void close() {
		if (!_buf.close()) {
			setstate(ios_base::failbit);
		}
	}
	basic_filebuf<char> *rdbuf() const {
		return const_cast<basic_filebuf<char> *>(&_buf);
	}

private:
	basic_filebuf<char> _buf;
};

template <>
class basic_ofstream<char, char_traits<char>> : public basic_ostream<char> {
public:
	basic_ofstream() : basic_ostream<char>(&_buf) {
	}
	explicit basic_ofstream(const char *path, ios_base::openmode mode = ios_base::out)
	    : basic_ostream<char>(&_buf) {
		open(path, mode);
	}
	explicit basic_ofstream(const string &path, ios_base::openmode mode = ios_base::out)
	    : basic_ofstream(path.c_str(), mode) {
	}
	~basic_ofstream() {
		_buf.close();
	}

	//! `fout = std::ofstream(name, binary)` is how llama-quant.cpp:702 opens
	//! each shard. The base's streambuf pointer stays aimed at our own _buf;
	//! only the open file and the stream state move.
	basic_ofstream(basic_ofstream &&other) : basic_ostream<char>(&_buf) {
		_buf = std::move(other._buf);
		clear(other.rdstate());
		other.clear();
	}
	basic_ofstream &operator=(basic_ofstream &&other) {
		if (this != &other) {
			_buf = std::move(other._buf);
			clear(other.rdstate());
			other.clear();
		}
		return *this;
	}

	void open(const char *path, ios_base::openmode mode = ios_base::out) {
		if (!_buf.open(path, mode | ios_base::out)) {
			setstate(ios_base::failbit);
		} else {
			clear();
		}
	}
	void open(const string &path, ios_base::openmode mode = ios_base::out) {
		open(path.c_str(), mode);
	}

	bool is_open() const {
		return _buf.is_open();
	}
	void close() {
		if (!_buf.close()) {
			setstate(ios_base::failbit);
		}
	}
	basic_filebuf<char> *rdbuf() const {
		return const_cast<basic_filebuf<char> *>(&_buf);
	}

private:
	basic_filebuf<char> _buf;
};

template <>
class basic_fstream<char, char_traits<char>> : public basic_iostream<char> {
public:
	basic_fstream() : basic_iostream<char>(&_buf) {
	}
	explicit basic_fstream(const char *path,
	                       ios_base::openmode mode = ios_base::in | ios_base::out)
	    : basic_iostream<char>(&_buf) {
		open(path, mode);
	}
	explicit basic_fstream(const string &path,
	                       ios_base::openmode mode = ios_base::in | ios_base::out)
	    : basic_fstream(path.c_str(), mode) {
	}

	void open(const char *path, ios_base::openmode mode = ios_base::in | ios_base::out) {
		if (!_buf.open(path, mode)) {
			setstate(ios_base::failbit);
		} else {
			clear();
		}
	}
	bool is_open() const {
		return _buf.is_open();
	}
	void close() {
		_buf.close();
	}
	basic_filebuf<char> *rdbuf() const {
		return const_cast<basic_filebuf<char> *>(&_buf);
	}

private:
	basic_filebuf<char> _buf;
};

} // namespace std
