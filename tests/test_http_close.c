/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Host unit tests for HTTP Connection header parser (http_close.h).
 * Tickets 5f45856dfe and d20c2280d4.
 *
 * Verifies that ukg_http_wants_close matches "close" as a whole token,
 * ignores case, accepts comma-separated tokens, and rejects header name
 * prefixes like Proxy-Connection and value prefixes like closed.
 */

#include <stdio.h>
#include <assert.h>
#include <http_close.h>

static void test_exact_matches(void)
{
	assert(ukg_http_wants_close("Connection: close") == 1);
	assert(ukg_http_wants_close("Connection: close\r\n") == 1);
	assert(ukg_http_wants_close("Connection: close\n") == 1);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nConnection: close\r\n\r\n") == 1);
	assert(ukg_http_wants_close("GET / HTTP/1.1\nConnection: close\n\n") == 1);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n") == 1);
}

static void test_whitespace_and_delimiters(void)
{
	assert(ukg_http_wants_close("Connection:close") == 1);
	assert(ukg_http_wants_close("Connection:  close  ") == 1);
	assert(ukg_http_wants_close("Connection:\tclose\t") == 1);
	assert(ukg_http_wants_close("Connection: \t close \t\r\n") == 1);
	assert(ukg_http_wants_close("Connection: ,,, close ,,, ") == 1);
	assert(ukg_http_wants_close("Connection: close,") == 1);
	assert(ukg_http_wants_close("Connection: ,close") == 1);
}

static void test_comma_separated_tokens(void)
{
	assert(ukg_http_wants_close("Connection: keep-alive, close") == 1);
	assert(ukg_http_wants_close("Connection: keep-alive,close") == 1);
	assert(ukg_http_wants_close("Connection: close, keep-alive") == 1);
	assert(ukg_http_wants_close("Connection: close,keep-alive") == 1);
	assert(ukg_http_wants_close("Connection: foo, bar, close, baz") == 1);
	assert(ukg_http_wants_close("Connection: foo, bar, close") == 1);
	assert(ukg_http_wants_close("Connection: close, foo, bar") == 1);
	assert(ukg_http_wants_close("Connection: a,b,c,d,close") == 1);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n") == 1);
}

static void test_case_variation(void)
{
	assert(ukg_http_wants_close("connection: close") == 1);
	assert(ukg_http_wants_close("CONNECTION: CLOSE") == 1);
	assert(ukg_http_wants_close("Connection: CLOSE") == 1);
	assert(ukg_http_wants_close("CoNnEcTiOn: ClOsE") == 1);
	assert(ukg_http_wants_close("Connection: keep-alive, ClOsE") == 1);
	assert(ukg_http_wants_close("CONNECTION: KEEP-ALIVE, CLOSE") == 1);
}

static void test_multiple_connection_headers(void)
{
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n") == 1);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nConnection: close\r\nConnection: keep-alive\r\n\r\n") == 1);
}

static void test_rejected_headers(void)
{
	/* Proxy-Connection and X-Connection must not match Connection */
	assert(ukg_http_wants_close("Proxy-Connection: close") == 0);
	assert(ukg_http_wants_close("X-Connection: close") == 0);
	assert(ukg_http_wants_close("My-Connection: close") == 0);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nProxy-Connection: close\r\n\r\n") == 0);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nProxy-Connection: close\r\nConnection: keep-alive\r\n\r\n") == 0);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nX-Connection: close\r\n\r\n") == 0);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nX-Connection: close\r\nConnection: keep-alive\r\n\r\n") == 0);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nX-Header: Connection: close\r\n\r\n") == 0);

	/* But if a real Connection: close is also present, it matches */
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nProxy-Connection: close\r\nConnection: close\r\n\r\n") == 1);
}

static void test_rejected_values(void)
{
	/* Value tokens starting with or containing close must not match */
	assert(ukg_http_wants_close("Connection: closed") == 0);
	assert(ukg_http_wants_close("Connection: closer") == 0);
	assert(ukg_http_wants_close("Connection: is-closed") == 0);
	assert(ukg_http_wants_close("Connection: close-it") == 0);
	assert(ukg_http_wants_close("Connection: notclose") == 0);
	assert(ukg_http_wants_close("Connection: enclosure") == 0);
	assert(ukg_http_wants_close("Connection: keep-alive, closed") == 0);
	assert(ukg_http_wants_close("Connection: closed, keep-alive") == 0);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nConnection: closed\r\n\r\n") == 0);

	/* Other connection values */
	assert(ukg_http_wants_close("Connection: keep-alive") == 0);
	assert(ukg_http_wants_close("Connection: upgrade") == 0);
	assert(ukg_http_wants_close("Connection:") == 0);
	assert(ukg_http_wants_close("Connection: ") == 0);
	assert(ukg_http_wants_close("Connection: \t") == 0);
	assert(ukg_http_wants_close("Connection: ,,, ") == 0);
	assert(ukg_http_wants_close("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n") == 0);
	assert(ukg_http_wants_close("") == 0);
	assert(ukg_http_wants_close(NULL) == 0);
}

int main(void)
{
	test_exact_matches();
	test_whitespace_and_delimiters();
	test_comma_separated_tokens();
	test_case_variation();
	test_multiple_connection_headers();
	test_rejected_headers();
	test_rejected_values();
	printf("test_http_close: all unit tests passed\n");
	return 0;
}
