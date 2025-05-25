idf_component_register(
	SRCS 
		"display.c"
		"../display/src/ST7920_SERIAL.c"
		"../display/src/bitmap.c"
		"../display/src/fonts/font.c"
		"../display/src/fonts/f10x16f.c"
		"../display/src/fonts/f6x8m.c"
	REQUIRES driver
	INCLUDE_DIRS 
	"."
	"../display/src"
	"../display/src/fonts"
	"../keypad16/include"
PRIV_REQUIRES esp-tls esp_wifi esp_timer nvs_flash keypad16
)
