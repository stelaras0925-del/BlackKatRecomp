Vector menu font (Vulkan renderer, PS2X_VK_FONT=imfell; launcher Remaster row: IM FELL)

imfell_menu.p2f          pre-rendered 8x atlas + glyph metrics for the front-end menu font
imfell_menu_preview.png  1/4-size preview (red/green = fill, blue = fill + outline)
IMFeENit28P.ttf          IM FELL English Italic, (c) 2007 Igino Marini, www.iginomarini.com
OFL.txt                  its licence: SIL Open Font License 1.1 (Reserved Font Name "IM FELL English Italic")

The .p2f is rendered output made from the font by tools/gen_menu_font.py (it is not a modified font).
Regenerate: python3 tools/gen_menu_font.py <front-end RAM dump> assets/fonts/IMFeENit28P.ttf assets/fonts/imfell_menu
  (defaults: --k 8 --outline 1.3 --match mean --xstretch 1.1 --place adv; each glyph is widened 10% and its
  advance box is centred in the game's advance box, because the game's advances are ~22% wider than IM FELL's)
(RAM dump: BLACKKAT_RAMDUMP=<sec> while the main menu is on screen; needs Pillow + numpy.)

