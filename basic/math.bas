10 REM ========================================================
20 REM hobbyOS BASIC Floating-Point & Math Engine Test Suite
30 REM ========================================================
40 CLS
50 PRINT "hobbyOS BASIC Math Engine Test Suite"
60 PRINT "===================================="
70 PRINT "Free Memory: "; FRE(0); " Bytes"
80 PRINT
90 PRINT "--- 1. Arithmetic & Division ---"
100 PRINT "10 / 4     = "; 10 / 4; "  (expected  2.5)"
110 PRINT "10 \ 4     = "; 10 \ 4; "  (expected  2)"
120 PRINT "10 MOD 4   = "; 10 MOD 4; "  (expected  2)"
130 PRINT "2.5 * 4    = "; 2.5 * 4; "  (expected  10)"
140 PRINT "3.14 + 1.86= "; 3.14 + 1.86; "  (expected  5)"
150 PRINT "10.5 - 12  = "; 10.5 - 12; "  (expected -1.5)"
160 PRINT
170 PRINT "--- 2. Powers & Roots ---"
180 PRINT "2 ^ 10     = "; 2 ^ 10; "  (expected  1024)"
190 PRINT "3 ^ 3      = "; 3 ^ 3; "  (expected  27)"
200 PRINT "SQR(2)     = "; SQR(2); "  (expected  1.41421)"
210 PRINT "SQR(64)    = "; SQR(64); "  (expected  8)"
220 PRINT "9 ^ 0.5    = "; 9 ^ 0.5; "  (expected  3)"
230 PRINT
240 PRINT "--- 3. Transcendental Functions ---"
250 PI = 3.141593
260 PRINT "SIN(PI/2)  = "; SIN(PI / 2); "  (expected  1)"
270 PRINT "COS(0)     = "; COS(0); "  (expected  1)"
280 PRINT "TAN(PI/4)  = "; TAN(PI / 4); "  (expected  1)"
290 PRINT "ATN(1) * 4 = "; ATN(1) * 4; "  (expected  3.14159)"
300 PRINT "EXP(1)     = "; EXP(1); "  (expected  2.71828)"
310 PRINT "LOG(EXP(1))= "; LOG(EXP(1)); "  (expected  1)"
320 PRINT
330 PRINT "--- 4. Conversion & Rounding ---"
340 PRINT "INT(3.75)  = "; INT(3.75); "  (expected  3)"
350 PRINT "FIX(-3.75) = "; FIX(-3.75); "  (expected -3)"
360 PRINT "ABS(-42.5) = "; ABS(-42.5); "  (expected  42.5)"
370 PRINT "SGN(-99.9) = "; SGN(-99.9); "  (expected -1)"
380 PRINT
390 PRINT "--- 5. Float Array & Loop ---"
400 DIM A(5)
410 FOR I = 1 TO 5
420 A(I) = I * 1.5
430 NEXT I
440 FOR I = 1 TO 5
450 PRINT "A("; I; ") = "; A(I); "  ";
460 NEXT I
470 PRINT
480 PRINT
490 PRINT "===================================="
500 PRINT "ALL MATH TESTS COMPLETED!"
510 PRINT "===================================="
520 SYSTEM
