/* The home-screen app, packed by tools/pack-app.py (GV_APP_PACK is its path). */
    .section .rodata
    .global gv_app_pack
    .global gv_app_pack_end
    .balign 16
gv_app_pack:
    .incbin GV_APP_PACK
gv_app_pack_end:
    .byte 0
    .section .note.GNU-stack,"",%progbits
