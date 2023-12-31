#include "pmp_dmp_setup.S"

# Note: pmp_setup expects to jump to a user-defined function named
#       test_start once it switched to user-mode!

.global main

.section .text

main:
    la s2, data_region0    # Load the data region 0 address in s2
    la s3, data_region1    #       -              1      -     s3
    call pmp_setup         # Call the generated binary
    li a0, 0               # Pass 0 to the exit function
    call exit              # 

.section .data

.section .data.dom1_data

data_region1:
    .incbin "data/data.bin"

.section .data.dom0_data

data_region0:
    .incbin "data/data.bin"

.section .data.dom2_data

data_region2:
    .incbin "data/data.bin"
