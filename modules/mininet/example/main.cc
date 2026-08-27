/*
 * A C++ application that fetches a byte range over mininet.
 *
 *     make app=modules/mininet/example \
 *         MININET_HOST=bucket.s3.eu-north-1.amazonaws.com \
 *         MININET_ADDR=3.5.216.240
 *
 * This is what app/miniduckdb's HTTP client will look like from the outside:
 * bring the stack up once, then call get() from whatever thread wants bytes.
 * It exists to exercise mininet.hh without a DuckDB build in the way -- a
 * broken C ABI is much easier to read here than inside a query.
 *
 * The object is assumed to be the benchmark's blob.bin. Nothing here needs it
 * to be, except the paths below.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/mininet/mininet.hh"

#ifndef MININET_HOST
#define MININET_HOST "example.invalid"
#endif
#ifndef MININET_ADDR
#define MININET_ADDR "0.0.0.0"
#endif
#ifndef MININET_PATH
#define MININET_PATH "/blob.bin"
#endif

namespace {

const size_t K = 1024;

int render_range_get(char *buf, size_t cap, uint64_t start, uint64_t end)
{
	return snprintf(buf, cap,
	                "GET %s HTTP/1.1\r\n"
	                "Host: %s\r\n"
	                "User-Agent: mininet-example/0.1\r\n"
	                "Range: bytes=%llu-%llu\r\n"
	                "Connection: close\r\n"
	                "\r\n",
	                MININET_PATH, MININET_HOST,
	                static_cast<unsigned long long>(start),
	                static_cast<unsigned long long>(end));
}

//! Fetch [start, end] into `out`. Returns true and fills `out` on success.
bool fetch(uint64_t start, uint64_t end, std::vector<uint8_t> &out)
{
	char head[512];
	int n = render_range_get(head, sizeof(head), start, end);
	if (n <= 0 || static_cast<size_t>(n) >= sizeof(head)) {
		printf("  request head did not fit\n");
		return false;
	}

	const uint64_t want = end - start + 1;
	out.assign(want, 0);

	mininet::response r{};
	int rc = mininet::get(head, static_cast<size_t>(n), out.data(), out.size(), &r);
	if (rc != mininet::OK) {
		printf("  get(%llu-%llu) failed: %s\n",
		       static_cast<unsigned long long>(start),
		       static_cast<unsigned long long>(end), mininet::strerror(rc));
		return false;
	}
	if (r.status != 206) {
		printf("  status %u, wanted 206\n", r.status);
		return false;
	}
	if (r.bytes != want || r.content_length != want) {
		printf("  got %llu bytes, content-length %llu, wanted %llu\n",
		       static_cast<unsigned long long>(r.bytes),
		       static_cast<unsigned long long>(r.content_length),
		       static_cast<unsigned long long>(want));
		return false;
	}
	return true;
}

int failures = 0;

void check(const char *what, bool ok)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

} // namespace

extern "C" void osv_app_main()
{
	printf("\n######## mininet from C++ ########\n\n");

	mininet::config cfg{};
	cfg.host = MININET_HOST;
	cfg.address = MININET_ADDR;
	cfg.tls = 1;
	cfg.workers = 2;
	cfg.conns_per_worker = 4;
	cfg.rx_buffer = 256 * K;

	printf("target: %s at %s\n", cfg.host, cfg.address);

	int rc = mininet::up(cfg);
	if (rc != mininet::OK) {
		printf("FAIL: mininet::up: %s\n", mininet::strerror(rc));
		while (true) {
			asm volatile("" ::: "memory");
		}
	}
	check("mininet::up brings the stack up", mininet::is_up());

	std::vector<uint8_t> a, b;
	check("a ranged GET delivers its bytes", fetch(0, 64 * K - 1, a));

	// The same assertion the Rust selftest makes, and for the same reason: a
	// byte count cannot tell one 64 KiB from another, so the only way to know
	// the bytes belong to the offset asked for is to overlap two ranges.
	check("an overlapping range agrees on the overlap",
	      fetch(32 * K, 64 * K - 1, b) && a.size() == 64 * K && b.size() == 32 * K &&
	          memcmp(a.data() + 32 * K, b.data(), 32 * K) == 0);

	// A buffer that cannot hold the response has to fail, not truncate.
	{
		char head[512];
		int n = render_range_get(head, sizeof(head), 0, 64 * K - 1);
		std::vector<uint8_t> small(K, 0);
		mininet::response r{};
		int small_rc = mininet::get(head, static_cast<size_t>(n), small.data(),
		                            small.size(), &r);
		check("a too-small buffer reports E_BUFFER_TOO_SMALL",
		      small_rc == mininet::E_BUFFER_TOO_SMALL);
	}

	printf("\n");
	if (failures == 0) {
		printf("COMPLETE: mininet.hh works from C++\n");
	} else {
		printf("INCOMPLETE: %d check(s) failed\n", failures);
	}

	// Do not power off: keep the serial output visible on the console.
	while (true) {
		asm volatile("" ::: "memory");
	}
}
