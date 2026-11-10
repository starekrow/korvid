#include "korvid/core/memory.h"
#include "korvid/core/test.h"

void test_xml(kv::TestCtx&);
void test_xml_fuzz(kv::TestCtx&);

int main() {
    kv::TestCtx c;
    test_xml(c);
    test_xml_fuzz(c);
    int rc = kv::test_report(c, "xml_tests");
    kv::mem_shutdown();
    return rc;
}
