#include "test_setup.S"

.section .text

# Test Description :
# \__________________
#
# Executes in: domain 1
# Accesses:    domain 0
# Should:      fail
# 
# chdom should NOT be used between domain 1
# and domain 0 (code error)
#


.section .text.dom0_code

test_start:
    /* Jump to domain 1 */
    la t1, test_start_dom1
    chdom
    ret

.section .text.dom1_code

test_start_dom1:
    /* Load the address of the domain 1 data */
    la  t1, test_start
    chdom
    retdom