# Base Load/Store tests between domains

The following table works as a recap the tests:

Base loads/stores:
```
╔═════════════╦═════════════╦═════════════╦═══════════════╗
║ Instruction ║ Code Domain ║ Data Domain ║    Should     ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣ 
║    l*/s*    ║      0      ║      0      ║     PASS      ║ 
║    l*/s*    ║      1      ║      0      ║     PASS      ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║    l*/s*    ║      0      ║      1      ║  FAIL (data)  ║
║    l*/s*    ║      0      ║      2      ║  FAIL (data)  ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║    l*/s*    ║      1      ║      1      ║  FAIL (data)  ║
║    l*/s*    ║      1      ║      2      ║  FAIL (data)  ║
╚═════════════╩═════════════╩═════════════╩═══════════════╝
```

Duplicated load/stores:
```
╔═════════════╦═════════════╦═════════════╦═══════════════╗
║ Instruction ║ Code Domain ║ Data Domain ║    Should     ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   l*1/s*1   ║      1      ║      1      ║     PASS      ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   l*1/s*1   ║      0      ║      0      ║  FAIL (code)  ║
║   l*1/s*1   ║      0      ║      1      ║  FAIL (code)  ║
║   l*1/s*1   ║      0      ║      2      ║  FAIL (code)  ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   l*1/s*1   ║      1      ║      0      ║  FAIL (data)  ║
║   l*1/s*1   ║      1      ║      2      ║  FAIL (data)  ║
╚═════════════╩═════════════╩═════════════╩═══════════════╝
```

Shadow-stack load/stores:
```
╔═════════════╦═════════════╦═════════════╦═══════════════╗
║ Instruction ║ Code Domain ║ Data Domain ║    Should     ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   lst/sst   ║      1      ║      2      ║     PASS      ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   lst/sst   ║      0      ║      0      ║  FAIL (code)  ║
║   lst/sst   ║      0      ║      1      ║  FAIL (code)  ║
║   lst/sst   ║      0      ║      2      ║  FAIL (code)  ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   lst/sst   ║      1      ║      0      ║  FAIL (data)  ║
║   lst/sst   ║      1      ║      1      ║  FAIL (data)  ║
╚═════════════╩═════════════╩═════════════╩═══════════════╝
```

Domain change:
```
╔═════════════╦═════════════╦═════════════╦═══════════════╗
║ Instruction ║ Code Domain ║ Data Domain ║    Should     ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║    chdom    ║      0      ║      1      ║ PASS (+flush) ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║    chdom    ║      0      ║      0      ║  FAIL (data)  ║
║    chdom    ║      0      ║      2      ║  FAIL (data)  ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║    chdom    ║      1      ║      0      ║  FAIL (code)  ║
║    chdom    ║      1      ║      1      ║  FAIL (code)  ║
║    chdom    ║      1      ║      2      ║  FAIL (code)  ║
╚═════════════╩═════════════╩═════════════╩═══════════════╝
```

Domain return:
```
╔═════════════╦═════════════╦═════════════╦═══════════════╗
║ Instruction ║ Code Domain ║ Data Domain ║    Should     ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   retdom    ║      1      ║      0      ║ PASS (+flush) ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   retdom    ║      1      ║      1      ║  FAIL (data)  ║
║   retdom    ║      1      ║      2      ║  FAIL (data)  ║
╠═════════════╬═════════════╬═════════════╬═══════════════╣
║   retdom    ║      0      ║      0      ║  FAIL (code)  ║
║   retdom    ║      0      ║      1      ║  FAIL (code)  ║
║   retdom    ║      0      ║      2      ║  FAIL (code)  ║
╚═════════════╩═════════════╩═════════════╩═══════════════╝
```

Config CSR:
```
╔═════════════╦═════════════╦═════════════╗
║   PMPCFG    ║   DMPCFG    ║     EXC     ║
╠═════════════╬═════════════╬═════════════╣
║    FAIL     ║    FAIL     ║ RAISE (PMP) ║
║    FAIL     ║    PASS     ║ RAISE (PMP) ║
║    PASS     ║    FAIL     ║ RAISE (DMP) ║
║    PASS     ║    PASS     ║    PASS     ║
╚═════════════╩═════════════╩═════════════╝
```