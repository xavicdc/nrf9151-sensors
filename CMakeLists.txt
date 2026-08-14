cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(pio_nrf9151_blinky)

target_sources(app PRIVATE src/main.c src/qmp6988.c src/mqtt_app.c src/credentials.c src/gnss.c src/agnss.c src/mcc_location_table.c)
