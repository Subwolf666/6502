// demonstrate that the v flag works as described
//
// returns with error = 0 if the test passes, error = 1 if the test fails
//
// five (additional) memory locations are used: error, s1, s2, u1, and u2
// which can be located anywhere convenient in ram
//
:BasicUpstart2(test)

test:
     cld       // clear decimal mode (just in case) for test
     lda #1
     sta error // store 1 in error until test passes
     lda #$80
     sta s1    // initalize s1 and s2 to -128 ($80)
     sta s2
     lda #0
     sta u1    // initialize u1 and u2 to 0
     sta u2
     ldy #1    // initialize y (used to set and clear the carry flag) to 1
loop:
     jsr add   // test adc
     cpx #1
     beq done  // end if v and unsigned result do not agree (x = 1)
     jsr sub   // test sbc
     cpx #1
     beq done  // end if v and unsigned result do not agree (x = 1)
     inc s1
     inc u1
     bne loop  // loop until all 256 possibilities of s1 and u1 are tested
     inc s2
     inc u2
     bne loop  // loop until all 256 possibilities of s2 and u2 are tested
     dey
     bpl loop  // loop until both possiblities of the carry flag are tested
     lda #0
     sta error // all tests pass, so store 0 in error
done:
     rts
//
// test adc
//
// x is initialized to 0
// x is incremented when v = 1
// x is incremented when the unsigned result predicts an overflow
// therefore, if the v flag and the unsigned result agree, x will be
// incremented zero or two times (returning x = 0 or x = 2), and if they do
// not agree x will be incremented once (returning x = 1)
//
add:
     cpy #1   // set carry when y = 1, clear carry when y = 0
     lda s1   // test twos complement addition
     adc s2
     ldx #0   // initialize x to 0
     bvc add1
     inx      // increment x if v = 1
add1:
     cpy #1   // set carry when y = 1, clear carry when y = 0
     lda u1   // test unsigned addition
     adc u2
     bcs add3 // carry is set if u1 + u2 >= 256
     bmi add2 // u1 + u2 < 256, a >= 128 if u1 + u2 >= 128
     inx      // increment x if u1 + u2 < 128
add2:
     rts
add3:
     bpl add4 // u1 + u2 >= 256, a <= 127 if u1 + u2 <= 383 ($17f)
     inx      // increment x if u1 + u2 > 383
add4:
     rts
//
// test sbc
//
// x is initialized to 0
// x is incremented when v = 1
// x is incremented when the unsigned result predicts an overflow
// therefore, if the v flag and the unsigned result agree, x will be
// incremented zero or two times (returning x = 0 or x = 2), and if they do
// not agree x will be incremented once (returning x = 1)
//
sub:
     cpy #1   // set carry when y = 1, clear carry when y = 0
     lda s1   // test twos complement subtraction
     sbc s2
     ldx #0   // initialize x to 0
     bvc sub1
     inx      // increment x if v = 1
sub1:
     cpy #1   // set carry when y = 1, clear carry when y = 0
     lda u1   // test unsigned subtraction
     sbc u2
     pha      // save the low byte of result on the stack
     lda #$ff
     sbc #$00 // result = (65280 + u1) - u2, 65280 = $ff00
     cmp #$fe
     bne sub4 // branch if result >= 65280 ($ff00) or result < 65024 ($fe00)
     pla      // get the low byte of result
     bmi sub3 // result < 65280 ($ff00), a >= 128 if result >= 65152 ($fe80)
sub2:
     inx      // increment x if result < 65152 ($fe80)
sub3:
     rts
sub4:
     pla      // get the low byte of result (does not affect the carry flag)
     bcc sub2 // the carry flag is clear if result < 65024 ($fe00)
     bpl sub5 // result >= 65280 ($ff00), a <= 127 if result <= 65407 ($ff7f)
     inx      // increment x if result > 65407 ($ff7f)
sub5:
     rts

error:
     .byte 0
s1:
     .byte 0
s2:
     .byte 0
u1:
     .byte 0
u2:
     .byte 0
