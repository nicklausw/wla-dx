.MEMORYMAP
    DEFAULTSLOT 0
    SLOTSIZE $100
    SLOT 0 $100
.ENDME 

.ROMBANKSIZE 100
.ROMBANKS 1

.BANK 0
.ORG 0

; @BT linked.rom

.db "01>"   ; @BT TEST-01 01 START
LDX $2A     ; @BT 9E 2A
LDA $2A     ; @BT 96 2A

.16BIT

LDX $2A     ; @BT BE 00 2A
LDA $2A     ; @BT B6 00 2A
.db "<01"   ; @BT END
