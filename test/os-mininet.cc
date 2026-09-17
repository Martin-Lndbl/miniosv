/*
 * mininet tests, runnable with no NIC: the Rust stack's own unit tests through
 * mininet_selftest(), then the C++ ABI surface and mininet_get()'s argument checks.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "modules/mininet/mininet.hh"

extern "C" {
// The Rust side's unit tests: returns how many checks failed.
uint32_t mininet_selftest(int verbose);
int mininet_get(const char *head, uint64_t head_len, void *buf, uint64_t cap, void *out);
int mininet_is_up(void);
const char *mininet_strerror(int rc);
}

namespace {

int failures;

void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		printf("  FAIL %s\n", what);
	}
}

// Argument checks come before any dereference, and "not up" before everything.
void test_get_argument_validation()
{
	printf("-- mininet_get argument validation\n");

	check(mininet_is_up() == 0, "stack is not up without a NIC");

	char buf[16];
	mininet::response r {};

	check(mininet_get(nullptr, 0, buf, sizeof(buf), &r) == mininet::E_NOT_UP,
	      "null head on a down stack reports not-up, does not fault");
	check(mininet_get("GET / HTTP/1.1\r\n\r\n", 18, nullptr, 0, &r) == mininet::E_NOT_UP,
	      "HEAD-shaped call on a down stack reports not-up");
	check(mininet_get("GET / HTTP/1.1\r\n\r\n", 18, nullptr, 64, &r) == mininet::E_NOT_UP,
	      "null buffer with nonzero cap on a down stack reports not-up");
}

// Every error code maps to a distinct, non-empty string.
void test_strerror()
{
	printf("-- strerror\n");

	const int codes[] = {
		mininet::OK,          mininet::E_NO_DEVICE,   mininet::E_NO_MEMORY,
		mininet::E_RSS,       mininet::E_DHCP,        mininet::E_ARP,
		mininet::E_NO_PORTS,  mininet::E_CONNECT,     mininet::E_SYN_TIMEOUT,
		mininet::E_TLS,       mininet::E_BAD_RESPONSE, mininet::E_BUFFER_TOO_SMALL,
		mininet::E_NOT_UP,    mininet::E_BAD_ARGUMENT,
	};
	const size_t n = sizeof(codes) / sizeof(codes[0]);

	for (size_t i = 0; i < n; i++) {
		const char *s = mininet::strerror(codes[i]);
		check(s != nullptr && s[0] != '\0', "every code has a message");
		for (size_t j = i + 1; j < n; j++) {
			if (s && strcmp(s, mininet::strerror(codes[j])) == 0) {
				printf("  FAIL codes %d and %d share \"%s\"\n",
				       codes[i], codes[j], s);
				failures++;
			}
		}
	}
	check(strcmp(mininet::strerror(12345), "unknown error") == 0,
	      "an unknown code is reported as unknown");
}

// The fixed char arrays must be NUL-terminated and sized as the header promises.
void test_response_abi()
{
	printf("-- response ABI\n");

	check(mininet::ETAG_MAX == 128, "ETAG_MAX matches capi.rs");
	check(mininet::DATE_MAX == 64, "DATE_MAX matches capi.rs");
	check(sizeof(mininet::response {}.etag) == mininet::ETAG_MAX, "etag is ETAG_MAX bytes");
	check(sizeof(mininet::response {}.last_modified) == mininet::DATE_MAX,
	      "last_modified is DATE_MAX bytes");

	mininet::response r {};
	check(r.etag[0] == '\0', "a zeroed response has an empty etag");
	check(r.last_modified[0] == '\0', "a zeroed response has an empty last_modified");
	check(r.status == 0 && r.bytes == 0 && r.has_range == 0,
	      "a zeroed response has no status, bytes or range");
}

// Without a stack up every counter must be a clean zero.
void test_stats_zeroed()
{
	printf("-- conn_stats\n");

	mininet::conn_stats c = mininet::stats();
	check(c.requests_served == 0, "no requests served before the stack is up");
	check(c.requests_reused == 0, "no requests reused before the stack is up");
	check(c.requests_reused <= c.requests_served, "reused never exceeds served");
}

} // namespace

int os_mininet_main()
{
	printf("---- mininet network stack ----\n");
	failures = 0;

	uint32_t rust_failures = mininet_selftest(0);

	test_get_argument_validation();
	test_strerror();
	test_response_abi();
	test_stats_zeroed();

	int total = static_cast<int>(rust_failures) + failures;
	printf("---- mininet: %s (%u rust, %d c++ failures) ----\n",
	       total ? "FAILURE" : "ok", rust_failures, failures);
	return total ? 1 : 0;
}
