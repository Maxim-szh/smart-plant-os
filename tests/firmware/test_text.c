#include "sp_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    assert(sp_text_valid("", 0, 80));
    assert(!sp_text_valid("", 1, 80));
    assert(sp_text_valid("Монстера 🌱", 1, 80));
    assert(!sp_text_valid("🌱", 1, 3));
    assert(!sp_text_valid(NULL, 0, 80));
    assert(!sp_text_valid("\xc0\xaf", 0, 80));
    assert(!sp_text_valid("\xed\xa0\x80", 0, 80));
    assert(!sp_text_valid("\xf4\x90\x80\x80", 0, 80));
    assert(!sp_text_valid("\xe2\x82", 0, 80));
    assert(!sp_text_valid("\xc2\x80", 0, 80));
    assert(!sp_text_valid("name\n", 0, 80));
    assert(!sp_text_valid("\x7f", 0, 80));
    puts("PASS: UTF-8 byte limits, overlong encoding, surrogate, truncation and control rejection");
}
