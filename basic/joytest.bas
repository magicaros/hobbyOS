10 REM hobbyOS Interactive Analog Joystick / Gameport Test
20 CLS: KEY OFF
30 LOCATE 1, 25: PRINT "hobbyOS Analog Joystick Test";
40 LOCATE 2, 26: PRINT "Port 0x201 - Gameport / SB16";
50 LOCATE 3, 25: PRINT "Press [Q] or [ESC] to Exit";

60 REM Draw Boxes for Joystick A and B
70 LOCATE 5, 8:  PRINT "+---------------------------+";
80 LOCATE 5, 46: PRINT "+---------------------------+";
90 FOR R = 6 TO 15
100   LOCATE R, 8:  PRINT "|                           |";
110   LOCATE R, 46: PRINT "|                           |";
120 NEXT R
130 LOCATE 16, 8:  PRINT "+---------------------------+";
140 LOCATE 16, 46: PRINT "+---------------------------+";
150 LOCATE 5, 16: PRINT "[ JOYSTICK A ]";
160 LOCATE 5, 54: PRINT "[ JOYSTICK B ]";

170 STRIG ON
180 OXA = 22: OYA = 10
190 OXB = 60: OYB = 10
200 HITA1 = 0: HITA2 = 0

210 REM --- Main Polling Loop ---
220 K$ = INKEY$
230 IF K$ = CHR$(27) OR K$ = "q" OR K$ = "Q" GOTO 700

240 REM Read all 4 axes (STICK(0) samples all 4)
250 XA = STICK(0): YA = STICK(1)
260 XB = STICK(2): YB = STICK(3)

270 REM Read buttons (1=Current, 0=Latched)
280 BA1 = STRIG(1): BA2 = STRIG(5)
290 BB1 = STRIG(3): BB2 = STRIG(7)
300 IF STRIG(0) THEN HITA1 = HITA1 + 1
310 IF STRIG(4) THEN HITA2 = HITA2 + 1

320 REM Numeric Readouts
330 LOCATE 18, 8:  PRINT "X:"; XA; "   Y:"; YA; "   ";
340 LOCATE 18, 46: PRINT "X:"; XB; "   Y:"; YB; "   ";

350 LOCATE 19, 8:  PRINT "Btn 1: ";
360 IF BA1 THEN PRINT "[DOWN] "; ELSE PRINT "[UP]   ";
370 LOCATE 20, 8:  PRINT "Btn 2: ";
380 IF BA2 THEN PRINT "[DOWN] "; ELSE PRINT "[UP]   ";
390 LOCATE 21, 8:  PRINT "Triggers: 1="; HITA1; " 2="; HITA2; "    ";

400 LOCATE 19, 46: PRINT "Btn 1: ";
410 IF BB1 THEN PRINT "[DOWN] "; ELSE PRINT "[UP]   ";
420 LOCATE 20, 46: PRINT "Btn 2: ";
430 IF BB2 THEN PRINT "[DOWN] "; ELSE PRINT "[UP]   ";

440 REM Calculate cursor positions inside 27x10 box
450 NXA = 9  + (XA * 26) / 200: NYA = 6 + (YA * 9) / 200
460 NXB = 47 + (XB * 26) / 200: NYB = 6 + (YB * 9) / 200
470 IF NXA < 9 THEN NXA = 9
475 IF NXA > 35 THEN NXA = 35
480 IF NYA < 6 THEN NYA = 6
485 IF NYA > 15 THEN NYA = 15
490 IF NXB < 47 THEN NXB = 47
495 IF NXB > 73 THEN NXB = 73
500 IF NYB < 6 THEN NYB = 6
505 IF NYB > 15 THEN NYB = 15

510 REM Redraw Joy A cursor if moved
520 IF NXA = OXA AND NYA = OYA GOTO 560
530 LOCATE OYA, OXA: PRINT " ";
540 LOCATE NYA, NXA: PRINT "O";
550 OXA = NXA: OYA = NYA

560 REM Redraw Joy B cursor if moved
570 IF NXB = OXB AND NYB = OYB GOTO 620
580 LOCATE OYB, OXB: PRINT " ";
590 LOCATE NYB, NXB: PRINT "O";
600 OXB = NXB: OYB = NYB

620 GOTO 210

700 REM --- Exit ---
710 STRIG OFF
720 CLS
730 PRINT "Joystick test finished."
740 SYSTEM
