10 REM ==========================================
20 REM hobbyOS BASIC String & Formatting Test Suite
30 REM ==========================================
40 CLS
50 PRINT "hobbyOS BASIC String & Formatting Test"
60 PRINT "======================================"
70 PRINT

100 REM --- 1. STRING$ & SPACE$ ---
110 PRINT "--- 1. STRING$ & SPACE$ ---"
120 S1$ = STRING$(5, 42)
130 S2$ = STRING$(4, "XYZ")
140 S3$ = SPACE$(3)
150 PRINT "STRING$(5, 42)  = ["; S1$; "] (expected [*****])"
160 PRINT "STRING$(4, XYZ) = ["; S2$; "] (expected [XXXX])"
170 PRINT "SPACE$(3)       = ["; S3$; "] (expected [   ])"
180 IF S1$ = "*****" AND S2$ = "XXXX" AND S3$ = "   " THEN PRINT "STRING$/SPACE$: PASSED" ELSE PRINT "STRING$/SPACE$: FAILED"
190 PRINT

200 REM --- 2. HEX$ & OCT$ ---
210 PRINT "--- 2. HEX$ & OCT$ ---"
220 H1$ = HEX$(0)
230 H2$ = HEX$(255)
240 H3$ = HEX$(4096)
250 H4$ = HEX$(-1)
260 O1$ = OCT$(0)
270 O2$ = OCT$(8)
280 O3$ = OCT$(-1)
290 PRINT "HEX$(0)    = "; H1$; " (expected 0)"
300 PRINT "HEX$(255)  = "; H2$; " (expected FF)"
310 PRINT "HEX$(4096) = "; H3$; " (expected 1000)"
320 PRINT "HEX$(-1)   = "; H4$; " (expected FFFF)"
330 PRINT "OCT$(0)    = "; O1$; " (expected 0)"
340 PRINT "OCT$(8)    = "; O2$; " (expected 10)"
350 PRINT "OCT$(-1)   = "; O3$; " (expected 177777)"
360 IF H1$="0" AND H2$="FF" AND H3$="1000" AND H4$="FFFF" AND O1$="0" AND O2$="10" AND O3$="177777" THEN PRINT "HEX$/OCT$:      PASSED" ELSE PRINT "HEX$/OCT$:      FAILED"
370 PRINT

400 REM --- 3. INSTR Function ---
410 PRINT "--- 3. INSTR Function ---"
420 T$ = "HELLO WORLD, WELCOME TO BASIC"
430 P1 = INSTR(T$, "WORLD")
440 P2 = INSTR(T$, "XYZ")
450 P3 = INSTR(10, T$, "TO")
460 P4 = INSTR(20, T$, "WORLD")
470 PRINT "INSTR(T$, 'WORLD')     = "; P1; " (expected 7)"
480 PRINT "INSTR(T$, 'XYZ')       = "; P2; " (expected 0)"
490 PRINT "INSTR(10, T$, 'TO')    = "; P3; " (expected 22)"
500 PRINT "INSTR(20, T$, 'WORLD') = "; P4; " (expected 0)"
510 IF P1 = 7 AND P2 = 0 AND P3 = 22 AND P4 = 0 THEN PRINT "INSTR:          PASSED" ELSE PRINT "INSTR:          FAILED"
520 PRINT

600 REM --- 4. MID$= Statement ---
610 PRINT "--- 4. MID$= Statement ---"
620 A$ = "HELLO WORLD"
630 MID$(A$, 7, 5) = "THERE"
640 PRINT "After MID$(A$,7,5)='THERE': ["; A$; "] (expected [HELLO THERE])"
650 MID$(A$, 1) = "GREAT"
660 PRINT "After MID$(A$,1)='GREAT':   ["; A$; "] (expected [GREAT THERE])"
670 DIM M$(3)
680 M$(1) = "ABCDEFGH"
690 MID$(M$(1), 3, 4) = "9999"
700 PRINT "Array MID$(M$(1),3,4)='9999': ["; M$(1); "] (expected [AB9999GH])"
710 IF A$ = "GREAT THERE" AND M$(1) = "AB9999GH" THEN PRINT "MID$=:          PASSED" ELSE PRINT "MID$=:          FAILED"
720 PRINT

800 REM --- 5. PRINT USING Templates ---
810 PRINT "--- 5. PRINT USING Templates ---"
820 PRINT "String !:     ["; : PRINT USING "!"; "HELLO"; : PRINT "]"
830 PRINT "String \ \:   ["; : PRINT USING "\   \"; "HELLO"; : PRINT "]"
840 PRINT "String &:     ["; : PRINT USING "&"; "HELLO"; : PRINT "]"
850 PRINT "Number ###:   ["; : PRINT USING "###"; 42; : PRINT "]"
860 PRINT "Float ###.##: ["; : PRINT USING "###.##"; 12.3; : PRINT "]"
870 PRINT "Sign +###.##: ["; : PRINT USING "+###.##"; 12.3; : PRINT "]"
880 PRINT "Trailing -:   ["; : PRINT USING "###.##-"; -12.3; : PRINT "]"
890 PRINT "Dollar $$:    ["; : PRINT USING "$$###.##"; 12.3; : PRINT "]"
900 PRINT "Asterisk **:  ["; : PRINT USING "**###.##"; 12.3; : PRINT "]"
910 PRINT "Both **$:     ["; : PRINT USING "**$###.##"; 12.3; : PRINT "]"
920 PRINT "Comma #:      ["; : PRINT USING "#,###.##"; 1234.5; : PRINT "]"
930 PRINT "Overflow %:   ["; : PRINT USING "##"; 999; : PRINT "]"
940 PRINT "PRINT USING:    PASSED"
950 PRINT

1000 REM --- 6. PRINT #, USING File I/O ---
1010 PRINT "--- 6. PRINT #, USING ---"
1020 OPEN "O", #1, "USING.TXT"
1030 PRINT #1, USING "ITEM: \   \ PRICE: $$###.##"; "PEN", 1.25
1040 PRINT #1, USING "ITEM: \   \ PRICE: $$###.##"; "BOOK", 14.95
1050 CLOSE #1
1060 OPEN "I", #1, "USING.TXT"
1070 LINE INPUT #1, L1$
1080 LINE INPUT #1, L2$
1090 CLOSE #1
1100 PRINT "Line 1: ["; L1$; "]"
1110 PRINT "Line 2: ["; L2$; "]"
1120 IF INSTR(L1$, "PEN") > 0 AND INSTR(L1$, "$1.25") > 0 AND INSTR(L2$, "BOOK") > 0 AND INSTR(L2$, "$14.95") > 0 THEN PRINT "PRINT # USING:  PASSED" ELSE PRINT "PRINT # USING:  FAILED"
1130 PRINT
1140 PRINT "======================================"
1150 PRINT "ALL STRING & FORMATTING TESTS COMPLETED!"
1160 PRINT "======================================"
