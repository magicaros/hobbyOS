10 REM ========================================================
20 REM hobbyOS BASIC System & Hardware Test Suite (Phase 7)
30 REM ========================================================
40 CLS
50 PRINT "hobbyOS BASIC System & Hardware Test Suite"
60 PRINT "=========================================="
70 PRINT

80 PRINT "--- 1. DEF SEG & PEEK ---"
90 DEF SEG = &H0040
100 VM% = PEEK(&H0049)
110 PRINT "BDA Video Mode (&H40:&H49): "; VM%; " (expected 3)"
120 DEF SEG = &HF000
130 ID% = PEEK(&HFFFE)
140 PRINT "BIOS Machine ID (&HF000:&HFFFE): "; ID%; " (expected 254 or 255)"
150 DEF SEG

160 PRINT
170 PRINT "--- 2. DEF SEG & POKE ---"
180 DEF SEG = &HB800
190 REM Save top-left character and attribute in CGA text VRAM
200 OLDCHAR% = PEEK(0)
210 OLDATTR% = PEEK(1)
220 REM Poke 'Z' (90) with Yellow on Blue (attribute 30)
230 POKE 0, 90
240 POKE 1, 30
250 NEWCHAR% = PEEK(0)
260 NEWATTR% = PEEK(1)
270 PRINT "Poked VRAM Character: "; CHR$(NEWCHAR%); " (code "; NEWCHAR%; ") Attr: "; NEWATTR%
280 REM Restore original character and attribute
290 POKE 0, OLDCHAR%
300 POKE 1, OLDATTR%
310 DEF SEG
320 PRINT "Restored VRAM successfully"

330 PRINT
340 PRINT "--- 3. Port I/O (INP & OUT) ---"
350 REM Port &H3DA is CGA Status Register
360 ST% = INP(&H3DA)
370 PRINT "CGA Status Port &H3DA: &H"; ST%; " (read OK)"
380 REM Port &H388 is AdLib/OPL FM Status Port
390 FM% = INP(&H388)
400 PRINT "AdLib Status Port &H388: &H"; FM%; " (read OK)"

410 PRINT
420 PRINT "--- 4. TIMER Function ---"
430 T1 = TIMER
440 PRINT "Start Time (sec since midnight): "; T1
450 FOR I% = 1 TO 500: NEXT I%
460 T2 = TIMER
470 PRINT "End Time after loop:             "; T2
480 ELAPSED = T2 - T1
490 PRINT "Elapsed seconds:                 "; ELAPSED; " (positive: "; (ELAPSED >= 0); ")"

500 PRINT
510 PRINT "--- 5. RANDOMIZE & RND ---"
520 RANDOMIZE 42
530 R1 = RND: R2 = RND
540 PRINT "Seed 42 -> R1: "; R1; "  R2: "; R2
550 RANDOMIZE 42
560 R3 = RND: R4 = RND
570 PRINT "Seed 42 -> R3: "; R3; "  R4: "; R4
580 IF R1 = R3 AND R2 = R4 THEN PRINT "PRNG Repeatability: PASSED" ELSE PRINT "PRNG Repeatability: FAILED"
590 IF R1 >= 0 AND R1 < 1 THEN PRINT "RND Range [0, 1):   PASSED" ELSE PRINT "RND Range [0, 1):   FAILED"

600 PRINT
610 PRINT "=========================================="
620 PRINT "ALL SYSTEM & HARDWARE TESTS COMPLETED!"
630 PRINT "=========================================="
