#include "sp_http_assets.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(sp_http_accepts_gzip("gzip, deflate, br"));
    assert(sp_http_accepts_gzip("br, GZip; q=0.5"));
    assert(sp_http_accepts_gzip("*;q=1"));
    assert(!sp_http_accepts_gzip("gzip;q=0, *;q=1"));
    assert(!sp_http_accepts_gzip("gzip;q=0.000"));
    assert(!sp_http_accepts_gzip("gzip;q=1.1"));
    assert(!sp_http_accepts_gzip("gzip;q=0x1"));
    assert(!sp_http_accepts_gzip("gzip;q=1oops"));
    assert(!sp_http_accepts_gzip("gzip;q=0.0001"));
    assert(!sp_http_accepts_gzip("br"));
    assert(!sp_http_accepts_gzip(NULL));
    assert(sp_http_etag_matches("W/\"abc\"", "\"abc\""));
    assert(sp_http_etag_matches("\"old\", \"abc\"", "\"abc\""));
    assert(sp_http_etag_matches("*", "\"abc\""));
    assert(!sp_http_etag_matches("\"old\"", "\"abc\""));
    assert(!sp_http_etag_matches("\"abc-gzip\"", "\"abc\""));
    puts("HTTP asset encoding negotiation and conditional GET passed");
}
